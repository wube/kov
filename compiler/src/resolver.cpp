#include "resolver.hpp"
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

void ResolveStats::add(const ResolveStats& o)
{
  typesResolved += o.typesResolved;
  typesDependent += o.typesDependent;
  typesUnresolved += o.typesUnresolved;
  typesBuiltinByName += o.typesBuiltinByName;
  namesLocal += o.namesLocal;
  namesMember += o.namesMember;
  namesGlobal += o.namesGlobal;
  namesDependent += o.namesDependent;
  namesUnresolved += o.namesUnresolved;
  qualifiedUnresolvedStd += o.qualifiedUnresolvedStd;
  unresolvedMacroLike += o.unresolvedMacroLike;
  for (const auto& [symbol, count] : o.unresolvedSymbols)
    unresolvedSymbols[symbol] += count;
  for (const auto& [symbol, where] : o.unresolvedFirst)
    unresolvedFirst.emplace(symbol, where);
  membersBound += o.membersBound;
  membersDependent += o.membersDependent;
  membersUnknownBase += o.membersUnknownBase;
  membersNotFound += o.membersNotFound;
  for (const auto& [symbol, count] : o.missingMembers)
    missingMembers[symbol] += count;
  for (const auto& [symbol, where] : o.missingMembersFirst)
    missingMembersFirst.emplace(symbol, where);
  for (const auto& [symbol, count] : o.unknownBaseMembers)
    unknownBaseMembers[symbol] += count;
  exprsTyped += o.exprsTyped;
  exprsDependent += o.exprsDependent;
  exprsUnknown += o.exprsUnknown;
  unknownName += o.unknownName;
  unknownBase += o.unknownBase;
  unknownMember += o.unknownMember;
  unknownOverload += o.unknownOverload;
  unknownDeduction += o.unknownDeduction;
  unknownAutoReturn += o.unknownAutoReturn;
  unknownUnsupported += o.unknownUnsupported;
  unknownOperator += o.unknownOperator;
  unknownAuto += o.unknownAuto;
  callsResolved += o.callsResolved;
  callsAmbiguousArity += o.callsAmbiguousArity;
  instExprsTyped += o.instExprsTyped;
  instExprsDependent += o.instExprsDependent;
  instExprsUnknown += o.instExprsUnknown;
}

// --- Scope tree ---------------------------------------------------------------------------

enum class ScopeKind : uint8_t { Namespace, Class, Enum };

struct Entry
{
  Decl* decl;
  Scope* scope; // Non-null when the declaration opens a scope (namespace, class or enum definition).
};

struct Scope
{
  ScopeKind kind;
  Scope* parent;
  Decl* owner; // Null for the global namespace; one of possibly several NamespaceDecls otherwise.
  uint32_t name = UINT32_MAX;

  // Symbol table, sharded for the heavily shared namespace scopes.
  struct Shard
  {
    std::mutex mutex;
    std::unordered_map<uint32_t, std::vector<Entry>> map;
  };
  std::unique_ptr<Shard[]> shards;
  uint32_t shardCount;

  // Lazily resolved relations (phase 2).
  std::vector<Type*> baseTypes;          // From the class definition.
  std::vector<Scope*> baseScopes;        // Resolved base class scopes.
  std::atomic<int> baseState{0};         // 0 untouched, 1 resolving, 2 done
  std::thread::id baseResolver;
  std::vector<UsingDirectiveDecl*> usingDirectives;
  std::vector<Scope*> usingScopes;
  std::atomic<int> usingState{0};
  std::vector<UsingNameDecl*> usingEnums; // `using enum E;` inside a class.
  std::vector<Scope*> usingEnumScopes;
  std::atomic<int> usingEnumState{0};
  Span<Decl*> templateParams; // Of the owning class template.
  std::vector<FunctionDecl*> outOfLineMembers; // `void Foo<T>::f() {}` definitions, attached by finishIndex.

  Scope(ScopeKind kind, Scope* parent, Decl* owner, uint32_t shardCount)
    : kind(kind), parent(parent), owner(owner), shards(new Shard[shardCount]), shardCount(shardCount)
  {
  }

  Shard& shardFor(uint32_t symbol) { return shards[symbol % shardCount]; }

  void add(uint32_t symbol, Decl* decl, Scope* scope)
  {
    Shard& shard = shardFor(symbol);
    std::lock_guard lock(shard.mutex);
    shard.map[symbol].push_back({decl, scope});
  }

  // Read-only lookup for phase 2.
  const std::vector<Entry>* find(uint32_t symbol)
  {
    Shard& shard = shardFor(symbol);
    auto it = shard.map.find(symbol);
    return it == shard.map.end() ? nullptr : &it->second;
  }

  // Finds an existing child namespace or creates it (merging all NamespaceDecls of that name).
  Scope* getOrCreateNamespace(uint32_t symbol, Decl* owner)
  {
    Shard& shard = shardFor(symbol);
    std::lock_guard lock(shard.mutex);
    std::vector<Entry>& entries = shard.map[symbol];
    for (const Entry& e : entries)
      if (e.scope && e.scope->kind == ScopeKind::Namespace)
        return e.scope;
    Scope* ns = new Scope(ScopeKind::Namespace, this, owner, 64);
    ns->name = symbol;
    entries.push_back({owner, ns});
    return ns;
  }
};

namespace
{
  // Local declarations inside a function body, template parameter lists and lambdas, kept as a
  // stack that is searched backwards.
  struct LocalStack
  {
    struct Item
    {
      uint32_t symbol;
      Decl* decl;
    };
    std::vector<Item> items;
    size_t mark() const { return items.size(); }
    void pop(size_t mark) { items.resize(mark); }
    void push(uint32_t symbol, Decl* decl)
    {
      if (symbol != UINT32_MAX)
        items.push_back({symbol, decl});
    }
    Decl* find(uint32_t symbol) const
    {
      for (size_t i = items.size(); i-- > 0;)
        if (items[i].symbol == symbol)
          return items[i].decl;
      return nullptr;
    }
  };

  enum class Found : uint8_t { No, Local, Member, Global, Dependent };

  struct Lookup
  {
    Found found = Found::No;
    Decl* decl = nullptr;
    Scope* scope = nullptr;                        // Scope opened by decl, if any.
    const std::vector<Entry>* overloads = nullptr; // All entries with that name in the scope where it was found.
    Scope* foundIn = nullptr;                      // Scope the name was found in.
  };

  // Template parameter -> type, for members seen through an instantiation like Vector<int>.
  struct Subst
  {
    struct Pair
    {
      Decl* param;
      const Ty* type;
    };
    std::vector<Pair> pairs;
    const Ty* find(Decl* param) const
    {
      for (const Pair& p : pairs)
        if (p.param == param)
          return p.type;
      return nullptr;
    }
    bool empty() const { return pairs.empty(); }
  };

  bool isAllCaps(std::string_view s)
  {
    if (s.size() < 2)
      return false;
    bool letter = false;
    for (char c : s)
    {
      if (c >= 'a' && c <= 'z')
        return false;
      if (c >= 'A' && c <= 'Z')
        letter = true;
    }
    return letter;
  }
}

struct GlobalScope::Impl
{
  SymbolTable& symbols;
  TypeTable types;
  Scope root{ScopeKind::Namespace, nullptr, nullptr, 256};
  std::mutex deferredMutex;
  std::vector<std::pair<Decl*, Scope*>> deferredQualified; // Out-of-line class/enum definitions.
  std::vector<std::pair<FunctionDecl*, Scope*>> deferredFunctions; // Out-of-line member function definitions.
  std::vector<std::pair<VariableDecl*, Scope*>> deferredVariables; // Out-of-line static data member definitions.
  std::unordered_map<Decl*, VariableDecl*> variableDefinitions;    // Static data member -> its `T C::x = ...;` definition.
  std::atomic<size_t> scopes{1};

  // ---- template instantiations: each (template, arguments) pair is checked exactly once ----
  struct Instantiation
  {
    std::atomic<int> state{0}; // 0 claimed-not-started is never visible: created in state 1 by the producer.
    std::thread::id owner;
    const Ty* deducedReturn = nullptr; // For function templates with `auto` return.
  };
  struct InstShard
  {
    std::mutex mutex;
    std::unordered_map<const Ty*, std::unique_ptr<Instantiation>> classes;
    std::unordered_map<std::string, std::unique_ptr<Instantiation>> functions; // key: decl + arg pointers
  };
  static constexpr uint32_t instShardCount = 64;
  InstShard instShards[instShardCount];
  std::atomic<size_t> instClasses{0}, instFunctions{0}, instBodies{0}, instRequests{0}, instWaits{0};
  std::atomic<uint64_t> instNanos{0};

  // Returns the record and whether the caller just became its producer.
  Instantiation* claimClass(const Ty* classTy, bool& producer)
  {
    ++instRequests;
    InstShard& shard = instShards[(reinterpret_cast<uintptr_t>(classTy) >> 4) % instShardCount];
    std::lock_guard lock(shard.mutex);
    auto it = shard.classes.find(classTy);
    if (it != shard.classes.end())
    {
      producer = false;
      return it->second.get();
    }
    auto inst = std::make_unique<Instantiation>();
    inst->state.store(1);
    inst->owner = std::this_thread::get_id();
    Instantiation* result = inst.get();
    shard.classes.emplace(classTy, std::move(inst));
    producer = true;
    ++instClasses;
    return result;
  }

  Instantiation* claimFunction(FunctionDecl* f, const Subst& subst, bool& producer)
  {
    ++instRequests;
    std::string key(reinterpret_cast<const char*>(&f), sizeof(f));
    for (const Subst::Pair& pair : subst.pairs)
    {
      key.append(reinterpret_cast<const char*>(&pair.param), sizeof(pair.param));
      key.append(reinterpret_cast<const char*>(&pair.type), sizeof(pair.type));
    }
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : key)
      h = (h ^ c) * 1099511628211ull;
    InstShard& shard = instShards[h % instShardCount];
    std::lock_guard lock(shard.mutex);
    auto it = shard.functions.find(key);
    if (it != shard.functions.end())
    {
      producer = false;
      return it->second.get();
    }
    auto inst = std::make_unique<Instantiation>();
    inst->state.store(1);
    inst->owner = std::this_thread::get_id();
    Instantiation* result = inst.get();
    shard.functions.emplace(std::move(key), std::move(inst));
    producer = true;
    ++instFunctions;
    return result;
  }

  std::atomic<size_t> entries{0};
  uint32_t symStd;
  uint32_t opSymbols[size_t(P::Count)]; // "operator+" etc., so operators can be looked up by name.
  uint32_t symBegin, symFirst, symSecond;
  struct BuiltinName
  {
    uint32_t symbol;
    const Ty* type;
  };
  std::vector<BuiltinName> builtinNames; // uint32_t, size_t... which come from headers outside the corpus.

  explicit Impl(SymbolTable& symbols) : symbols(symbols)
  {
    symStd = symbols.intern("std");
    symBegin = symbols.intern("begin");
    symFirst = symbols.intern("first");
    symSecond = symbols.intern("second");
    for (uint32_t i = 0; i < uint32_t(P::Count); ++i)
      opSymbols[i] = symbols.intern("operator" + std::string(punctuatorSpelling(P(i))));
    auto add = [&](const char* name, const Ty* t) { builtinNames.push_back({symbols.intern(name), t}); };
    add("int8_t", types.intType(8));
    add("i8", types.intType(8));
    add("i16", types.intType(16));
    add("i32", types.intType(32));
    add("i64", types.intType(64));
    add("u8", types.intType(8, true));
    add("u16", types.intType(16, true));
    add("u32", types.intType(32, true));
    add("u64", types.intType(64, true));
    add("usize", types.sizeType());
    add("int16_t", types.intType(16));
    add("int32_t", types.intType(32));
    add("int64_t", types.intType(64));
    add("uint8_t", types.intType(8, true));
    add("uint16_t", types.intType(16, true));
    add("uint32_t", types.intType(32, true));
    add("uint64_t", types.intType(64, true));
    add("size_t", types.sizeType());
    add("ptrdiff_t", types.ptrdiffType());
    add("intptr_t", types.intType(64));
    add("uintptr_t", types.intType(64, true));
    add("__int64", types.intType(64));
    add("__int32", types.intType(32));
    add("__int16", types.intType(16));
    add("__int8", types.intType(8));
    add("ssize_t", types.ptrdiffType());
    add("int_fast32_t", types.intType(32));
    add("uint_fast32_t", types.intType(32, true));
    add("nullptr_t", types.nullptrType());
    add("_Bool", types.boolType());
    add("wchar_t", types.charType(16, true));
  }

  const Ty* builtinByName(uint32_t symbol) const
  {
    for (const BuiltinName& b : builtinNames)
      if (b.symbol == symbol)
        return b.type;
    return nullptr;
  }

  // ---------------- phase 1: indexing ----------------

  static uint32_t simpleName(const QualifiedName& name)
  {
    return name.parts.size() == 1 && name.parts[0].special == 0 ? name.parts[0].symbol : UINT32_MAX;
  }

  // Symbol under which a (possibly operator) function is indexed, or UINT32_MAX.
  // The symbol a function is known by: its own name, the last part of `Class<T>::name`.
  uint32_t functionSymbol(const QualifiedName& name) const
  {
    const NamePart& part = name.parts[name.parts.size() - 1];
    if (part.special == 0)
      return part.symbol;
    if (part.special == 2 && part.opToken < uint32_t(P::Count))
      return opSymbols[part.opToken];
    return UINT32_MAX;
  }

  void indexDecls(Span<Decl*> decls, Scope* scope)
  {
    for (Decl* d : decls)
      indexDecl(d, scope);
  }

  void indexDecl(Decl* d, Scope* scope)
  {
    switch (d->kind)
    {
      case NodeKind::NamespaceDecl:
      {
        auto* ns = static_cast<NamespaceDecl*>(d);
        Scope* target = scope;
        if (ns->name != UINT32_MAX)
        {
          for (const NamePart& part : ns->nested.parts)
          {
            target = target->getOrCreateNamespace(part.symbol, ns);
            if (target->owner == ns)
              ++scopes;
          }
        }
        // Anonymous and inline namespaces are flattened into the parent.
        ns->ownScope = target;
        indexDecls(ns->decls, target);
        break;
      }
      case NodeKind::ClassDecl:
      {
        auto* c = static_cast<ClassDecl*>(d);
        if (c->name.parts.size() > 1)
        {
          // `struct A::B { ... }`: attach once A exists.
          if (c->isDefinition)
          {
            std::lock_guard lock(deferredMutex);
            deferredQualified.push_back({c, scope});
          }
          break;
        }
        Scope* own = nullptr;
        if (c->isDefinition)
        {
          own = new Scope(ScopeKind::Class, scope, c, 1);
          own->templateParams = c->templateParams;
          for (const ClassDecl::Base& b : c->bases)
            own->baseTypes.push_back(b.type);
          c->ownScope = own;
          ++scopes;
          indexDecls(c->members, own);
        }
        if (c->name.empty())
        {
          // Anonymous struct/union: its members belong to the enclosing scope.
          if (own)
            indexDecls(c->members, scope);
        }
        else
        {
          scope->add(c->name.parts[0].symbol, c, own);
          ++entries;
        }
        break;
      }
      case NodeKind::EnumDecl:
      {
        auto* e = static_cast<EnumDecl*>(d);
        if (e->name.parts.size() > 1)
        {
          if (e->isDefinition)
          {
            std::lock_guard lock(deferredMutex);
            deferredQualified.push_back({e, scope});
          }
          break;
        }
        Scope* own = nullptr;
        if (e->isDefinition)
        {
          own = new Scope(ScopeKind::Enum, scope, e, 1);
          e->ownScope = own;
          ++scopes;
          for (Decl* en : e->enumerators)
            if (en->kind == NodeKind::EnumeratorDecl)
            {
              uint32_t name = static_cast<EnumeratorDecl*>(en)->name;
              own->add(name, en, nullptr);
              ++entries;
              if (!e->isClass)
                scope->add(name, en, nullptr); // Unscoped enumerators leak into the enclosing scope.
            }
          indexDecls(e->members, own);
        }
        if (!e->name.empty())
        {
          scope->add(e->name.parts[0].symbol, e, own);
          ++entries;
        }
        break;
      }
      case NodeKind::FunctionDecl:
      {
        auto* f = static_cast<FunctionDecl*>(d);
        // Out-of-line definitions (`void A::f()`) are not new names. Constructors/destructors
        // are reachable through the class name.
        if (f->name.parts.size() == 1 && !(f->flags & (DF_Constructor | DF_Destructor | DF_Conversion)) &&
            !(f->flags & DF_Friend))
        {
          uint32_t symbol = functionSymbol(f->name);
          if (symbol != UINT32_MAX)
          {
            scope->add(symbol, f, nullptr);
            ++entries;
          }
        }
        else if (f->name.parts.size() > 1 && (f->body || (f->flags & (DF_Default | DF_Delete))))
        {
          // An out-of-line definition, `Flow::~Flow() = default;` included: attached to its class by finishIndex.
          std::lock_guard lock(deferredMutex);
          deferredFunctions.push_back({f, scope});
        }
        break;
      }
      case NodeKind::VariableDecl:
      {
        auto* v = static_cast<VariableDecl*>(d);
        if (v->name.parts.size() == 1)
        {
          uint32_t symbol = simpleName(v->name);
          if (symbol != UINT32_MAX)
          {
            scope->add(symbol, v, nullptr);
            ++entries;
          }
        }
        else if (v->name.parts.size() > 1)
        {
          // `const Style& Widget::defaultWidgetStyle = Widget::getStyleStatic();`: the definition
          // of a static data member, attached to its declaration by finishIndex.
          std::lock_guard lock(deferredMutex);
          deferredVariables.push_back({v, scope});
        }
        for (Expr* b : v->structuredBindings)
        {
          scope->add(simpleName(static_cast<NameExpr*>(b)->name), v, nullptr);
          ++entries;
        }
        break;
      }
      case NodeKind::UsingAliasDecl:
      {
        auto* u = static_cast<UsingAliasDecl*>(d);
        scope->add(u->name, u, nullptr);
        ++entries;
        break;
      }
      case NodeKind::UsingNameDecl:
      {
        auto* u = static_cast<UsingNameDecl*>(d);
        if (u->isEnum)
        {
          std::lock_guard lock(scope->shards[0].mutex);
          scope->usingEnums.push_back(u);
        }
        else if (!u->name.empty() && u->name.last().special == 0)
        {
          scope->add(u->name.last().symbol, u, nullptr);
          ++entries;
        }
        break;
      }
      case NodeKind::UsingDirectiveDecl:
      {
        std::lock_guard lock(scope->shards[0].mutex);
        scope->usingDirectives.push_back(static_cast<UsingDirectiveDecl*>(d));
        break;
      }
      default:
        break;
    }
  }

  // ---------------- phase 2: lookup ----------------

  // Entries that open a scope are preferred (definition over forward declaration, primary
  // template over specialisation).
  static Entry pick(const std::vector<Entry>& entries)
  {
    const Entry* best = &entries[0];
    for (const Entry& e : entries)
    {
      bool primary = !(e.decl->flags & DF_Specialization);
      bool bestPrimary = !(best->decl->flags & DF_Specialization);
      if ((e.scope && !best->scope) || (e.scope && best->scope && primary && !bestPrimary))
        best = &e;
    }
    return *best;
  }

  Scope* lexicalScopeOf(Decl* d)
  {
    for (Decl* p = d ? d->parent : nullptr; p; p = p->parent)
      if (p->ownScope)
        return p->ownScope;
    return &root;
  }

  // Resolves a type to the scope it denotes (class/enum/namespace scope), following aliases.
  Scope* scopeOfType(Type* type, Scope* context, const LocalStack* locals, int depth)
  {
    if (!type || depth > 16)
      return nullptr;
    switch (type->kind)
    {
      case NodeKind::ConstType: return scopeOfType(static_cast<ConstType*>(type)->inner, context, locals, depth + 1);
      case NodeKind::PointerType: return scopeOfType(static_cast<PointerType*>(type)->pointee, context, locals, depth + 1);
      case NodeKind::LRefType: return scopeOfType(static_cast<LRefType*>(type)->referenced, context, locals, depth + 1);
      case NodeKind::RRefType: return scopeOfType(static_cast<RRefType*>(type)->referenced, context, locals, depth + 1);
      case NodeKind::NamedType:
      {
        Lookup l = lookupQualified(static_cast<NamedType*>(type)->name, context, locals, depth + 1);
        return l.scope;
      }
      default:
        return nullptr;
    }
  }

  Scope* scopeOfEntry(const Entry& e, int depth)
  {
    if (e.scope)
      return e.scope;
    Decl* d = e.decl;
    if (d->kind == NodeKind::UsingAliasDecl)
      return scopeOfType(static_cast<UsingAliasDecl*>(d)->type, lexicalScopeOf(d), nullptr, depth + 1);
    if (d->kind == NodeKind::VariableDecl && (d->flags & DF_Typedef))
      return scopeOfType(static_cast<VariableDecl*>(d)->type, lexicalScopeOf(d), nullptr, depth + 1);
    if (d->kind == NodeKind::UsingNameDecl)
    {
      Lookup l = lookupQualified(static_cast<UsingNameDecl*>(d)->name, lexicalScopeOf(d), nullptr, depth + 1);
      return l.scope;
    }
    if (d->kind == NodeKind::ClassDecl || d->kind == NodeKind::EnumDecl)
    {
      // Forward declaration: find the definition through the lexical scope.
      Scope* s = lexicalScopeOf(d);
      uint32_t symbol = d->kind == NodeKind::ClassDecl ? static_cast<ClassDecl*>(d)->name.parts[0].symbol
                                                       : static_cast<EnumDecl*>(d)->name.parts[0].symbol;
      if (const std::vector<Entry>* entries = s->find(symbol))
      {
        Entry best = pick(*entries);
        if (best.scope)
          return best.scope;
      }
    }
    return nullptr;
  }

  void ensureBases(Scope* cls, int depth)
  {
    int state = cls->baseState.load(std::memory_order_acquire);
    if (state == 2)
      return;
    if (state == 0 && cls->baseState.compare_exchange_strong(state, 1))
    {
      cls->baseResolver = std::this_thread::get_id();
      std::vector<Scope*> bases;
      LocalStack params;
      for (Decl* p : cls->templateParams)
        params.push(static_cast<TemplateParamDecl*>(p)->name, p);
      for (Type* t : cls->baseTypes)
        if (Scope* s = scopeOfType(t, cls->parent, &params, depth + 1))
          if (s != cls)
            bases.push_back(s);
      cls->baseScopes = std::move(bases);
      cls->baseState.store(2, std::memory_order_release);
      return;
    }
    // Another thread is resolving; if it is us (a cycle through aliases), give up on bases.
    if (cls->baseResolver == std::this_thread::get_id())
      return;
    while (cls->baseState.load(std::memory_order_acquire) != 2)
      std::this_thread::yield();
  }

  void ensureUsing(Scope* ns, int depth)
  {
    int state = ns->usingState.load(std::memory_order_acquire);
    if (state == 2)
      return;
    if (state == 0 && ns->usingState.compare_exchange_strong(state, 1))
    {
      std::vector<Scope*> scopes;
      for (UsingDirectiveDecl* u : ns->usingDirectives)
      {
        Lookup l = lookupQualified(u->name, lexicalScopeOf(u), nullptr, depth + 1);
        if (l.scope && l.scope != ns)
          scopes.push_back(l.scope);
      }
      ns->usingScopes = std::move(scopes);
      ns->usingState.store(2, std::memory_order_release);
      return;
    }
    // Being resolved elsewhere: proceed without using-directives rather than risk a deadlock.
  }

  void ensureUsingEnums(Scope* cls, int depth)
  {
    int state = cls->usingEnumState.load(std::memory_order_acquire);
    if (state == 2)
      return;
    if (state == 0 && cls->usingEnumState.compare_exchange_strong(state, 1))
    {
      std::vector<Scope*> scopes;
      for (UsingNameDecl* u : cls->usingEnums)
      {
        Lookup l = lookupQualified(u->name, cls, nullptr, depth + 1);
        if (l.scope)
          scopes.push_back(l.scope);
      }
      cls->usingEnumScopes = std::move(scopes);
      cls->usingEnumState.store(2, std::memory_order_release);
    }
  }

  // Looks a symbol up in one scope: its own entries, template parameters, using-enums, bases.
  bool lookupInScope(Scope* s, uint32_t symbol, Lookup& out, int depth, bool withUsing = true)
  {
    if (depth > 24)
      return false;
    if (const std::vector<Entry>* entries = s->find(symbol))
    {
      Entry e = pick(*entries);
      out.decl = e.decl;
      out.scope = e.scope ? e.scope : scopeOfEntry(e, depth + 1);
      out.found = s->kind == ScopeKind::Namespace ? Found::Global : Found::Member;
      out.overloads = entries;
      out.foundIn = s;
      return true;
    }
    if (s->kind == ScopeKind::Class)
    {
      for (Decl* p : s->templateParams)
        if (static_cast<TemplateParamDecl*>(p)->name == symbol)
        {
          out.decl = p;
          out.found = Found::Dependent;
          return true;
        }
      if (!s->usingEnums.empty())
      {
        ensureUsingEnums(s, depth);
        for (Scope* e : s->usingEnumScopes)
          if (lookupInScope(e, symbol, out, depth + 1))
            return true;
      }
      if (!s->baseTypes.empty())
      {
        ensureBases(s, depth);
        for (Scope* b : s->baseScopes)
          if (lookupInScope(b, symbol, out, depth + 1))
            return true;
      }
    }
    else if (s->kind == ScopeKind::Namespace && withUsing && !s->usingDirectives.empty())
    {
      ensureUsing(s, depth);
      for (Scope* u : s->usingScopes)
        if (lookupInScope(u, symbol, out, depth + 1, false))
          return true;
    }
    return false;
  }

  Lookup lookupUnqualified(uint32_t symbol, Scope* context, const LocalStack* locals, int depth)
  {
    Lookup out;
    if (locals)
      if (Decl* d = locals->find(symbol))
      {
        out.decl = d;
        out.found = d->kind == NodeKind::TemplateParamDecl ? Found::Dependent : Found::Local;
        if (d->ownScope)
          out.scope = d->ownScope;
        return out;
      }
    for (Scope* s = context; s; s = s->parent)
      if (lookupInScope(s, symbol, out, depth))
        return out;
    return out;
  }

  // Among the entries a class template's name resolved to, the full specialisation whose
  // arguments are exactly `args`; null when there is none or an argument is still dependent.
  Decl* matchSpecialization(const Lookup& l, const std::vector<const Ty*>& args, int depth)
  {
    if (!l.overloads || args.empty() || depth > 24)
      return nullptr;
    for (const Ty* a : args)
      if (!a || !a->isKnown() || types.isDependent(a))
        return nullptr;
    for (const Entry& e : *l.overloads)
    {
      if (!e.decl || e.decl->kind != NodeKind::ClassDecl || !(e.decl->flags & DF_Specialization))
        continue;
      auto* c = static_cast<ClassDecl*>(e.decl);
      if (!c->isDefinition || c->name.empty())
        continue;
      const NamePart& part = c->name.last();
      if (part.templateArgs.size() != args.size())
        continue;
      TypeContext sctx{lexicalScopeOf(c), nullptr, nullptr, c->file ? c->file->tokens : nullptr, c->file ? c->file->text : std::string_view()};
      bool same = true;
      for (size_t i = 0; i < args.size() && same; ++i)
      {
        Node* a = part.templateArgs[i];
        Decl* param = l.decl && i < l.decl->templateParams.size() ? l.decl->templateParams[i] : nullptr;
        same = templateArgument(a, param, sctx, depth) == args[i];
      }
      if (same)
        return c;
    }
    return nullptr;
  }

  Lookup lookupQualified(const QualifiedName& name, Scope* context, const LocalStack* locals, int depth, const Subst* subst = nullptr)
  {
    Lookup out;
    if (name.empty() || depth > 24)
      return out;
    Scope* current = name.global ? &root : nullptr;
    for (size_t i = 0; i < name.parts.size(); ++i)
    {
      const NamePart& part = name.parts[i];
      bool last = i + 1 == name.parts.size();
      if (part.special == 1)
      {
        // Destructor name: resolves to the class itself.
        if (!last)
          return {};
        if (current)
          lookupInScope(current, part.symbol, out, depth);
        else
          out = lookupUnqualified(part.symbol, context, locals, depth);
        return out;
      }
      if (part.special == 2 && part.opToken < uint32_t(P::Count))
      {
        if (!last)
          return {};
        uint32_t symbol = opSymbols[part.opToken];
        if (current)
          lookupInScope(current, symbol, out, depth);
        else
          out = lookupUnqualified(symbol, context, locals, depth);
        return out;
      }
      if (part.special != 0 || part.symbol == UINT32_MAX)
      {
        // conversion operators and `decltype(...)` components: the class scope is the result.
        if (current)
        {
          out.found = current->kind == ScopeKind::Namespace ? Found::Global : Found::Member;
          out.decl = current->owner;
          out.scope = nullptr;
          return out;
        }
        return {};
      }
      Lookup step;
      if (current)
      {
        if (!lookupInScope(current, part.symbol, step, depth))
          return {};
      }
      else
        step = lookupUnqualified(part.symbol, context, locals, depth);
      if (step.found == Found::No)
        return {};
      if (step.found == Found::Dependent)
      {
        if (!last && subst && step.decl && step.decl->kind == NodeKind::TemplateParamDecl)
          if (const Ty* sub = subst->find(step.decl); sub && sub->kind == TyKind::Class && sub->decl)
            if (Scope* inside = classScopeOf(sub))
            {
              current = inside; // `Policy::getCoordinate` with Policy = VerticalPolicy: the rest inside that class
              continue;
            }
        out = step;
        out.found = Found::Dependent;
        return out; // `T::type`: dependent on a template parameter.
      }
      if (last)
        return step;
      if (part.hasTemplateArgs && step.decl && step.decl->kind == NodeKind::ClassDecl)
      {
        // `Limits<int>::max`: the specialisation for these arguments, when there is one.
        std::vector<const Ty*> args;
        TypeContext actx{context, locals, subst};
        for (Node* a : part.templateArgs)
          args.push_back(uint8_t(a->kind) <= uint8_t(NodeKind::PackType) ? convertType(static_cast<Type*>(a), actx, depth + 1) : types.unknown());
        if (Decl* spec = matchSpecialization(step, args, depth))
        {
          step.decl = spec;
          step.scope = spec->ownScope;
        }
      }
      if (!step.scope)
      {
        // A variable/function used as a qualifier, or an alias we could not follow.
        return {};
      }
      current = step.scope;
      out = step;
    }
    return out;
  }

  // ---------------- phase 2: types ----------------

  struct TypeContext
  {
    Scope* scope;
    const LocalStack* locals;
    const Subst* subst;
    const std::vector<Token>* tokens = nullptr; // For evaluating array bounds written as literals.
    std::string_view text;
  };

  // --- builtin type traits ---

  static bool isTypeNode(Node* n)
  {
    switch (n->kind)
    {
      case NodeKind::NamedType: case NodeKind::PointerType: case NodeKind::LRefType: case NodeKind::RRefType: case NodeKind::ConstType:
      case NodeKind::ArrayType: case NodeKind::FunctionType: case NodeKind::DecltypeType: case NodeKind::AutoType: case NodeKind::BuiltinType:
      case NodeKind::MemberPointerType: case NodeKind::PackType:
        return true;
      default: return false;
    }
  }

  // The argument types of a trait, or empty when one is not a type or is still dependent.
  std::vector<const Ty*> traitArgs(const NamePart& part, const TypeContext& ctx, int depth)
  {
    std::vector<const Ty*> args;
    for (Node* a : part.templateArgs)
    {
      if (!isTypeNode(a))
        return {};
      const Ty* t = convertType(static_cast<Type*>(a), ctx, depth + 1);
      if (!t || !t->isKnown() || types.isDependent(t))
        return {};
      args.push_back(t);
    }
    return args;
  }

  const Ty* enumUnderlyingType(const Ty* e, const TypeContext& ctx, int depth)
  {
    Decl* def = e->decl;
    if (def && def->kind == NodeKind::EnumDecl && static_cast<EnumDecl*>(def)->underlying)
      return convertType(static_cast<EnumDecl*>(def)->underlying, ctx, depth + 1);
    return types.intType();
  }

  bool isTypeTraitName(uint32_t symbol)
  {
    std::string_view name = symbols.name(symbol);
    return name == "RemoveConst" || name == "RemoveCv" || name == "RemoveReference" || name == "RemoveExtent" || name == "RemovePointer" ||
           name == "Decay" || name == "UnderlyingType" || name == "AddConst" || name == "AddPointer" || name == "RemoveCvRef";
  }

  // RemoveConst<T> and the other type-valued traits; null when `part` is not one.
  const Ty* typeTrait(const NamePart& part, const TypeContext& ctx, int depth)
  {
    std::string_view name = symbols.name(part.symbol);
    if (!isTypeTraitName(part.symbol))
      return nullptr;
    if (part.templateArgs.size() != 1 || !isTypeNode(part.templateArgs[0]))
      return types.unknown();
    const Ty* t = convertType(static_cast<Type*>(part.templateArgs[0]), ctx, depth + 1);
    if (!t || !t->isKnown() || types.isDependent(t))
      return t; // settled when the instantiation substitutes the argument
    if (name == "RemoveConst" || name == "RemoveCv")
      return types.unqualified(t);
    if (name == "RemoveReference")
      return types.stripRef(t);
    if (name == "RemoveCvRef")
      return types.unqualified(types.stripRef(t));
    if (name == "RemoveExtent")
      return t->kind == TyKind::Array ? t->inner : t;
    if (name == "RemovePointer")
      return t->kind == TyKind::Pointer ? t->inner : t;
    if (name == "AddConst")
      return types.qualified(t, true, false);
    if (name == "AddPointer")
      return types.pointer(types.stripRef(t));
    if (name == "Decay")
      return types.decay(t);
    if (name == "UnderlyingType")
      return t->kind == TyKind::Enum ? enumUnderlyingType(t, ctx, depth) : t;
    return nullptr;
  }

  static bool isArithmeticKind(const Ty* t)
  {
    return t->kind == TyKind::Int || t->kind == TyKind::Char || t->kind == TyKind::Bool || t->kind == TyKind::Float || t->kind == TyKind::Enum;
  }

  // Whether `derived` is `base` or has it among its bases (classes only).
  bool derivesFrom(const Ty* derived, const Ty* base, int depth = 0)
  {
    if (!derived || !base || depth > 16)
      return false;
    if (derived->kind != TyKind::Class || base->kind != TyKind::Class)
      return false;
    if (derived->decl == base->decl)
      return true;
    Decl* def = derived->decl;
    if (def && def->kind == NodeKind::ClassDecl && !def->ownScope)
    {
      Entry e{def, nullptr};
      Scope* s = scopeOfEntry(e, 0);
      def = s ? s->owner : def;
    }
    if (!def || def->kind != NodeKind::ClassDecl)
      return false;
    DeclContext dc;
    declContextFor(def, dc);
    for (const ClassDecl::Base& b : static_cast<ClassDecl*>(def)->bases)
    {
      const Ty* bt = convertType(b.type, TypeContext{dc.scope, &dc.params, nullptr, def->file ? def->file->tokens : nullptr,
                                                     def->file ? def->file->text : std::string_view()}, 0);
      if (bt && derivesFrom(bt, base, depth + 1))
        return true;
    }
    return false;
  }

  // isArray<T>, isSame<A, B>, ... ; nullopt when `name` is not a trait.
  std::optional<bool> traitValue(std::string_view name, const std::vector<const Ty*>& a)
  {
    auto one = [&]() { return a.size() == 1 ? a[0] : nullptr; };
    if (name == "isArray") return one() && one()->kind == TyKind::Array;
    if (name == "isPointer") return one() && one()->kind == TyKind::Pointer;
    if (name == "isEnum") return one() && one()->kind == TyKind::Enum;
    if (name == "isClass") return one() && one()->kind == TyKind::Class;
    if (name == "isVoid") return one() && one()->kind == TyKind::Void;
    if (name == "isReference") return one() && one()->isReference();
    if (name == "isConst") return one() && one()->isConst;
    if (name == "isNullPointer") return one() && one() == types.nullptrType();
    if (name == "isIntegral") return one() && (one()->kind == TyKind::Int || one()->kind == TyKind::Char || one()->kind == TyKind::Bool);
    if (name == "isFloatingPoint") return one() && one()->kind == TyKind::Float;
    if (name == "isArithmetic") return one() && isArithmeticKind(one()) && one()->kind != TyKind::Enum;
    if (name == "isSigned")
      return one() && (one()->kind == TyKind::Float || ((one()->kind == TyKind::Int || one()->kind == TyKind::Char) && !one()->isUnsigned));
    if (name == "isUnsigned")
      return one() && ((one()->kind == TyKind::Int || one()->kind == TyKind::Char || one()->kind == TyKind::Bool) && (one()->isUnsigned || one()->kind == TyKind::Bool));
    if (name == "isFunction") return one() && one()->kind == TyKind::Function;
    if (name == "isTriviallyCopyable")
    {
      const Ty* t = one();
      if (!t)
        return false;
      if (t->kind != TyKind::Class)
        return true;
      Decl* def = t->decl;
      if (!def || def->kind != NodeKind::ClassDecl)
        return false;
      for (Decl* m : static_cast<ClassDecl*>(def)->members)
        if (m->kind == NodeKind::FunctionDecl && (m->flags & (DF_Destructor | DF_Virtual)))
          return false;
      return true;
    }
    if (name == "isSame") return a.size() == 2 && a[0] == a[1];
    if (name == "isBaseOf") return a.size() == 2 && derivesFrom(a[1], a[0]);
    if (name == "isConvertible")
    {
      if (a.size() != 2)
        return false;
      const Ty* from = types.unqualified(types.stripRef(a[0]));
      const Ty* to = types.unqualified(types.stripRef(a[1]));
      if (from == to)
        return true;
      if (isArithmeticKind(from) && isArithmeticKind(to) && to->kind != TyKind::Enum)
        return true;
      if (from == types.nullptrType() && to->kind == TyKind::Pointer)
        return true;
      if (from->kind == TyKind::Pointer && to->kind == TyKind::Pointer)
      {
        const Ty* fp = types.unqualified(from->inner);
        const Ty* tp = types.unqualified(to->inner);
        return tp->kind == TyKind::Void || fp == tp || derivesFrom(fp, tp);
      }
      return false;
    }
    return std::nullopt;
  }

  bool isBoolTraitName(uint32_t symbol)
  {
    std::string_view n = symbols.name(symbol);
    return n == "isArray" || n == "isPointer" || n == "isEnum" || n == "isClass" || n == "isVoid" || n == "isReference" || n == "isConst" ||
           n == "isNullPointer" || n == "isIntegral" || n == "isFloatingPoint" || n == "isArithmetic" || n == "isSigned" ||
           n == "isUnsigned" || n == "isFunction" || n == "isTriviallyCopyable" || n == "isSame" || n == "isBaseOf" || n == "isConvertible";
  }

  // Minimal constant evaluation for array bounds: literals and arithmetic on them.
  // The value of a declaration used as a constant: a const/constexpr variable with an
  // initializer, an enumerator, or a non-type template parameter under the substitution.
  std::optional<int64_t> constOfDecl(Decl* d, const TypeContext& ctx, int depth)
  {
    if (!d || depth > 16)
      return std::nullopt;
    if (d->kind == NodeKind::TemplateParamDecl)
    {
      if (ctx.subst)
        if (const Ty* t = ctx.subst->find(d); t && t->kind == TyKind::Value)
          return t->value;
      return std::nullopt;
    }
    if (d->kind == NodeKind::VariableDecl)
    {
      auto* v = static_cast<VariableDecl*>(d);
      VariableDecl* def = v; // `static const int LIMIT;` in the class, `const int Gui::LIMIT = 21;` outside
      if (!v->init)
        if (auto it = variableDefinitions.find(v); it != variableDefinitions.end())
          def = it->second;
      if (!def->init)
        return std::nullopt;
      TypeContext vctx{lexicalScopeOf(v), nullptr, nullptr, v->file ? v->file->tokens : nullptr, v->file ? v->file->text : std::string_view()};
      const Ty* t = convertType(v->type, vctx, depth + 1);
      if (!(v->flags & DF_Constexpr) && !(t && t->isConst))
        return std::nullopt;
      // The definition's initializer, read from its file, evaluated where the declaration is (the class).
      TypeContext ictx{lexicalScopeOf(v), nullptr, nullptr, def->file ? def->file->tokens : nullptr, def->file ? def->file->text : std::string_view()};
      return constEval(def->init, ictx, depth + 1);
    }
    if (d->kind == NodeKind::EnumeratorDecl)
    {
      // Enumerators count up from zero, or from the last written value.
      auto* e = static_cast<EnumDecl*>(d->parent);
      if (!e || e->kind != NodeKind::EnumDecl)
        return std::nullopt;
      TypeContext ectx{lexicalScopeOf(e), nullptr, nullptr, e->file ? e->file->tokens : nullptr, e->file ? e->file->text : std::string_view()};
      int64_t next = 0;
      for (Decl* m : e->enumerators)
      {
        if (m->kind != NodeKind::EnumeratorDecl)
          continue;
        auto* cur = static_cast<EnumeratorDecl*>(m);
        if (cur->value)
        {
          std::optional<int64_t> v = constEval(cur->value, ectx, depth + 1);
          if (!v)
            return std::nullopt;
          next = *v;
        }
        if (cur == d)
          return next;
        ++next;
      }
    }
    return std::nullopt;
  }

  std::optional<int64_t> constEval(Expr* e, const TypeContext& ctx, int depth = 0)
  {
    if (!e || depth > 16)
      return std::nullopt;
    switch (e->kind)
    {
      case NodeKind::NameExpr:
      {
        auto* n = static_cast<NameExpr*>(e);
        Lookup l = lookupQualified(n->name, ctx.scope, ctx.locals, depth + 1, ctx.subst);
        return constOfDecl(l.decl, ctx, depth);
      }
      case NodeKind::MemberExpr:
      {
        // `Enum::Value` is parsed as a name; `object.member` is never constant.
        return std::nullopt;
      }
      case NodeKind::LiteralExpr:
      {
        auto* l = static_cast<LiteralExpr*>(e);
        if (l->literalKind == TokenKind::Keyword)
          return l->keyword == Kw::true_ ? 1 : l->keyword == Kw::false_ ? 0 : std::optional<int64_t>();
        if (l->literalKind != TokenKind::Number || !ctx.tokens)
          return std::nullopt;
        const Token& t = (*ctx.tokens)[std::min<size_t>(l->token, ctx.tokens->size() - 1)];
        std::string digits;
        for (char c : ctx.text.substr(t.offset, t.length))
          if (c != '\'')
            digits += c;
        try
        {
          if (digits.size() > 2 && digits[0] == '0' && (digits[1] == 'b' || digits[1] == 'B'))
            return std::stoll(digits.substr(2), nullptr, 2); // 0b1010
          return std::stoll(digits, nullptr, 0);
        }
        catch (...)
        {
          return std::nullopt;
        }
      }
      case NodeKind::ParenExpr: return constEval(static_cast<ParenExpr*>(e)->inner, ctx, depth + 1);
      case NodeKind::UnaryExpr:
      {
        auto* u = static_cast<UnaryExpr*>(e);
        auto v = constEval(u->operand, ctx, depth + 1);
        if (!v || u->isKeyword)
          return std::nullopt;
        return P(u->op) == P::Minus ? -*v : P(u->op) == P::Plus ? *v : std::optional<int64_t>();
      }
      case NodeKind::BinaryExpr:
      {
        auto* b = static_cast<BinaryExpr*>(e);
        auto l = constEval(b->lhs, ctx, depth + 1), r = constEval(b->rhs, ctx, depth + 1);
        if (!l || !r || b->isKeyword)
          return std::nullopt;
        switch (P(b->op))
        {
          case P::Plus: return *l + *r;
          case P::Minus: return *l - *r;
          case P::Star: return *l * *r;
          case P::Slash: return *r ? *l / *r : std::optional<int64_t>();
          case P::Percent: return *r ? *l % *r : std::optional<int64_t>();
          case P::Shl: return *l << *r;
          case P::Shr: return *l >> *r;
          default: return std::nullopt;
        }
      }
      default:
        return std::nullopt;
    }
  }

  static Decl* enclosingClassDecl(Scope* s)
  {
    for (; s; s = s->parent)
      if (s->kind == ScopeKind::Class || s->kind == ScopeKind::Enum)
        return s->owner;
    return nullptr;
  }

  static size_t enumeratorCount(Decl* e)
  {
    size_t n = 0;
    for (Decl* d : static_cast<EnumDecl*>(e)->enumerators)
      n += d->kind == NodeKind::EnumeratorDecl;
    return n;
  }

  // Type of a class or enum declaration as written inside it: template parameters stay dependent.
  const Ty* selfType(Decl* d)
  {
    if (d->kind == NodeKind::EnumDecl)
      return types.enumType(d);
    std::vector<const Ty*> args;
    for (Decl* p : d->templateParams)
      args.push_back(types.param(p));
    return types.classType(d, Span<const Ty*>{args.data(), uint32_t(args.size())});
  }

  // Maps the template parameters of `cls` to the arguments of an instantiation type.
  Subst substFor(const Ty* classTy)
  {
    Subst s;
    if (!classTy || classTy->kind != TyKind::Class || !classTy->decl)
      return s;
    Span<Decl*> params = classTy->decl->templateParams;
    for (size_t i = 0; i < params.size() && i < classTy->args.size(); ++i)
    {
      if (params[i]->kind == NodeKind::TemplateParamDecl && static_cast<TemplateParamDecl*>(params[i])->isPack)
      {
        // `template<typename R, typename... Args>`: the pack takes the rest of the arguments.
        std::vector<const Ty*> rest(classTy->args.begin() + i, classTy->args.end());
        if (rest.size() == 1 && rest[0]->kind == TyKind::Pack)
          s.pairs.push_back({params[i], rest[0]});
        else
          s.pairs.push_back({params[i], types.pack(Span<const Ty*>{rest.data(), uint32_t(rest.size())})});
        break;
      }
      s.pairs.push_back({params[i], classTy->args[i]});
    }
    if (!params.empty() && classTy->args.size() == params.size() - 1)
    {
      Decl* lastParam = params[params.size() - 1];
      if (lastParam->kind == NodeKind::TemplateParamDecl && static_cast<TemplateParamDecl*>(lastParam)->isPack)
        s.pairs.push_back({lastParam, types.pack({})}); // `Function<void()>`: an empty pack
    }
    return s;
  }

  // For `Vector<int>::iterator`: the substitution of the instantiation the qualifier names
  // (its innermost part with template arguments), empty when there is none.
  Subst qualifierSubst(const QualifiedName& name, const TypeContext& ctx, int depth)
  {
    Subst s;
    if (name.parts.size() < 2 || depth > 20)
      return s;
    // The qualifier as a type: `Vector<int>::`, or `VT::` with `using VT = Vector<int>`.
    QualifiedName prefix = name;
    prefix.parts.count -= 1;
    Lookup l = lookupQualified(prefix, ctx.scope, ctx.locals, depth + 1, ctx.subst);
    if (!l.decl || (l.decl->kind != NodeKind::ClassDecl && l.decl->kind != NodeKind::UsingAliasDecl))
      return s;
    const Ty* qualifier = typeOfTypeDecl(l.decl, l, prefix.last(), ctx, depth + 1);
    if (!qualifier || qualifier->kind != TyKind::Class || qualifier->args.empty())
      return s;
    return substFor(qualifier);
  }

  // A written template argument: a type, or a constant expression for a non-type parameter
  // (a Value type of the parameter's declared type), or unknown.
  const Ty* templateArgument(Node* arg, Decl* param, const TypeContext& ctx, int depth)
  {
    if (!arg)
      return types.unknown();
    std::optional<int64_t> v;
    if (arg->kind == NodeKind::NamedType)
    {
      // `Array<char, bufferSize>`: a bare name is a type to the parser; it may be a constant.
      Lookup l = lookupQualified(static_cast<NamedType*>(arg)->name, ctx.scope, ctx.locals, depth + 1, ctx.subst);
      bool constant = l.decl && ((l.decl->kind == NodeKind::VariableDecl && !(l.decl->flags & DF_Typedef)) || l.decl->kind == NodeKind::EnumeratorDecl ||
                                 (l.decl->kind == NodeKind::TemplateParamDecl && !static_cast<TemplateParamDecl*>(l.decl)->isType));
      if (constant)
        v = constOfDecl(l.decl, ctx, depth);
      else
        return convertType(static_cast<Type*>(arg), ctx, depth + 1);
    }
    else if (uint8_t(arg->kind) <= uint8_t(NodeKind::PackType))
      return convertType(static_cast<Type*>(arg), ctx, depth + 1);
    else
      v = constEval(static_cast<Expr*>(arg), ctx, depth + 1);
    if (!v)
      return types.unknown();
    const Ty* of = types.intType();
    if (param && param->kind == NodeKind::TemplateParamDecl)
      if (auto* p = static_cast<TemplateParamDecl*>(param); !p->isType && p->valueType)
        of = convertType(p->valueType, TypeContext{lexicalScopeOf(param), nullptr, nullptr}, depth + 1);
    return types.valueType(of, *v);
  }

  uint32_t functionSymbol_ = UINT32_MAX;
  uint32_t functionSymbolId()
  {
    if (functionSymbol_ == UINT32_MAX)
      functionSymbol_ = symbols.intern("Function");
    return functionSymbol_;
  }
  uint32_t functionImplSymbol_ = UINT32_MAX;
  uint32_t initializerListSymbol_ = UINT32_MAX;
  uint32_t functionImplSymbolId()
  {
    if (functionImplSymbol_ == UINT32_MAX)
      functionImplSymbol_ = symbols.intern("FunctionImpl");
    return functionImplSymbol_;
  }
  Decl* initializerListDecl()
  {
    if (initializerListSymbol_ == UINT32_MAX)
      initializerListSymbol_ = symbols.intern("InitializerList");
    Lookup l;
    if (lookupInScope(&root, initializerListSymbol_, l, 0) && l.scope && l.scope->owner)
      return l.scope->owner;
    return nullptr;
  }
  bool isInitializerList(const Ty* t)
  {
    return t && t->kind == TyKind::Class && t->decl && t->decl == initializerListDecl();
  }
  uint32_t typeInfoSymbol_ = UINT32_MAX;
  Decl* typeInfoDecl()
  {
    if (typeInfoSymbol_ == UINT32_MAX)
      typeInfoSymbol_ = symbols.intern("TypeInfo");
    Lookup l;
    if (lookupInScope(&root, typeInfoSymbol_, l, 0) && l.scope && l.scope->owner)
      return l.scope->owner;
    return nullptr;
  }
  bool isFunctionImpl(const Ty* t)
  {
    if (!t || t->kind != TyKind::Class || !t->decl || t->decl->kind != NodeKind::ClassDecl)
      return false;
    const QualifiedName& n = static_cast<ClassDecl*>(t->decl)->name;
    return !n.empty() && n.last().symbol == functionImplSymbolId();
  }
  // Whether a callable of type `a` takes the arguments of FunctionImpl<R, Args...> `impl`: a
  // lambda or function (pointer) by its parameters; anything else is not checked.
  bool callableWith(const Ty* a, const Ty* impl, int depth)
  {
    a = types.decay(a);
    const Ty* fn = nullptr;
    if (a->kind == TyKind::Lambda || a->kind == TyKind::Function)
      fn = a;
    else if (a->kind == TyKind::Pointer && a->inner->kind == TyKind::Function)
      fn = a->inner;
    else
      return true;
    if (impl->args.empty())
      return true;
    size_t n = impl->args.size() - 1;
    if (fn->args.size() != n)
      return false;
    for (size_t i = 0; i < n; ++i)
      if (conversionRank(impl->args[i + 1], fn->args[i], depth + 1) >= NotViable)
        return false;
    return true;
  }

  // `ScopedSetter guard(x, true)`: a class template named without arguments is typed as its own
  // parameters (what typeOfTypeDecl gives for the primary); the arguments come from a constructor.
  bool isUndeducedClassTemplate(const Ty* t)
  {
    if (!t || t->kind != TyKind::Class || !t->decl)
      return false;
    Span<Decl*> params = t->decl->templateParams;
    if (params.empty() || t->args.size() != params.size())
      return false;
    for (size_t i = 0; i < params.size(); ++i)
      if (t->args[i] != types.param(params[i]))
        return false;
    return true;
  }
  // The class's arguments deduced from a constructor taking these argument types, or null.
  const Ty* deduceClassArgs(const Ty* primary, const std::vector<const Ty*>& argTypes)
  {
    Scope* s = classScopeOf(primary);
    if (!s || !s->owner || s->owner->kind != NodeKind::ClassDecl)
      return nullptr;
    auto* cls = static_cast<ClassDecl*>(s->owner);
    for (Decl* m : cls->members)
    {
      if (m->kind != NodeKind::FunctionDecl || !(m->flags & DF_Constructor))
        continue;
      auto* ctor = static_cast<FunctionDecl*>(m);
      size_t required = 0;
      bool pack = false;
      for (Decl* p : ctor->params)
      {
        auto* pd = static_cast<ParamDecl*>(p);
        if (pd->isPack)
          pack = true;
        else if (!pd->defaultValue)
          ++required;
      }
      if (argTypes.size() < required || (!pack && argTypes.size() > ctor->params.size()))
        continue;
      const Ty* ft = declaredTypeOf(ctor, nullptr, 0);
      if (!ft || ft->kind != TyKind::Function)
        continue;
      Subst d;
      deduceCall(ctor, ft, argTypes, d);
      std::vector<const Ty*> args;
      bool all = true;
      for (Decl* p : cls->templateParams)
      {
        const Ty* t = d.find(p);
        if (!t)
          all = false;
        else
          args.push_back(t);
      }
      if (!all)
        continue;
      bool ok = true;
      for (size_t i = 0; i < argTypes.size() && i < ft->args.size() && ok; ++i)
        if (conversionRank(argTypes[i], substitute(ft->args[i], d)) >= NotViable)
          ok = false;
      if (ok)
        return types.classType(cls, Span<const Ty*>{args.data(), uint32_t(args.size())});
    }
    return nullptr;
  }

  // The canonical type a resolved name denotes when used as a type.
  const Ty* typeOfTypeDecl(Decl* d, const Lookup& l, const NamePart& lastPart, const TypeContext& ctx, int depth)
  {
    switch (d->kind)
    {
      case NodeKind::ClassDecl:
      {
        // Use the definition (the scope owner) so that all mentions share one decl.
        Decl* def = l.scope && l.scope->owner && l.scope->kind == ScopeKind::Class ? l.scope->owner : d;
        std::vector<const Ty*> args;
        Span<Decl*> params = def->templateParams;
        if (lastPart.templateArgs.size() == 1 && lastPart.symbol == functionSymbolId())
        {
          // The library's `Function<R(Args...)>`: the compiler takes the signature apart, the
          // class behind it is FunctionImpl<R, Args...> (stdlib/core/Function.kov).
          const Ty* signature = templateArgument(lastPart.templateArgs[0], nullptr, ctx, depth);
          if (signature && signature->kind == TyKind::Function)
          {
            Lookup implLookup;
            if (lookupInScope(&root, symbols.intern("FunctionImpl"), implLookup, 0) && implLookup.scope && implLookup.scope->owner)
            {
              std::vector<const Ty*> implArgs{signature->inner};
              for (const Ty* a : signature->args)
                implArgs.push_back(a);
              return types.classType(implLookup.scope->owner, Span<const Ty*>{implArgs.data(), uint32_t(implArgs.size())});
            }
          }
        }
        for (size_t i = 0; i < lastPart.templateArgs.size(); ++i)
        {
          const Ty* a = templateArgument(lastPart.templateArgs[i], i < params.size() ? params[i] : nullptr, ctx, depth);
          if (a && a->kind == TyKind::Pack)
            for (const Ty* e : a->args)
              args.push_back(e); // `FunctionImpl<R, Args...>` inside the template: the elements
          else
            args.push_back(a);
        }
        if (!args.empty())
        {
          // Parameters not written take their defaults, which may use the earlier arguments.
          for (size_t i = args.size(); i < params.size(); ++i)
          {
            auto* p = static_cast<TemplateParamDecl*>(params[i]);
            if (!p->defaultArg)
              break;
            Subst s;
            for (size_t k = 0; k < i; ++k)
              s.pairs.push_back({params[k], args[k]});
            TypeContext dctx{lexicalScopeOf(def), nullptr, &s, def->file ? def->file->tokens : nullptr, def->file ? def->file->text : std::string_view()};
            args.push_back(templateArgument(p->defaultArg, p, dctx, depth));
          }
        }
        if (args.empty() && !params.empty() && ctx.subst)
        {
          // Injected class name inside its own template, or a dependent mention: keep parameters.
          for (Decl* p : params)
            args.push_back(ctx.subst->find(p) ? ctx.subst->find(p) : types.param(p));
        }
        else if (args.empty() && !params.empty())
          for (Decl* p : params)
            args.push_back(types.param(p));
        if (!params.empty())
          if (Decl* spec = matchSpecialization(l, args, depth))
            return types.classType(spec, {}); // a full specialisation is its own class
        return types.classType(def, Span<const Ty*>{args.data(), uint32_t(args.size())});
      }
      case NodeKind::EnumDecl:
      {
        Decl* def = l.scope && l.scope->owner && l.scope->kind == ScopeKind::Enum ? l.scope->owner : d;
        return types.enumType(def);
      }
      case NodeKind::TemplateParamDecl:
        if (ctx.subst)
          if (const Ty* t = ctx.subst->find(d))
            return t;
        return types.param(d);
      case NodeKind::UsingAliasDecl:
      {
        auto* u = static_cast<UsingAliasDecl*>(d);
        if (!u->templateParams.empty())
        {
          // An alias template: its type with the written arguments (and defaults) substituted.
          Span<Decl*> params = u->templateParams;
          Subst s;
          std::vector<const Ty*> args;
          for (size_t i = 0; i < lastPart.templateArgs.size() && i < params.size(); ++i)
          {
            args.push_back(templateArgument(lastPart.templateArgs[i], params[i], ctx, depth));
            s.pairs.push_back({params[i], args.back()});
          }
          for (size_t i = args.size(); i < params.size(); ++i)
          {
            auto* p = static_cast<TemplateParamDecl*>(params[i]);
            if (!p->defaultArg)
              break;
            TypeContext dctx{lexicalScopeOf(u), nullptr, &s, u->file ? u->file->tokens : nullptr, u->file ? u->file->text : std::string_view()};
            args.push_back(templateArgument(p->defaultArg, p, dctx, depth));
            s.pairs.push_back({params[i], args.back()});
          }
          if (ctx.subst)
            for (const Subst::Pair& p : ctx.subst->pairs)
              s.pairs.push_back(p);
          TypeContext actx{lexicalScopeOf(u), nullptr, &s, u->file ? u->file->tokens : nullptr, u->file ? u->file->text : std::string_view()};
          return convertType(u->type, actx, depth + 1);
        }
        return declaredTypeOf(d, ctx.subst, depth + 1); // under the instantiation's arguments, if any
      }
      case NodeKind::VariableDecl:
        if (d->flags & DF_Typedef)
          return declaredTypeOf(d, ctx.subst, depth + 1);
        return types.unknown();
      case NodeKind::UsingNameDecl:
      {
        Lookup inner = lookupQualified(static_cast<UsingNameDecl*>(d)->name, lexicalScopeOf(d), nullptr, depth + 1);
        if (inner.decl && inner.decl != d)
          return typeOfTypeDecl(inner.decl, inner, static_cast<UsingNameDecl*>(d)->name.last(), TypeContext{lexicalScopeOf(d), nullptr, nullptr}, depth + 1);
        return types.unknown();
      }
      default:
        return types.unknown();
    }
  }

  // AST type -> canonical type.
  const Ty* convertType(Type* t, const TypeContext& ctx, int depth)
  {
    if (!t || depth > 40)
      return types.unknown();
    switch (t->kind)
    {
      case NodeKind::BuiltinType:
      {
        auto* b = static_cast<BuiltinType*>(t);
        switch (b->keyword)
        {
          case Kw::void_: return types.voidType();
          case Kw::bool_: return types.boolType();
          case Kw::char_: return types.charType(8, b->isUnsigned);
          case Kw::char8_t_: return types.charType(8, true);
          case Kw::char16_t_: return types.charType(16, true);
          case Kw::char32_t_: return types.charType(32, true);
          case Kw::wchar_t_: return types.charType(16, true);
          case Kw::float_: return types.floatType(32);
          case Kw::double_: return types.floatType(64);
          case Kw::short_: return types.intType(16, b->isUnsigned);
          default:
            // int, long, long long, unsigned, signed: Windows LLP64.
            return types.intType(b->longCount >= 2 ? 64 : 32, b->isUnsigned);
        }
      }
      case NodeKind::NamedType:
      {
        auto* n = static_cast<NamedType*>(t);
        if (n->name.empty())
          return types.unknown();
        if (n->name.parts.size() == 1 && n->name.parts[0].hasTemplateArgs)
          if (const Ty* trait = typeTrait(n->name.parts[0], ctx, depth))
            return trait;
        Lookup l = lookupQualified(n->name, ctx.scope, ctx.locals, depth + 1);
        if (l.found == Found::No)
        {
          if (n->name.parts.size() == 1)
            if (const Ty* b = builtinByName(n->name.parts[0].symbol))
              return b;
          return types.unknown();
        }
        if (l.found == Found::Dependent && l.decl && l.decl->kind == NodeKind::TemplateParamDecl)
        {
          if (n->name.parts.size() > 1)
          {
            // T::type: dependent on T; the substituted T could be looked up, but not yet.
            if (ctx.subst)
              if (const Ty* sub = ctx.subst->find(l.decl))
              {
                if (sub->kind == TyKind::Class && sub->decl && sub->decl->ownScope)
                {
                  // Resolve the rest of the name inside the substituted class.
                  QualifiedName rest = n->name;
                  rest.parts.data += 1;
                  rest.parts.count -= 1;
                  Lookup inner = lookupQualified(rest, sub->decl->ownScope, nullptr, depth + 1);
                  if (inner.decl && inner.found != Found::No)
                  {
                    Subst s2 = substFor(sub);
                    TypeContext c2{sub->decl->ownScope, nullptr, &s2};
                    return typeOfTypeDecl(inner.decl, inner, rest.last(), c2, depth + 1);
                  }
                }
                return types.unknown();
              }
            return types.param(l.decl);
          }
          if (ctx.subst)
            if (const Ty* sub = ctx.subst->find(l.decl))
              return sub;
          return types.param(l.decl);
        }
        if (!l.decl)
          return types.unknown();
        if (n->name.parts.size() > 1 &&
            (l.decl->kind == NodeKind::UsingAliasDecl || (l.decl->kind == NodeKind::VariableDecl && (l.decl->flags & DF_Typedef))))
        {
          // `Vector<int>::iterator`: the alias as the instantiation named by the qualifier has it.
          Subst qs = qualifierSubst(n->name, ctx, depth);
          if (!qs.empty())
          {
            if (ctx.subst)
              for (const Subst::Pair& p : ctx.subst->pairs)
                qs.pairs.push_back(p);
            TypeContext c2{ctx.scope, ctx.locals, &qs, ctx.tokens, ctx.text};
            return typeOfTypeDecl(l.decl, l, n->name.last(), c2, depth);
          }
        }
        return typeOfTypeDecl(l.decl, l, n->name.last(), ctx, depth);
      }
      case NodeKind::PointerType: return types.pointer(convertType(static_cast<PointerType*>(t)->pointee, ctx, depth + 1));
      case NodeKind::LRefType: return types.lref(convertType(static_cast<LRefType*>(t)->referenced, ctx, depth + 1));
      case NodeKind::RRefType: return types.rref(convertType(static_cast<RRefType*>(t)->referenced, ctx, depth + 1));
      case NodeKind::ConstType:
      {
        auto* c = static_cast<ConstType*>(t);
        return types.qualified(convertType(c->inner, ctx, depth + 1), true, c->isVolatile);
      }
      case NodeKind::ArrayType:
      {
        auto* a = static_cast<ArrayType*>(t);
        auto n = constEval(a->size, ctx);
        return types.array(convertType(a->element, ctx, depth + 1), n && *n > 0 ? uint32_t(*n) : 0);
      }
      case NodeKind::FunctionType:
      {
        auto* f = static_cast<FunctionType*>(t);
        std::vector<const Ty*> params;
        for (Decl* p : f->params)
        {
          const Ty* pt = convertType(static_cast<ParamDecl*>(p)->type, ctx, depth + 1);
          if (pt && pt->kind == TyKind::Pack)
            for (const Ty* e : pt->args)
              params.push_back(e); // `R (*)(Args...)` under an instantiation: one parameter per element
          else
            params.push_back(pt);
        }
        return types.function(convertType(f->result, ctx, depth + 1), Span<const Ty*>{params.data(), uint32_t(params.size())}, false);
      }
      case NodeKind::AutoType: return types.autoType();
      case NodeKind::DecltypeType:
      {
        auto* d = static_cast<DecltypeType*>(t);
        return d->expr && d->expr->ty ? d->expr->ty : types.unknown();
      }
      case NodeKind::MemberPointerType:
      {
        auto* m = static_cast<MemberPointerType*>(t);
        const Ty* cls = convertType(m->classType, ctx, depth + 1);
        return types.memberPointer(convertType(m->pointee, ctx, depth + 1), cls);
      }
      case NodeKind::PackType: return convertType(static_cast<PackType*>(t)->pattern, ctx, depth + 1);
      default: return types.unknown();
    }
  }

  // Context in which a declaration's own type is written: its lexical scope plus the template
  // parameters of the declaration and of its enclosing templates.
  struct DeclContext
  {
    LocalStack params;
    Scope* scope;
  };
  void declContextFor(Decl* d, DeclContext& out)
  {
    out.scope = lexicalScopeOf(d);
    for (Decl* p = d; p; p = p->parent)
      if (p->kind == NodeKind::FunctionDecl)
      {
        // `int Box::f(const Terminal t) const` outside its class: the class scope, as C++ has it,
        // for the function and everything declared inside it.
        auto* f = static_cast<FunctionDecl*>(p);
        if (f->name.parts.size() > 1)
        {
          QualifiedName qualifier = f->name;
          qualifier.parts.count -= 1;
          LocalStack params;
          for (Decl* tp : f->templateParams)
            params.push(static_cast<TemplateParamDecl*>(tp)->name, tp);
          Lookup l = lookupQualified(qualifier, lexicalScopeOf(f), &params, 0);
          if (l.scope && l.scope->kind == ScopeKind::Class)
            out.scope = l.scope;
        }
        break;
      }
    std::vector<Decl*> chain;
    for (Decl* p = d; p; p = p->parent)
      chain.push_back(p);
    for (size_t i = chain.size(); i-- > 0;)
      for (Decl* tp : chain[i]->templateParams)
        out.params.push(static_cast<TemplateParamDecl*>(tp)->name, tp);
  }

  // Declared type of a declaration, seen through an optional substitution (for members of
  // template instantiations). Non-dependent results are cached on the declaration.
  const Ty* declaredTypeOf(Decl* d, const Subst* subst, int depth)
  {
    if (!d || depth > 40)
      return types.unknown();
    bool useCache = !subst || subst->empty();
    if (useCache)
      if (const Ty* cached = d->cachedType.load(std::memory_order_acquire))
        return cached;
    const Ty* result = computeDeclaredType(d, subst, depth);
    if (useCache && result->kind != TyKind::Auto)
      d->cachedType.store(result, std::memory_order_release);
    return result;
  }

  const Ty* computeDeclaredType(Decl* d, const Subst* subst, int depth)
  {
    DeclContext dc;
    declContextFor(d, dc);
    TypeContext ctx{dc.scope, &dc.params, subst, d->file ? d->file->tokens : nullptr, d->file ? d->file->text : std::string_view()};
    switch (d->kind)
    {
      case NodeKind::VariableDecl:
      {
        auto* v = static_cast<VariableDecl*>(d);
        if (v->builtin == BUILTIN_EnumValues && v->parent && v->parent->kind == NodeKind::EnumDecl)
          return types.array(types.enumType(v->parent), uint32_t(enumeratorCount(v->parent))); // E::values
        const Ty* t = convertType(v->type, ctx, depth + 1);
        if (t && t->kind == TyKind::Array && t->count == 0 && v->init && v->init->kind == NodeKind::BraceInitExpr)
          t = types.array(t->inner, uint32_t(static_cast<BraceInitExpr*>(v->init)->args.size())); // `double a[] = {1, 2}`
        if (types.isDependent(t) && containsAuto(t))
          return t; // Deduced from the initializer by the walker when it reaches the declaration.
        return t;
      }
      case NodeKind::ParamDecl:
      {
        const Ty* t = convertType(static_cast<ParamDecl*>(d)->type, ctx, depth + 1);
        return t && t->kind == TyKind::Array ? types.decay(t) : t; // `double m[3][3]` is a pointer parameter
      }
      case NodeKind::EnumeratorDecl:
        return d->parent && d->parent->kind == NodeKind::EnumDecl ? types.enumType(d->parent) : types.intType();
      case NodeKind::FunctionDecl:
      {
        auto* f = static_cast<FunctionDecl*>(d);
        const Ty* result;
        if (f->flags & (DF_Constructor | DF_Destructor))
          result = types.voidType();
        else if (f->flags & DF_Conversion)
          result = convertType(f->name.last().convType, ctx, depth + 1);
        else if (f->trailingReturn)
          result = convertType(f->trailingReturn, ctx, depth + 1);
        else
          result = convertType(f->returnType, ctx, depth + 1);
        std::vector<const Ty*> params;
        bool variadic = (f->flags & DF_Variadic) != 0;
        for (Decl* p : f->params)
        {
          auto* pd = static_cast<ParamDecl*>(p);
          const Ty* pt = convertType(pd->type, ctx, depth + 1);
          if (pt && pt->kind == TyKind::Array)
            pt = types.decay(pt); // `f(double m[3][3])` takes a pointer, as in C
          if (pt && !pt->isReference() && pt->kind != TyKind::Pack)
            pt = types.unqualified(pt); // `f(const int x)` is f(int): the const is the definition's business
          if (pd->isPack && pt->kind == TyKind::Pack)
            for (const Ty* e : pt->args)
              params.push_back(e); // deduced pack: one parameter per element
          else
          {
            params.push_back(pt);
            if (pd->isPack)
              variadic = true; // still a pack
          }
        }
        return types.function(result, Span<const Ty*>{params.data(), uint32_t(params.size())}, variadic);
      }
      case NodeKind::ClassDecl:
      case NodeKind::EnumDecl:
        return selfType(d);
      case NodeKind::TemplateParamDecl:
      {
        auto* p = static_cast<TemplateParamDecl*>(d);
        if (subst)
          if (const Ty* t = subst->find(d))
            return p->isType || t->kind != TyKind::Value ? t : t->inner; // a value parameter has its type
        if (p->isType)
          return types.param(d);
        return p->valueType ? convertType(p->valueType, ctx, depth + 1) : types.intType();
      }
      case NodeKind::UsingAliasDecl:
        return convertType(static_cast<UsingAliasDecl*>(d)->type, ctx, depth + 1);
      default:
        return types.unknown();
    }
  }

  bool containsAuto(const Ty* t) const
  {
    for (; t; t = t->inner)
      if (t->kind == TyKind::Auto)
        return true;
    return false;
  }

  // Replaces the Auto placeholder in `pattern` with the type deduced from an initializer.
  const Ty* deduceAuto(const Ty* pattern, const Ty* init)
  {
    if (!pattern || !init || !init->isKnown())
      return types.unknown();
    if (pattern->kind == TyKind::Auto)
      return pattern->isConst ? types.qualified(types.decay(init), true, false) : types.decay(init);
    auto leaf = [&](const Ty* p) -> const Ty*
    {
      const Ty* base = types.stripRef(init);
      return types.qualified(base, p->isConst, p->isVolatile);
    };
    switch (pattern->kind)
    {
      case TyKind::LRef:
        return pattern->inner->kind == TyKind::Auto ? types.lref(leaf(pattern->inner)) : types.lref(deduceAuto(pattern->inner, init));
      case TyKind::RRef:
        return pattern->inner->kind == TyKind::Auto ? (init->isReference() ? init : types.rref(leaf(pattern->inner)))
                                                    : types.rref(deduceAuto(pattern->inner, init));
      case TyKind::Pointer:
      {
        const Ty* base = types.stripRef(init);
        if (pattern->inner->kind == TyKind::Auto && base->kind == TyKind::Pointer)
          return types.qualified(types.pointer(types.qualified(base->inner, pattern->inner->isConst, false)), pattern->isConst, false);
        if (base->kind == TyKind::Pointer)
          return types.pointer(deduceAuto(pattern->inner, base->inner));
        return types.unknown();
      }
      default:
        return types.unknown();
    }
  }

  // Does `t` mention one of the given template parameters?
  static bool dependsOn(const Ty* t, Span<Decl*> params)
  {
    if (!t)
      return false;
    switch (t->kind)
    {
      case TyKind::Param:
        for (Decl* p : params)
          if (p == t->decl)
            return true;
        return false;
      case TyKind::Pointer: case TyKind::LRef: case TyKind::RRef: case TyKind::Array: case TyKind::MemberPointer:
        return dependsOn(t->inner, params);
      case TyKind::Function: case TyKind::Class: case TyKind::Lambda:
        if (dependsOn(t->inner, params))
          return true;
        for (const Ty* a : t->args)
          if (dependsOn(a, params))
            return true;
        return false;
      default:
        return false;
    }
  }

  // Structural template argument deduction: binds parameters of `pattern` from `arg`.
  void deduce(const Ty* pattern, const Ty* arg, Subst& out, int depth = 0)
  {
    if (!pattern || !arg || depth > 16 || !arg->isKnown())
      return;
    if (pattern->kind == TyKind::Param)
    {
      // `T value` takes the decayed argument; inside a pointer or a class (`T*`, `Vector<T>`) T
      // keeps the argument's qualifiers: `const int*` against `T*` makes T `const int`.
      if (!out.find(pattern->decl))
        out.pairs.push_back({pattern->decl, depth == 0 ? types.decay(arg) : types.stripRef(arg)});
      return;
    }
    if (pattern->isReference())
    {
      // `const T&` binds T to the argument without its reference and top-level const; `T&`
      // keeps the argument's const (`forward(const Event&)` deduces T = const Event, as C++
      // does); the forwarding reference `T&&` given an lvalue of type U binds T to U& (which
      // collapses the parameter to U&), so that the callee sees an lvalue and copies rather
      // than moves.
      const Ty* a = types.stripRef(arg);
      if (pattern->inner->kind == TyKind::Param)
      {
        bool forwarding = pattern->kind == TyKind::RRef && !pattern->inner->isConst && depth == 0;
        if (!out.find(pattern->inner->decl))
        {
          const Ty* bound = pattern->inner->isConst ? types.unqualified(a) : a;
          if (forwarding && arg->kind == TyKind::LRef)
            bound = types.lref(a);
          out.pairs.push_back({pattern->inner->decl, bound});
        }
        return;
      }
      deduce(pattern->inner, a, out, depth + 1);
      return;
    }
    const Ty* a = types.stripRef(arg);
    switch (pattern->kind)
    {
      case TyKind::Pointer:
        if (a->kind == TyKind::Pointer)
          deduce(pattern->inner, a->inner, out, depth + 1);
        else if (a->kind == TyKind::Array)
          deduce(pattern->inner, a->inner, out, depth + 1);
        break;
      case TyKind::Array:
        if (a->kind == TyKind::Array || a->kind == TyKind::Pointer)
          deduce(pattern->inner, a->inner, out, depth + 1);
        break;
      case TyKind::MemberPointer:
        if (a->kind == TyKind::MemberPointer)
        {
          deduce(pattern->inner, a->inner, out, depth + 1);
          if (!pattern->args.empty() && !a->args.empty())
            deduce(pattern->args[0], a->args[0], out, depth + 1);
        }
        break;
      case TyKind::Class:
        if (a->kind == TyKind::Class && a->decl == pattern->decl)
          for (size_t i = 0; i < pattern->args.size() && i < a->args.size(); ++i)
            deduce(pattern->args[i], a->args[i], out, depth + 1);
        break;
      case TyKind::Function:
        if (a->kind == TyKind::Function || a->kind == TyKind::Lambda)
        {
          deduce(pattern->inner, a->inner, out, depth + 1);
          for (size_t i = 0; i < pattern->args.size() && i < a->args.size(); ++i)
            deduce(pattern->args[i], a->args[i], out, depth + 1);
        }
        break;
      default:
        break;
    }
  }

  // The template parameter a parameter pack's type is built from (`const Args&...` -> Args).
  static Decl* packParamOf(const Ty* t)
  {
    for (int i = 0; t && i < 8; ++i)
    {
      if (t->kind == TyKind::Param)
        return t->decl && t->decl->kind == NodeKind::TemplateParamDecl && static_cast<TemplateParamDecl*>(t->decl)->isPack ? t->decl : nullptr;
      if (t->kind == TyKind::Pointer || t->isReference())
        t = t->inner;
      else
        return nullptr;
    }
    return nullptr;
  }

  // Deduces f's template parameters from the argument types of a call, including a trailing
  // parameter pack, which binds to the remaining arguments.
  // The declared type of candidate with its template parameters renamed to those of f: a
  // declaration and its separate definition each have their own, and signatures must compare equal.
  const Ty* typeAs(FunctionDecl* candidate, FunctionDecl* f)
  {
    const Ty* t = declaredTypeOf(candidate, nullptr, 0);
    if (candidate == f || f->templateParams.empty() || candidate->templateParams.size() != f->templateParams.size())
      return t;
    Subst s;
    for (size_t i = 0; i < f->templateParams.size(); ++i)
      s.pairs.push_back({candidate->templateParams[i], types.param(f->templateParams[i])});
    return substitute(t, s);
  }

  void deduceCall(FunctionDecl* f, const Ty* fnTy, const std::vector<const Ty*>& argTypes, Subst& out)
  {
    if (!fnTy || fnTy->kind != TyKind::Function)
      return;
    size_t i = 0;
    for (size_t p = 0; p < f->params.size() && i < fnTy->args.size(); ++p)
    {
      auto* pd = static_cast<ParamDecl*>(f->params[p]);
      const Ty* pattern = fnTy->args[i];
      if (pd->isPack)
      {
        if (Decl* packDecl = packParamOf(pattern))
        {
          if (!out.find(packDecl))
          {
            bool forwarding = pattern->kind == TyKind::RRef && pattern->inner->kind == TyKind::Param && !pattern->inner->isConst;
            std::vector<const Ty*> elems;
            for (size_t j = i; j < argTypes.size(); ++j)
            {
              const Ty* at = argTypes[j];
              if (!at)
                elems.push_back(types.unknown());
              else if (forwarding && at->kind == TyKind::LRef)
                elems.push_back(types.lref(types.stripRef(at))); // `emplace_back(name)`: Args = String&
              else
                elems.push_back(types.decay(at));
            }
            out.pairs.push_back({packDecl, types.pack(Span<const Ty*>{elems.data(), uint32_t(elems.size())})});
          }
        }
        return;
      }
      if (i < argTypes.size())
        deduce(pattern, argTypes[i], out);
      ++i;
    }
  }

  // Applies a substitution to a canonical type.
  const Ty* substitute(const Ty* t, const Subst& s, int depth = 0)
  {
    if (!t || s.empty() || depth > 16)
      return t;
    switch (t->kind)
    {
      case TyKind::Param:
        if (const Ty* r = s.find(t->decl))
          return types.qualified(r, t->isConst || r->isConst, t->isVolatile || r->isVolatile); // T = const int keeps its const
        return t;
      case TyKind::Pointer: return types.qualified(types.pointer(substitute(t->inner, s, depth + 1)), t->isConst, t->isVolatile);
      case TyKind::LRef: return types.lref(substitute(t->inner, s, depth + 1));
      case TyKind::RRef: return types.rref(substitute(t->inner, s, depth + 1));
      case TyKind::Array: return types.array(substitute(t->inner, s, depth + 1), t->count);
      case TyKind::MemberPointer:
        return types.memberPointer(substitute(t->inner, s, depth + 1), t->args.empty() ? nullptr : substitute(t->args[0], s, depth + 1));
      case TyKind::Function:
      case TyKind::Class:
      case TyKind::Lambda:
      case TyKind::Pack:
      {
        std::vector<const Ty*> args;
        for (const Ty* a : t->args)
        {
          const Ty* sub = substitute(a, s, depth + 1);
          if (sub->kind == TyKind::Pack && t->kind != TyKind::Pack)
            for (const Ty* e : sub->args)
              args.push_back(e);
          else
            args.push_back(sub);
        }
        Span<const Ty*> span{args.data(), uint32_t(args.size())};
        if (t->kind == TyKind::Pack)
          return types.pack(span);
        if (t->kind == TyKind::Function)
          return types.function(substitute(t->inner, s, depth + 1), span, t->variadic);
        if (t->kind == TyKind::Lambda)
          return types.lambda(substitute(t->inner, s, depth + 1), span, t->decl, t->value);
        return types.qualified(types.classType(t->decl, span), t->isConst, t->isVolatile);
      }
      default:
        return t;
    }
  }

  // ---------------- overload resolution ----------------

  // Is `base` a (transitive) base class scope of `derived`?
  bool isBaseScope(Scope* base, Scope* derived, int depth = 0)
  {
    if (!base || !derived || depth > 16)
      return false;
    ensureBases(derived, depth);
    for (Scope* b : derived->baseScopes)
      if (b == base || isBaseScope(base, b, depth + 1))
        return true;
    return false;
  }

  Scope* classScopeOf(const Ty* t)
  {
    t = types.unqualified(types.stripRef(t));
    if (!t || t->kind != TyKind::Class || !t->decl)
      return nullptr;
    if (t->decl->ownScope)
      return t->decl->ownScope;
    Entry e{t->decl, nullptr};
    return scopeOfEntry(e, 0);
  }

  static constexpr int NotViable = 1000;

  // Rank of converting an argument of type `arg` to a parameter of type `param`: 0 exact,
  // 1 qualification, 2 promotion, 3 standard conversion, 4 user-defined, NotViable.
  int conversionRank(const Ty* arg, const Ty* param, int depth = 0)
  {
    if (!arg || !param || !arg->isKnown() || !param->isKnown() || types.isDependent(arg) || types.isDependent(param))
      return 2; // Unknown types never decide an overload.
    if (depth > 4)
      return NotViable;
    const Ty* a = types.stripRef(arg);
    if (isInitializerList(a) && !isInitializerList(types.stripRef(param)))
      return 2; // a brace list: it converts to anything else through its elements, which the code generator checks
    if (param->isReference())
    {
      const Ty* pt = param->inner;
      bool lvalueToRvalueRef = param->kind == TyKind::RRef && arg->kind == TyKind::LRef; // `T&&` takes no lvalue directly
      if (types.unqualified(a) == types.unqualified(pt))
      {
        if (lvalueToRvalueRef || (!pt->isConst && a->isConst))
          return NotViable;
        return pt->isConst && !a->isConst ? 1 : 0;
      }
      if (a->kind == TyKind::Class && pt->kind == TyKind::Class && isBaseScope(classScopeOf(pt), classScopeOf(a)))
        return lvalueToRvalueRef || (!pt->isConst && a->isConst) ? NotViable : 3;
      if (param->kind == TyKind::LRef && !pt->isConst)
        return NotViable; // A non-const lvalue reference binds only to the same type.
      int r = conversionRank(a, pt, depth + 1);
      if (r >= NotViable)
        return NotViable;
      if (r >= 4 && param->kind == TyKind::LRef)
        return 5; // the converted temporary binds better to `T&&` than to `const T&`
      return std::max(r, 2);
    }
    a = types.decay(a);
    const Ty* p = types.unqualified(param);
    if (a == p)
      return 0;
    auto isInt = [](const Ty* t) { return t->kind == TyKind::Int || t->kind == TyKind::Char || t->kind == TyKind::Bool; };
    switch (p->kind)
    {
      case TyKind::Bool:
        if (isInt(a) || a->kind == TyKind::Float || a->kind == TyKind::Pointer || a->kind == TyKind::Nullptr)
          return 3;
        if (a->kind == TyKind::Enum && !static_cast<EnumDecl*>(a->decl)->isClass)
          return 3;
        break;
      case TyKind::Int:
      case TyKind::Char:
        if (isInt(a))
        {
          bool promotion = p->kind == TyKind::Int && p->bits == 32 && !p->isUnsigned &&
                           (a->kind == TyKind::Bool || a->kind == TyKind::Char || a->bits < 32);
          return promotion ? 2 : 3;
        }
        if (a->kind == TyKind::Float)
          return 3;
        if (a->kind == TyKind::Enum)
          return static_cast<EnumDecl*>(a->decl)->isClass ? NotViable : (p->kind == TyKind::Int && p->bits == 32 ? 2 : 3);
        break;
      case TyKind::Float:
        if (a->kind == TyKind::Float)
          return p->bits > a->bits ? 2 : 3;
        if (isInt(a))
          return 3;
        if (a->kind == TyKind::Enum && !static_cast<EnumDecl*>(a->decl)->isClass)
          return 3;
        break;
      case TyKind::Enum:
        if (a->kind == TyKind::Enum && a->decl == p->decl)
          return 0;
        break;
      case TyKind::MemberPointer:
        if (a->kind == TyKind::MemberPointer && types.unqualified(a->inner) == types.unqualified(p->inner) && a->decl == p->decl)
          return 0;
        if (a->kind == TyKind::Nullptr)
          return 3;
        break;
      case TyKind::Pointer:
        if (a->kind == TyKind::Nullptr)
          return 3;
        if (a->kind == TyKind::Pointer)
        {
          const Ty* ai = a->inner;
          const Ty* pi = p->inner;
          if (types.unqualified(ai) == types.unqualified(pi))
            return pi->isConst == ai->isConst ? 0 : (pi->isConst ? 1 : NotViable);
          if (pi->kind == TyKind::Void)
            return 3;
          if (ai->kind == TyKind::Class && pi->kind == TyKind::Class && isBaseScope(classScopeOf(pi), classScopeOf(ai)))
            return !pi->isConst && ai->isConst ? NotViable : 3;
          return NotViable;
        }
        if (a->kind == TyKind::Lambda && p->inner->kind == TyKind::Function)
          return 3;
        break;
      case TyKind::Class:
      {
        if (a->kind == TyKind::Class)
        {
          if (types.unqualified(a)->decl == p->decl && a->args.size() == p->args.size())
          {
            bool same = true;
            for (size_t i = 0; i < a->args.size(); ++i)
              same = same && a->args[i] == p->args[i];
            if (same)
              return 0;
          }
          if (isBaseScope(classScopeOf(p), classScopeOf(a)))
            return 3;
        }
        // User-defined: a non-explicit single-argument constructor of p accepting a.
        if (Decl* def = p->decl && p->decl->ownScope ? p->decl : (classScopeOf(p) ? classScopeOf(p)->owner : nullptr))
        {
          Subst s = substFor(p);
          for (Decl* m : static_cast<ClassDecl*>(def)->members)
          {
            if (m->kind != NodeKind::FunctionDecl || !(m->flags & DF_Constructor) || (m->flags & DF_Explicit))
              continue;
            auto* f = static_cast<FunctionDecl*>(m);
            if (f->params.size() != 1)
              continue;
            Subst d = s;
            if (!f->templateParams.empty())
            {
              // `template<typename F> Function(F callable)`: F from the argument.
              const Ty* ft = declaredTypeOf(f, &s, 0);
              deduceCall(f, ft, std::vector<const Ty*>{a}, d);
              if (isFunctionImpl(p) && !callableWith(a, p, depth))
                continue; // Function<void(double)> takes no `[]() {}`
            }
            const Ty* pt = declaredTypeOf(f->params[0], &d, 0);
            if (pt && types.stripRef(pt)->kind == TyKind::Class && types.unqualified(types.stripRef(pt)) == types.unqualified(p))
              continue; // The copy constructor is not a conversion (ReverseIterator<const T> from ReverseIterator<T> is).
            if (conversionRank(a, pt, depth + 1) < 4)
              return 4;
          }
        }
        break;
      }
      default:
        break;
    }
    // Conversion operator of the argument's class.
    if (a->kind == TyKind::Class)
      if (Scope* s = classScopeOf(a))
      {
        Subst subst = substFor(a);
        for (Decl* m : static_cast<ClassDecl*>(s->owner)->members)
        {
          if (m->kind != NodeKind::FunctionDecl || !(m->flags & DF_Conversion) || (m->flags & DF_Explicit))
            continue;
          const Ty* ft = declaredTypeOf(m, &subst, 0);
          if (ft && ft->kind == TyKind::Function && conversionRank(ft->inner, param, depth + 1) < 4)
            return 4;
        }
      }
    return NotViable;
  }

  struct Candidate
  {
    FunctionDecl* f;
    std::vector<int> ranks;
    int thisRank; // 0 exact constness, 1 const method on a non-const object, NotViable if const object/non-const method
    std::vector<bool> refs; // per rank: the parameter is a reference
  };

  // Picks the best candidate by C++'s rule (better in one argument, not worse in any); on a tie
  // the non-template, then the first one. Null when no candidate is viable.
  FunctionDecl* chooseAmong(const std::vector<FunctionDecl*>& candidates, const std::vector<const Ty*>& argTypes,
                            const Subst* classSubst, const Ty* objectTy, bool& ambiguous)
  {
    std::vector<Candidate> viable;
    bool objectConst = objectTy && types.stripRef(objectTy)->isConst;
    bool single = candidates.size() == 1; // One candidate: arity decides, no ranking needed.
    for (FunctionDecl* f : candidates)
    {
      size_t required = 0, total = f->params.size();
      bool variadic = (f->flags & DF_Variadic) != 0;
      for (Decl* p : f->params)
      {
        auto* param = static_cast<ParamDecl*>(p);
        if (param->isPack)
          variadic = true;
        else if (!param->defaultValue)
          ++required;
      }
      if (argTypes.size() < required || (!variadic && argTypes.size() > total))
        continue;
      Candidate c{f, {}, 0, {}};
      bool constMethod = (f->flags & DF_Const) != 0;
      if (objectTy && !(f->flags & DF_Static) && !single)
      {
        if (objectConst && !constMethod)
          continue;
        c.thisRank = (!objectConst && constMethod) ? 1 : 0;
      }
      const Ty* ft = declaredTypeOf(f, classSubst, 0);
      Subst deduced = classSubst ? *classSubst : Subst{};
      if (!f->templateParams.empty() && ft && ft->kind == TyKind::Function)
        deduceCall(f, ft, argTypes, deduced);
      bool ok = true;
      for (size_t i = 0; i < argTypes.size() && ok; ++i)
      {
        const Ty* pt = ft && ft->kind == TyKind::Function && i < ft->args.size() ? ft->args[i] : nullptr;
        if (!pt)
        {
          c.ranks.push_back(3); // variadic tail
          c.refs.push_back(false);
          continue;
        }
        if (!f->templateParams.empty())
        {
          pt = substitute(pt, deduced);
          const Ty* at = argTypes[i];
          if (at && at->isKnown() && !types.isDependent(at) && dependsOn(pt, f->templateParams))
          {
            // Deduction failed (`operator<<(UniquePtr<T>&)` given a Label&): not a candidate.
            ok = false;
            break;
          }
        }
        if (pt->kind == TyKind::Pack)
        {
          // A deduced parameter pack: the remaining arguments match its elements one by one.
          for (size_t j = i; j < argTypes.size() && ok; ++j)
          {
            const Ty* et = j - i < pt->args.size() ? pt->args[j - i] : nullptr;
            int r = et ? conversionRank(argTypes[j], et) : 3;
            if (r >= NotViable)
              ok = false;
            c.ranks.push_back(r);
            c.refs.push_back(et && et->isReference());
          }
          break;
        }
        int r = conversionRank(argTypes[i], pt);
        if (r >= NotViable)
          ok = false;
        c.ranks.push_back(r);
        c.refs.push_back(pt->isReference());
      }
      if (ok)
        viable.push_back(std::move(c));
    }
    ambiguous = false;
    if (viable.empty())
      return nullptr;
    auto better = [&](const Candidate& a, const Candidate& b)
    {
      bool someBetter = false;
      for (size_t i = 0; i < a.ranks.size(); ++i)
      {
        // Exact and const-qualified-reference bindings differ only between two references
        // (`T&` beats `const T&`); against a by-value parameter they are the same match.
        if (a.ranks[i] + b.ranks[i] == 1 && !(a.refs[i] && b.refs[i]))
          continue;
        if (a.ranks[i] > b.ranks[i])
          return false;
        if (a.ranks[i] < b.ranks[i])
          someBetter = true;
      }
      if (a.thisRank > b.thisRank)
        return false;
      if (a.thisRank < b.thisRank)
        someBetter = true;
      if (!someBetter && a.f->templateParams.empty() && !b.f->templateParams.empty())
        return true;
      return someBetter;
    };
    for (const Candidate& c : viable)
    {
      bool best = true;
      for (const Candidate& o : viable)
        if (&o != &c && !better(c, o))
          best = false;
      if (best)
        return c.f;
    }
    ambiguous = viable.size() > 1;
    // No clear winner: the smallest total rank, then non-template, then declaration order.
    const Candidate* pick = &viable[0];
    auto total = [](const Candidate& c) { int t = c.thisRank; for (int r : c.ranks) t += r; return t; };
    for (const Candidate& c : viable)
      if (total(c) < total(*pick) || (total(c) == total(*pick) && c.f->templateParams.empty() && !pick->f->templateParams.empty()))
        pick = &c;
    return pick->f;
  }

  // ---------------- phase 2: walking ----------------

  struct Walker
  {
    Impl& impl;
    TypeTable& types;
    ResolveStats& stats;
    const SymbolTable& symbols;
    const std::vector<Token>& tokens;
    std::string_view text;
    LocalStack locals;
    Scope* scope;            // Current lexical/semantic scope.
    const Ty* thisType = nullptr;
    const Subst* activeSubst = nullptr; // Set while checking a template body for an instantiation.
    int instantiationDepth = 0;
    // Return statements inside lambdas / auto functions record their type here.
    std::vector<const Ty*> returnFrames;

    Walker(Impl& impl, ResolveStats& stats, const std::vector<Token>& tokens, std::string_view text)
      : impl(impl), types(impl.types), stats(stats), symbols(impl.symbols), tokens(tokens), text(text), scope(&impl.root)
    {
    }

    std::string_view spelling(uint32_t token) const
    {
      const Token& t = tokens[std::min<size_t>(token, tokens.size() - 1)];
      return text.substr(t.offset, t.length);
    }

    // --- statistics helpers ---

    void countUnresolved(const QualifiedName& name, uint32_t token)
    {
      uint32_t first = name.parts[0].symbol;
      if (first == impl.symStd && name.parts.size() > 1)
      {
        ++stats.qualifiedUnresolvedStd;
        return;
      }
      uint32_t key = name.last().symbol;
      if (key == UINT32_MAX)
        return;
      if (name.parts.size() == 1 && isAllCaps(symbols.name(key)))
      {
        ++stats.unresolvedMacroLike;
        return;
      }
      ++stats.unresolvedSymbols[key];
      stats.unresolvedFirst.emplace(key, ResolveStats::Where{stats.fileIndex, token});
    }

    enum class Why : uint8_t { None, Name, Base, Member, Overload, Deduction, AutoReturn, Unsupported, Operator, Auto };

    const Ty* unknown(Why why)
    {
      switch (why)
      {
        case Why::Name: ++stats.unknownName; break;
        case Why::Base: ++stats.unknownBase; break;
        case Why::Member: ++stats.unknownMember; break;
        case Why::Overload: ++stats.unknownOverload; break;
        case Why::Deduction: ++stats.unknownDeduction; break;
        case Why::AutoReturn: ++stats.unknownAutoReturn; break;
        case Why::Unsupported: ++stats.unknownUnsupported; break;
        case Why::Operator: ++stats.unknownOperator; break;
        case Why::Auto: ++stats.unknownAuto; break;
        default: break;
      }
      return types.unknown();
    }

    // Records the type of an expression node and counts it.
    const Ty* record(Expr* e, const Ty* t)
    {
      if (!t)
        t = types.unknown();
      e->ty = t;
      if (!t->isKnown())
        ++stats.exprsUnknown;
      else if (types.isDependent(t))
        ++stats.exprsDependent;
      else
        ++stats.exprsTyped;
      return t;
    }

    TypeContext ctx() const { return TypeContext{scope, &locals, activeSubst, &tokens, text}; }

    // --- template instantiation: check bodies once per (template, arguments) ---

    static bool argsConcrete(TypeTable& types, const Ty* t)
    {
      if (!t || t->kind != TyKind::Class || t->args.empty())
        return false;
      for (const Ty* a : t->args)
        if (!a->isKnown() || types.isDependent(a))
          return false;
      return true;
    }

    // Runs `body` as a sub-check with the given substitution, scope and this-type, in the
    // context of the template's own file. Expression statistics are merged into ours.
    template<typename F>
    void subCheck(Decl* templ, const Subst& subst, Scope* bodyScope, const Ty* bodyThis, F&& body)
    {
      ParsedFile* file = templ->file;
      if (!file || !file->tokens)
        return;
      ResolveStats local;
      local.fileIndex = stats.fileIndex;
      Walker sub(impl, local, *file->tokens, file->text);
      sub.scope = bodyScope;
      sub.thisType = bodyThis;
      sub.activeSubst = &subst;
      sub.instantiationDepth = instantiationDepth + 1;
      for (Decl* p : templ->templateParams)
        sub.locals.push(static_cast<TemplateParamDecl*>(p)->name, p);
      body(sub);
      local.instExprsTyped += local.exprsTyped;
      local.instExprsDependent += local.exprsDependent;
      local.instExprsUnknown += local.exprsUnknown;
      stats.add(local);
    }

    void checkMemberBody(Walker& sub, FunctionDecl* f, const Subst& classSubst)
    {
      // Out-of-line members carry their own template parameter list; map it positionally.
      Subst merged = classSubst;
      Decl* cls = sub.scope->owner;
      if (cls && f->templateParams.size() == cls->templateParams.size() && f->parent != cls)
        for (size_t i = 0; i < f->templateParams.size(); ++i)
          if (const Ty* t = classSubst.find(cls->templateParams[i]))
            merged.pairs.push_back({f->templateParams[i], t});
      sub.activeSubst = &merged;
      size_t mark = sub.locals.mark();
      if (f->parent != cls)
        for (Decl* p : f->templateParams)
          sub.locals.push(static_cast<TemplateParamDecl*>(p)->name, p);
      sub.walkDecl(f);
      sub.locals.pop(mark);
      sub.activeSubst = &classSubst;
      ++impl.instBodies;
    }

    // Checks every member function body of a class template for one instantiation, once.
    void instantiateClass(const Ty* classTy)
    {
      if (instantiationDepth > 16 || !argsConcrete(types, classTy) || !classTy->decl || !classTy->decl->ownScope)
        return;
      if (!classTy->decl->isTemplate || classTy->decl->templateParams.empty())
        return;
      bool producer;
      Impl::Instantiation* inst = impl.claimClass(classTy, producer);
      if (!producer)
        return; // Done or being done by someone else; nothing here needs its result.
      auto start = std::chrono::steady_clock::now();
      Subst subst = impl.substFor(classTy);
      Scope* cls = classTy->decl->ownScope;
      subCheck(classTy->decl, subst, cls, types.pointer(classTy), [&](Walker& sub)
      {
        auto* c = static_cast<ClassDecl*>(classTy->decl);
        for (Decl* m : c->members)
          if (m->kind == NodeKind::FunctionDecl && static_cast<FunctionDecl*>(m)->body && m->templateParams.empty())
            checkMemberBody(sub, static_cast<FunctionDecl*>(m), subst);
        for (FunctionDecl* f : cls->outOfLineMembers)
          if (f->body)
            checkMemberBody(sub, f, subst);
      });
      impl.instNanos += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count());
      inst->state.store(2, std::memory_order_release);
    }

    // Checks a function template body for one set of deduced arguments, once. Returns the
    // deduced return type when the function returns `auto` (waiting for another producer).
    const Ty* instantiateFunction(FunctionDecl* f, const Subst& subst, bool needReturn, const Subst* classSubst = nullptr,
                                  const Ty* objectType = nullptr)
    {
      for (Decl* p : f->templateParams)
        if (!subst.find(p) || types.isDependent(subst.find(p)) || !subst.find(p)->isKnown())
          return nullptr;
      if (!f->body || instantiationDepth > 16)
        return nullptr;
      if (classSubst)
        for (const Subst::Pair& pair : classSubst->pairs)
          if (!pair.type->isKnown() || types.isDependent(pair.type))
            return nullptr;
      Subst full = subst;
      if (classSubst)
        for (const Subst::Pair& pair : classSubst->pairs)
          if (!full.find(pair.param))
            full.pairs.push_back(pair);
      bool producer;
      Impl::Instantiation* inst = impl.claimFunction(f, full, producer);
      if (!producer)
      {
        if (!needReturn)
          return nullptr;
        if (inst->owner == std::this_thread::get_id())
          return nullptr; // Recursive auto function.
        if (inst->state.load(std::memory_order_acquire) != 2)
        {
          ++impl.instWaits;
          while (inst->state.load(std::memory_order_acquire) != 2)
            std::this_thread::yield();
        }
        return inst->deducedReturn;
      }
      auto start = std::chrono::steady_clock::now();
      Scope* bodyScope = f->name.parts.size() > 1 ? semanticScopeFor(f->name) : impl.lexicalScopeOf(f);
      const Ty* bodyThis = nullptr;
      if (objectType)
        bodyThis = types.pointer(types.unqualified(types.stripRef(objectType)));
      else if (Decl* cls = enclosingClassDecl(bodyScope))
        bodyThis = types.pointer(impl.substitute(impl.selfType(cls), full));
      const Ty* deduced = nullptr;
      subCheck(f, full, bodyScope, bodyThis, [&](Walker& sub)
      {
        sub.returnFrames.push_back(nullptr);
        sub.walkDecl(f);
        deduced = sub.returnFrames.back();
        sub.returnFrames.pop_back();
        ++impl.instBodies;
      });
      inst->deducedReturn = deduced ? types.decay(deduced) : nullptr;
      impl.instNanos += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count());
      inst->state.store(2, std::memory_order_release);
      return inst->deducedReturn;
    }

    // --- names ---

    void resolveTypeName(NamedType* t)
    {
      if (t->name.empty())
        return;
      Lookup l = impl.lookupQualified(t->name, scope, &locals, 0, activeSubst);
      t->resolved = l.decl;
      if (l.found == Found::No)
      {
        if (t->name.parts.size() == 1 && impl.builtinByName(t->name.parts[0].symbol))
          ++stats.typesBuiltinByName;
        else if (t->name.parts.size() == 1 && t->name.parts[0].hasTemplateArgs && impl.isTypeTraitName(t->name.parts[0].symbol))
          ++stats.typesResolved; // RemoveConst<T> and friends
        else
        {
          ++stats.typesUnresolved;
          countUnresolved(t->name, t->token);
        }
      }
      else if (l.found == Found::Dependent)
        ++stats.typesDependent;
      else
        ++stats.typesResolved;
      for (const NamePart& part : t->name.parts)
        for (Node* arg : part.templateArgs)
          walkNode(arg);
    }

    // Converts a type written in the current context.
    const Ty* typeOf(Type* t) { return impl.convertType(t, ctx(), 0); }

    // Type of a name expression from the declaration it resolved to.
    // `auto [a, b] = pair;`: the type of the binding named `symbol`, the matching data member (or
    // array element) of the hidden object the declaration names; null when symbol is no binding.
    const Ty* bindingType(VariableDecl* v, uint32_t symbol)
    {
      size_t index = v->structuredBindings.size();
      for (size_t i = 0; i < v->structuredBindings.size(); ++i)
        if (static_cast<NameExpr*>(v->structuredBindings[i])->name.last().symbol == symbol)
          index = i;
      if (index == v->structuredBindings.size())
        return nullptr;
      const Ty* t = impl.declaredTypeOf(v, activeSubst, 0);
      if (t && impl.containsAuto(t))
        t = v->init && v->init->ty ? impl.deduceAuto(t, v->init->ty) : nullptr;
      if (!t || !t->isKnown())
        return unknown(Why::Auto);
      bool isConst = types.stripRef(t)->isConst;
      const Ty* object = types.unqualified(types.stripRef(t));
      if (object->kind == TyKind::Array)
        return isConst ? types.qualified(object->inner, true, false) : object->inner;
      if (object->kind != TyKind::Class)
        return unknown(Why::Unsupported);
      Scope* cs = impl.classScopeOf(object);
      if (!cs || !cs->owner || cs->owner->kind != NodeKind::ClassDecl)
        return unknown(Why::Unsupported);
      size_t seen = 0;
      for (Decl* m : static_cast<ClassDecl*>(cs->owner)->members)
      {
        if (m->kind != NodeKind::VariableDecl || (m->flags & (DF_Static | DF_Typedef)))
          continue;
        if (seen++ == index)
        {
          Subst s = object->args.empty() ? Subst{} : impl.substFor(object);
          const Ty* mt = impl.declaredTypeOf(m, object->args.empty() ? activeSubst : &s, 0);
          return mt && isConst ? types.qualified(mt, true, false) : mt;
        }
      }
      return unknown(Why::Unsupported);
    }

    const Ty* typeOfResolvedName(const Lookup& l, const NamePart& lastPart, Expr* e)
    {
      Decl* d = l.decl;
      if (!d)
        return unknown(Why::Name);
      switch (d->kind)
      {
        case NodeKind::VariableDecl:
        {
          if (auto* v = static_cast<VariableDecl*>(d); !v->structuredBindings.empty())
            if (const Ty* bt = bindingType(v, lastPart.symbol))
              return bt;
          const Ty* t = impl.declaredTypeOf(d, activeSubst, 0);
          if (impl.containsAuto(t))
          {
            // A local whose initializer was typed already has its deduced type cached by the
            // walker; otherwise (members/globals in other files) deduce from the initializer now.
            auto* v = static_cast<VariableDecl*>(d);
            if (v->init && v->init->ty)
              return impl.deduceAuto(t, v->init->ty);
            return unknown(Why::Auto);
          }
          return t;
        }
        case NodeKind::ParamDecl:
        {
          const Ty* t = impl.declaredTypeOf(d, activeSubst, 0);
          return impl.containsAuto(t) ? unknown(Why::Auto) : t;
        }
        case NodeKind::EnumeratorDecl:
          return impl.declaredTypeOf(d, nullptr, 0);
        case NodeKind::FunctionDecl:
          return impl.declaredTypeOf(d, activeSubst, 0);
        case NodeKind::ClassDecl:
        case NodeKind::EnumDecl:
        case NodeKind::UsingAliasDecl:
        {
          // A type used as an expression: constructor call `Foo(x)`, `Foo{}` or `Enum(x)`.
          TypeContext c = ctx();
          return impl.typeOfTypeDecl(d, l, lastPart, c, 0);
        }
        case NodeKind::TemplateParamDecl:
          return impl.declaredTypeOf(d, activeSubst, 0);
        case NodeKind::NamespaceDecl:
          return unknown(Why::Unsupported);
        default:
          (void)e;
          return unknown(Why::Unsupported);
      }
    }

    const Ty* resolveName(NameExpr* e)
    {
      if (e->name.empty())
        return unknown(Why::Unsupported);
      if (e->name.parts.size() == 1 && e->name.parts[0].hasTemplateArgs && impl.isBoolTraitName(e->name.parts[0].symbol))
      {
        // A builtin trait: a bool the code generator evaluates once the arguments are known.
        for (Node* arg : e->name.parts[0].templateArgs)
          walkNode(arg);
        e->resolved = nullptr;
        ++stats.namesGlobal;
        return types.boolType();
      }
      Lookup l = impl.lookupQualified(e->name, scope, &locals, 0, activeSubst);
      e->resolved = l.decl;
      for (const NamePart& part : e->name.parts)
        for (Node* arg : part.templateArgs)
          walkNode(arg);
      switch (l.found)
      {
        case Found::No:
          ++stats.namesUnresolved;
          countUnresolved(e->name, e->token);
          if (e->name.parts.size() == 1)
            if (const Ty* b = impl.builtinByName(e->name.parts[0].symbol))
              return b; // `uint32_t(x)`
          return unknown(Why::Name);
        case Found::Local: ++stats.namesLocal; break;
        case Found::Member: ++stats.namesMember; break;
        case Found::Global: ++stats.namesGlobal; break;
        case Found::Dependent: ++stats.namesDependent; break;
      }
      if (l.found == Found::Dependent)
        return l.decl ? impl.declaredTypeOf(l.decl, activeSubst, 0) : types.unknown();
      // Members of the current class template keep their dependent types; members reached
      // through a qualified name on an instantiation get substituted.
      if (e->name.parts.size() > 1 && l.found == Found::Member && l.foundIn && l.foundIn->owner &&
          !l.foundIn->owner->templateParams.empty())
      {
        // Qualifier like `Vector<int>::npos`: substitute from the qualifier type when it is an instantiation.
        QualifiedName qualifier = e->name;
        qualifier.parts.count -= 1;
        NamedType tmp{};
        tmp.kind = NodeKind::NamedType;
        tmp.name = qualifier;
        const Ty* q = typeOf(&tmp);
        if (q->kind == TyKind::Class && !q->args.empty())
        {
          Subst s = impl.substFor(q);
          return impl.declaredTypeOf(l.decl, &s, 0);
        }
      }
      return typeOfResolvedName(l, e->name.last(), e);
    }

    // --- member access through the object's type ---

    struct MemberResult
    {
      const Ty* type = nullptr;
      Decl* decl = nullptr;
      const std::vector<Entry>* overloads = nullptr;
      Subst subst;
      Why why = Why::None;
    };

    // Follows operator-> on class types for `a->b`.
    const Ty* objectForArrow(const Ty* t, int depth = 0)
    {
      t = types.stripRef(t);
      if (!t || depth > 4)
        return t;
      if (t->kind == TyKind::Pointer)
        return t->inner;
      if (t->kind == TyKind::Class)
      {
        MemberResult arrow = memberOf(t, impl.opSymbols[uint32_t(P::Arrow)], nullptr, 0, false);
        if (arrow.type && arrow.type->kind == TyKind::Function)
          return objectForArrow(arrow.type->inner, depth + 1);
        return types.unknown();
      }
      return t;
    }

    // Looks a member up in the class of `objectTy` (after stripping references and const).
    MemberResult memberOf(const Ty* objectTy, uint32_t symbol, const QualifiedName* name, uint32_t token, bool countMissing = true)
    {
      MemberResult r;
      const Ty* t = types.stripRef(objectTy);
      if (!t || !t->isKnown())
      {
        r.why = Why::Base;
        return r;
      }
      if (t->kind == TyKind::Param || (t->kind == TyKind::Class && types.isDependent(t) && !t->decl))
      {
        r.type = types.param(t->decl);
        return r;
      }
      if (t->kind != TyKind::Class && t->kind != TyKind::Enum)
      {
        r.why = Why::Base;
        return r;
      }
      Scope* cls = t->decl ? t->decl->ownScope : nullptr;
      if (!cls)
      {
        // Only a forward declaration was seen.
        Entry e{t->decl, nullptr};
        cls = impl.scopeOfEntry(e, 0);
        if (!cls)
        {
          r.why = Why::Base;
          return r;
        }
      }
      Lookup l;
      if (name && name->parts.size() > 1)
      {
        // `obj.Base::member`
        l = impl.lookupQualified(*name, cls, nullptr, 0);
      }
      else if (!impl.lookupInScope(cls, symbol, l, 0))
      {
        if (types.isDependent(t))
        {
          r.type = types.unknown();
          r.why = Why::None; // Member of a dependent base: cannot know yet.
          if (countMissing)
            ++stats.membersDependent;
          return r;
        }
        if (hasUnknownBase(cls, 0))
        {
          // A base class outside the corpus (std::vector...) may declare it.
          r.why = Why::Base;
          if (countMissing)
          {
            ++stats.membersUnknownBase;
            ++stats.unknownBaseMembers[symbol];
          }
          return r;
        }
        r.why = Why::Member;
        if (countMissing)
        {
          ++stats.membersNotFound;
          ++stats.missingMembers[symbol];
          stats.missingMembersFirst.emplace(symbol, ResolveStats::Where{stats.fileIndex, token});
        }
        return r;
      }
      if (l.decl && l.decl->kind == NodeKind::UsingNameDecl && !static_cast<UsingNameDecl*>(l.decl)->isEnum)
      {
        // `using Base::f;` in the class: f means the base's overloads.
        Lookup target = impl.lookupQualified(static_cast<UsingNameDecl*>(l.decl)->name, cls, nullptr, 0);
        if (target.found != Found::No && target.decl)
        {
          l.decl = target.decl;
          l.overloads = target.overloads;
        }
      }
      if (l.found == Found::No || !l.decl)
      {
        r.why = Why::Member;
        return r;
      }
      r.decl = l.decl;
      r.overloads = l.overloads;
      r.subst = impl.substFor(t);
      if (!r.subst.empty())
        instantiateClass(types.unqualified(t));
      if (l.found == Found::Dependent)
      {
        r.type = types.param(l.decl);
        return r;
      }
      const Ty* declared = impl.declaredTypeOf(l.decl, &r.subst, 0);
      if (impl.containsAuto(declared))
      {
        auto* v = l.decl->kind == NodeKind::VariableDecl ? static_cast<VariableDecl*>(l.decl) : nullptr;
        declared = v && v->init && v->init->ty ? impl.deduceAuto(declared, v->init->ty) : types.unknown();
        if (!declared->isKnown())
          r.why = Why::Auto;
      }
      r.type = declared;
      return r;
    }

    // True if the class or one of its bases has a base type that could not be resolved.
    bool hasUnknownBase(Scope* cls, int depth)
    {
      if (!cls || depth > 16 || cls->baseTypes.empty())
        return false;
      impl.ensureBases(cls, 0);
      if (cls->baseScopes.size() < cls->baseTypes.size())
        return true;
      for (Scope* b : cls->baseScopes)
        if (hasUnknownBase(b, depth + 1))
          return true;
      return false;
    }

    const Ty* typeOfMember(MemberExpr* m)
    {
      const Ty* base = walkExpr(m->base);
      for (const NamePart& part : m->member.parts)
        for (Node* arg : part.templateArgs)
          walkNode(arg);
      if (m->member.empty())
        return unknown(Why::Unsupported);
      const NamePart& last = m->member.last();
      uint32_t symbol = last.special == 2 && last.opToken < uint32_t(P::Count) ? impl.opSymbols[last.opToken] : last.symbol;
      if (symbol == UINT32_MAX)
      {
        if (last.special == 1)
        {
          // obj.~Foo()
          ++stats.membersBound;
          return types.function(types.voidType(), {}, false);
        }
        return unknown(Why::Unsupported);
      }
      const Ty* object = base;
      if (m->op == P::Arrow || m->op == P::SafeArrow)
        object = objectForArrow(base);
      if (!object || !object->isKnown())
      {
        ++stats.membersUnknownBase;
        ++stats.unknownBaseMembers[symbol];
        return unknown(Why::Base);
      }
      MemberResult r = memberOf(object, symbol, &m->member, m->token);
      m->resolved = r.decl;
      if (r.decl)
      {
        if (r.type && types.isDependent(r.type))
          ++stats.membersDependent;
        else
          ++stats.membersBound;
      }
      else if (r.why == Why::Base)
        ++stats.membersUnknownBase;
      if (r.why != Why::None)
        return unknown(r.why);
      if (r.type && r.type->kind == TyKind::Function && last.special == 0)
      {
        // `obj.method` used as a value is only meaningful in a call, which handles overloads.
      }
      return r.type ? r.type : types.unknown();
    }

    // --- calls ---

    // Picks a function among overloads by argument count and conversion ranking.
    FunctionDecl* pickOverload(Decl* primary, const std::vector<Entry>* overloads, const std::vector<const Ty*>& argTypes,
                               const Subst* classSubst, const Ty* objectTy, bool& ambiguous)
    {
      std::vector<FunctionDecl*> candidates;
      if (overloads)
      {
        for (const Entry& e : *overloads)
          if (e.decl && e.decl->kind == NodeKind::FunctionDecl)
            candidates.push_back(static_cast<FunctionDecl*>(e.decl));
      }
      else if (primary && primary->kind == NodeKind::FunctionDecl)
        candidates.push_back(static_cast<FunctionDecl*>(primary));
      return impl.chooseAmong(candidates, argTypes, classSubst, objectTy, ambiguous);
    }

    // Argument types of a call; a pack expansion `args...` contributes one type per element.
    // Lvalues present themselves as lvalue references (argumentType).
    std::vector<const Ty*> argTypesOf(const Span<Expr*>& args)
    {
      std::vector<const Ty*> out;
      for (Expr* a : args)
      {
        if (a && a->ty && a->ty->kind == TyKind::Pack)
          for (const Ty* e : a->ty->args)
            out.push_back(argumentType(a, e, types));
        else if (a && a->kind == NodeKind::BraceInitExpr)
          out.push_back(braceListType(a, impl.initializerListDecl(), types));
        else
          out.push_back(a ? argumentType(a, a->ty, types) : nullptr);
      }
      return out;
    }

    // Return type of calling `f` with the given arguments, deducing its template parameters.
    const Ty* returnTypeOfCall(FunctionDecl* f, const Subst* classSubst, const Span<Expr*>& args, const NamePart* namePart,
                               CallExpr* call = nullptr)
    {
      if (f->body == nullptr && f->returnType == nullptr && !(f->flags & (DF_Constructor | DF_Destructor | DF_Conversion)))
        return unknown(Why::Unsupported);
      bool autoReturn = false;
      if (f->returnType && f->returnType->kind == NodeKind::AutoType && !f->trailingReturn)
        autoReturn = true;
      if (autoReturn)
      {
        if (!f->templateParams.empty())
        {
          Subst s = classSubst ? *classSubst : Subst{};
          const Ty* fnTy = impl.declaredTypeOf(f, classSubst, 0);
          if (namePart)
            for (size_t i = 0; i < namePart->templateArgs.size() && i < f->templateParams.size(); ++i)
              s.pairs.push_back({f->templateParams[i], impl.templateArgument(namePart->templateArgs[i], f->templateParams[i], ctx(), 0)});
          impl.deduceCall(f, fnTy, argTypesOf(args), s);
          if (const Ty* r = instantiateFunction(f, s, true, classSubst))
            return r;
          return unknown(Why::AutoReturn);
        }
        if (const Ty* cached = f->cachedType.load(std::memory_order_acquire))
          if (cached->kind == TyKind::Function && cached->inner->kind != TyKind::Auto)
            return classSubst ? impl.substitute(cached->inner, *classSubst) : cached->inner;
        return unknown(Why::AutoReturn);
      }
      Subst subst = classSubst ? *classSubst : Subst{};
      const Ty* fnTy = impl.declaredTypeOf(f, classSubst ? classSubst : activeSubst, 0);
      if (!f->templateParams.empty())
      {
        // Explicit template arguments first, then deduction from the arguments.
        if (namePart)
          for (size_t i = 0; i < namePart->templateArgs.size() && i < f->templateParams.size(); ++i)
            subst.pairs.push_back({f->templateParams[i], impl.templateArgument(namePart->templateArgs[i], f->templateParams[i], ctx(), 0)});
        impl.deduceCall(f, fnTy, argTypesOf(args), subst);
        if (call)
        {
          call->templateArgCount = 0;
          for (Decl* p : f->templateParams)
          {
            const Ty* t = subst.find(p);
            if (!t || call->templateArgCount >= 4)
              break;
            call->templateArgs[call->templateArgCount++] = t;
          }
        }
        {
          static bool debugCalls = getenv("KOV_DEBUG_CALLS") != nullptr;
          if (debugCalls)
          {
            std::string line = std::string(call ? "call " : "call(no expr) ") + std::string(impl.symbols.name(f->name.last().symbol)) + ":";
            for (size_t i = 0; call && i < call->templateArgCount; ++i)
              line += " " + types.toString(call->templateArgs[i], &impl.symbols);
            line += " | explicit:";
            if (namePart)
              for (size_t i = 0; i < namePart->templateArgs.size() && i < f->templateParams.size(); ++i)
                line += " " + types.toString(impl.templateArgument(namePart->templateArgs[i], f->templateParams[i], ctx(), 0), &impl.symbols);
            line += " | subst:";
            if (activeSubst)
              for (auto& [d, ty] : activeSubst->pairs)
                line += " " + std::string(impl.symbols.name(static_cast<TemplateParamDecl*>(d)->name)) + "=" + types.toString(ty, &impl.symbols);
            fprintf(stderr, "%s\n", line.c_str());
          }
        }
        const Ty* result = impl.substitute(fnTy->inner, subst);
        if (impl.dependsOn(result, f->templateParams))
          return unknown(Why::Deduction);
        if (!types.isDependent(result))
          instantiateFunction(f, subst, false, classSubst);
        return result;
      }
      return fnTy->inner;
    }

    const Ty* typeOfCall(CallExpr* c)
    {
      for (Expr* a : c->args)
        walkExpr(a);
      Expr* callee = c->callee;
      bool ambiguous = false;
      if (callee->kind == NodeKind::NameExpr)
      {
        auto* n = static_cast<NameExpr*>(callee);
        Lookup l = impl.lookupQualified(n->name, scope, &locals, 0, activeSubst);
        n->resolved = l.decl;
        for (const NamePart& part : n->name.parts)
          for (Node* arg : part.templateArgs)
            walkNode(arg);
        switch (l.found)
        {
          case Found::No:
            ++stats.namesUnresolved;
            countUnresolved(n->name, n->token);
            if (n->name.parts.size() == 1)
              if (const Ty* b = impl.builtinByName(n->name.parts[0].symbol))
              {
                record(n, b);
                return b;
              }
            record(n, unknown(Why::Name));
            return types.unknown();
          case Found::Local: ++stats.namesLocal; break;
          case Found::Member: ++stats.namesMember; break;
          case Found::Global: ++stats.namesGlobal; break;
          case Found::Dependent: ++stats.namesDependent; break;
        }
        if (l.found == Found::Dependent)
        {
          const Ty* t = l.decl ? impl.declaredTypeOf(l.decl, activeSubst, 0) : types.unknown();
          record(n, t);
          return t;
        }
        Decl* d = l.decl;
        if (d && d->kind == NodeKind::FunctionDecl)
        {
          FunctionDecl* f = pickOverload(d, l.overloads, argTypesOf(c->args), nullptr, nullptr, ambiguous);
          if (!f)
          {
            record(n, types.unknown());
            return unknown(Why::Overload);
          }
          if (ambiguous)
            ++stats.callsAmbiguousArity;
          ++stats.callsResolved;
          n->resolved = f;
          const Ty* fnTy = impl.declaredTypeOf(f, nullptr, 0);
          record(n, fnTy);
          return returnTypeOfCall(f, nullptr, c->args, &n->name.last(), c);
        }
        // Constructor call, functional cast, callable variable...
        const Ty* t = typeOfResolvedName(l, n->name.last(), n);
        record(n, t);
        if (d && (d->kind == NodeKind::ClassDecl || d->kind == NodeKind::EnumDecl || d->kind == NodeKind::UsingAliasDecl ||
                  (d->kind == NodeKind::VariableDecl && (d->flags & DF_Typedef))))
        {
          ++stats.callsResolved; // a functional cast, `int64_t(x)` included when int64_t is an imported typedef
          return t;
        }
        return callResultOfValue(t, c);
      }
      if (callee->kind == NodeKind::MemberExpr)
      {
        auto* m = static_cast<MemberExpr*>(callee);
        const Ty* base = walkExpr(m->base);
        for (const NamePart& part : m->member.parts)
          for (Node* arg : part.templateArgs)
            walkNode(arg);
        const Ty* object = (m->op == P::Arrow || m->op == P::SafeArrow) ? objectForArrow(base) : base;
        if (m->member.empty())
        {
          record(m, types.unknown());
          return unknown(Why::Unsupported);
        }
        const NamePart& last = m->member.last();
        if (!object || !object->isKnown())
        {
          ++stats.membersUnknownBase;
          if (last.symbol != UINT32_MAX)
            ++stats.unknownBaseMembers[last.symbol];
          record(m, types.unknown());
          return unknown(Why::Base);
        }
        if (last.special == 1)
        {
          ++stats.membersBound;
          record(m, types.function(types.voidType(), {}, false));
          return types.voidType();
        }
        uint32_t symbol = last.special == 2 && last.opToken < uint32_t(P::Count) ? impl.opSymbols[last.opToken] : last.symbol;
        if (symbol == UINT32_MAX)
        {
          record(m, types.unknown());
          return unknown(Why::Unsupported);
        }
        MemberResult r = memberOf(object, symbol, &m->member, m->token);
        m->resolved = r.decl;
        if (!r.decl)
        {
          if (r.why == Why::Base)
            ++stats.membersUnknownBase;
          record(m, types.unknown());
          return r.why == Why::None ? types.unknown() : unknown(r.why);
        }
        if (r.decl->kind == NodeKind::FunctionDecl)
        {
          FunctionDecl* f = pickOverload(r.decl, r.overloads, argTypesOf(c->args), &r.subst, object, ambiguous);
          if (!f)
          {
            ++stats.membersBound;
            record(m, types.unknown());
            return unknown(Why::Overload);
          }
          if (ambiguous)
            ++stats.callsAmbiguousArity;
          ++stats.callsResolved;
          m->resolved = f;
          const Ty* fnTy = impl.declaredTypeOf(f, &r.subst, 0);
          record(m, fnTy);
          if (types.isDependent(fnTy))
            ++stats.membersDependent;
          else
            ++stats.membersBound;
          return returnTypeOfCall(f, &r.subst, c->args, &last, c);
        }
        if (r.type && types.isDependent(r.type))
          ++stats.membersDependent;
        else
          ++stats.membersBound;
        record(m, r.type ? r.type : types.unknown());
        if (r.why != Why::None)
          return unknown(r.why);
        return callResultOfValue(r.type, c);
      }
      const Ty* t = walkExpr(callee);
      return callResultOfValue(t, c);
    }

    // Result of calling a value of type t: function pointers, lambdas, functors.
    const Ty* callResultOfValue(const Ty* t, CallExpr* c)
    {
      t = types.stripRef(t);
      if (!t || !t->isKnown())
        return types.unknown();
      if (t->kind == TyKind::Function)
        return t->inner;
      if (t->kind == TyKind::Pointer && t->inner->kind == TyKind::Function)
        return t->inner->inner;
      if (t->kind == TyKind::Lambda)
        return t->inner ? t->inner : unknown(Why::AutoReturn);
      if (t->kind == TyKind::Class)
      {
        MemberResult r = memberOf(t, impl.opSymbols[uint32_t(P::LParen)], nullptr, c->token, false);
        if (r.decl && r.decl->kind == NodeKind::FunctionDecl)
        {
          bool ambiguous;
          if (FunctionDecl* f = pickOverload(r.decl, r.overloads, argTypesOf(c->args), &r.subst, t, ambiguous))
            return returnTypeOfCall(f, &r.subst, c->args, nullptr);
          return unknown(Why::Overload);
        }
        return r.why == Why::Member ? unknown(Why::Member) : types.unknown();
      }
      if (t->kind == TyKind::Param)
        return t;
      return unknown(Why::Unsupported);
    }

    // --- operators ---

    const Ty* promote(const Ty* t)
    {
      t = types.unqualified(types.stripRef(t));
      switch (t->kind)
      {
        case TyKind::Bool: case TyKind::Char: return types.intType(32);
        case TyKind::Int: return t->bits < 32 ? types.intType(32) : t;
        case TyKind::Enum: return types.intType(32);
        default: return t;
      }
    }

    const Ty* arithmetic(const Ty* a, const Ty* b)
    {
      a = promote(a);
      b = promote(b);
      if (a->kind == TyKind::Float || b->kind == TyKind::Float)
      {
        uint8_t bits = std::max<uint8_t>(a->kind == TyKind::Float ? a->bits : 0, b->kind == TyKind::Float ? b->bits : 0);
        return types.floatType(bits);
      }
      if (a->kind != TyKind::Int || b->kind != TyKind::Int)
        return a->kind == TyKind::Int ? a : b;
      if (a->bits != b->bits)
        return a->bits > b->bits ? a : b;
      return types.intType(a->bits, a->isUnsigned || b->isUnsigned);
    }

    // Overloaded operator lookup: member of the left operand's class, then free functions in
    // the class's namespace and the current scope chain.
    const Ty* overloadedOperator(P op, const Ty* lhs, const Ty* rhs, uint32_t token, size_t arity)
    {
      uint32_t symbol = impl.opSymbols[uint32_t(op)];
      const Ty* l = types.stripRef(lhs);
      if (l && l->kind == TyKind::Class)
      {
        MemberResult r = memberOf(l, symbol, nullptr, token, false);
        if (r.decl && r.decl->kind == NodeKind::FunctionDecl)
        {
          bool ambiguous;
          Span<Expr*> none;
          std::vector<const Ty*> argTypes;
          if (arity == 2)
            argTypes.push_back(rhs);
          if (FunctionDecl* f = pickOverload(r.decl, r.overloads, argTypes, &r.subst, lhs, ambiguous))
            return returnTypeOfCall(f, &r.subst, none, nullptr);
        }
      }
      // Free function: look in the namespaces of both operand classes and the current scope.
      std::vector<Scope*> places;
      for (const Ty* t : {l, types.stripRef(rhs)})
        if (t && t->kind == TyKind::Class && t->decl)
          places.push_back(impl.lexicalScopeOf(t->decl));
      places.push_back(scope);
      for (Scope* place : places)
      {
        for (Scope* s = place; s; s = s->parent)
        {
          Lookup look;
          if (!impl.lookupInScope(s, symbol, look, 0))
            continue;
          if (look.decl && look.decl->kind == NodeKind::FunctionDecl)
          {
            bool ambiguous;
            Span<Expr*> none;
            std::vector<const Ty*> argTypes{lhs};
            if (arity == 2)
              argTypes.push_back(rhs);
            if (FunctionDecl* f = pickOverload(look.decl, look.overloads, argTypes, nullptr, nullptr, ambiguous))
              return returnTypeOfCall(f, nullptr, none, nullptr);
          }
          break;
        }
      }
      // Operators every class has or that always yield bool.
      switch (op)
      {
        case P::Assign: return lhs;                       // implicit copy/move assignment
        case P::AndAnd: case P::OrOr: case P::Bang: return types.boolType();
        case P::EqEq: case P::NotEq: case P::Less: case P::Greater: case P::LessEq: case P::GreaterEq:
          return l && l->kind == TyKind::Enum ? types.boolType() : unknown(Why::Operator);
        default: return unknown(Why::Operator);
      }
    }

    const Ty* typeOfBinary(BinaryExpr* b)
    {
      const Ty* l = walkExpr(b->lhs);
      const Ty* r = walkExpr(b->rhs);
      if (b->isKeyword)
      {
        switch (Kw(b->op))
        {
          case Kw::and_: case Kw::or_: case Kw::not_eq_: return types.boolType();
          default: return arithmetic(l, r);
        }
      }
      P op = P(b->op);
      if (op == P::Comma)
        return r;
      const Ty* ls = types.stripRef(l);
      const Ty* rs = types.stripRef(r);
      if (!ls || !rs)
        return types.unknown();
      if (op == P::DotStar || op == P::ArrowStar)
        return rs->kind == TyKind::MemberPointer ? types.lref(rs->inner) : types.unknown();
      if (ls->kind == TyKind::Class || rs->kind == TyKind::Class)
      {
        if (!ls->isKnown() || !rs->isKnown())
          return types.unknown();
        if (types.isDependent(ls) || types.isDependent(rs))
          return types.param(ls->kind == TyKind::Param ? ls->decl : rs->decl);
        return overloadedOperator(op, l, argumentType(b->rhs, r, types), b->token, 2);
      }
      if (!ls->isKnown() || !rs->isKnown())
      {
        // Comparisons and logic have a known result regardless of the operands.
        switch (op)
        {
          case P::EqEq: case P::NotEq: case P::Less: case P::Greater: case P::LessEq: case P::GreaterEq:
          case P::AndAnd: case P::OrOr:
            return types.boolType();
          default:
            if (op == P::Assign || op == P::PlusAssign || op == P::MinusAssign || op == P::StarAssign ||
                op == P::SlashAssign || op == P::PercentAssign || op == P::AmpAssign || op == P::PipeAssign ||
                op == P::CaretAssign || op == P::ShlAssign || op == P::ShrAssign)
              return ls->isKnown() ? l : types.unknown();
            return types.unknown();
        }
      }
      if (ls->kind == TyKind::Param || rs->kind == TyKind::Param)
        return ls->kind == TyKind::Param ? ls : rs;
      switch (op)
      {
        case P::Assign: case P::PlusAssign: case P::MinusAssign: case P::StarAssign: case P::SlashAssign:
        case P::PercentAssign: case P::AmpAssign: case P::PipeAssign: case P::CaretAssign: case P::ShlAssign:
        case P::ShrAssign:
          return l;
        case P::EqEq: case P::NotEq: case P::Less: case P::Greater: case P::LessEq: case P::GreaterEq:
        case P::AndAnd: case P::OrOr:
          return types.boolType();
        case P::Spaceship:
          return types.intType();
        case P::Shl: case P::Shr:
          return promote(l);
        case P::Plus: case P::Minus:
        {
          bool lp = ls->kind == TyKind::Pointer || ls->kind == TyKind::Array;
          bool rp = rs->kind == TyKind::Pointer || rs->kind == TyKind::Array;
          if (lp && rp)
            return types.ptrdiffType();
          if (lp)
            return types.decay(ls);
          if (rp)
            return types.decay(rs);
          return arithmetic(l, r);
        }
        case P::Star: case P::Slash: case P::Percent: case P::Amp: case P::Pipe: case P::Caret:
          return arithmetic(l, r);
        case P::DotStar: case P::ArrowStar:
          return rs->kind == TyKind::MemberPointer ? types.lref(rs->inner) : types.unknown();
        default:
          return unknown(Why::Unsupported);
      }
    }

    const Ty* typeOfUnary(UnaryExpr* u)
    {
      const Ty* t = walkExpr(u->operand);
      const Ty* s = types.stripRef(t);
      if (u->isKeyword)
        return Kw(u->op) == Kw::not_ ? types.boolType() : promote(t);
      P op = P(u->op);
      if (op == P::Ellipsis)
        return t;
      if (!s || !s->isKnown())
        return op == P::Bang ? types.boolType() : types.unknown();
      if (op == P::Amp && u->operand->kind == NodeKind::NameExpr)
      {
        // `&Style::field`: a pointer to a member of the class, not to the field of some object.
        auto* n = static_cast<NameExpr*>(u->operand);
        Decl* d = n->resolved;
        if (n->name.parts.size() > 1 && d && (d->kind == NodeKind::VariableDecl || d->kind == NodeKind::FunctionDecl) &&
            !(d->flags & DF_Static) && d->parent && d->parent->kind == NodeKind::ClassDecl)
          return types.memberPointer(s, impl.selfType(d->parent));
      }
      if (s->kind == TyKind::Param)
        return op == P::Bang ? types.boolType() : op == P::Amp ? types.pointer(s) : s;
      if (s->kind == TyKind::Class)
      {
        if (op == P::Amp)
          return types.pointer(s);
        if (types.isDependent(s))
          return types.unknown();
        return overloadedOperator(op, t, nullptr, u->token, 1);
      }
      switch (op)
      {
        case P::Star:
          if (s->kind == TyKind::Pointer || s->kind == TyKind::Array)
            return types.lref(s->inner);
          return unknown(Why::Operator);
        case P::Amp: return types.pointer(s);
        case P::Bang: return types.boolType();
        case P::Minus: case P::Plus: case P::Tilde: return promote(t);
        case P::PlusPlus: case P::MinusMinus: return u->postfix ? types.unqualified(s) : t;
        default: return unknown(Why::Unsupported);
      }
    }

    // --- literals ---

    const Ty* typeOfLiteral(LiteralExpr* l)
    {
      switch (l->literalKind)
      {
        case TokenKind::Keyword:
          return l->keyword == Kw::nullptr_ ? types.nullptrType() : types.boolType();
        case TokenKind::Char:
        {
          std::string_view s = spelling(l->token);
          if (s.starts_with("u8")) return types.charType(8, true);
          if (s.starts_with("u")) return types.charType(16, true);
          if (s.starts_with("U")) return types.charType(32, true);
          if (s.starts_with("L")) return types.charType(16, true);
          return types.charType(8, false);
        }
        case TokenKind::String:
        {
          std::string_view s = spelling(l->token);
          const Ty* ch = types.charType(8, false);
          if (s.starts_with("u8")) ch = types.charType(8, true);
          else if (s.starts_with("u")) ch = types.charType(16, true);
          else if (s.starts_with("U")) ch = types.charType(32, true);
          else if (s.starts_with("L")) ch = types.charType(16, true);
          return types.pointer(types.qualified(ch, true, false));
        }
        case TokenKind::Number:
        {
          std::string_view s = spelling(l->token);
          bool hex = s.size() > 1 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X');
          bool isFloat = false;
          bool isUnsigned = false;
          int longs = 0;
          bool f32 = false;
          for (size_t i = hex ? 2 : 0; i < s.size(); ++i)
          {
            char c = s[i];
            if (c == '.' || (!hex && (c == 'e' || c == 'E')) || (hex && (c == 'p' || c == 'P')))
              isFloat = true;
          }
          size_t end = s.size();
          for (const char* ms : {"i64", "i32", "i16", "i8"}) // MSVC: 1i64, 1ui64
            if (!isFloat && end > strlen(ms) && s.compare(end - strlen(ms), strlen(ms), ms) == 0)
            {
              if (ms[1] == '6')
                longs = 2;
              end -= strlen(ms);
              break;
            }
          while (end > 0)
          {
            char c = s[end - 1];
            if (c == 'u' || c == 'U') { isUnsigned = true; --end; }
            else if (c == 'l' || c == 'L') { ++longs; --end; }
            else if ((c == 'f' || c == 'F') && !hex) { f32 = true; isFloat = true; --end; }
            else if (c == 'z' || c == 'Z') { --end; }
            else break;
          }
          if (isFloat)
            return types.floatType(f32 ? 32 : 64);
          // The first type the value fits: int, then (for hex and octal) unsigned int, then the
          // 64-bit ones, as C++ has it; 0xffffffff is unsigned, 4294967296 is 64 bits wide.
          std::string digits;
          for (size_t i = 0; i < end; ++i)
            if (s[i] != '\'')
              digits += s[i];
          bool binary = digits.size() > 2 && digits[0] == '0' && (digits[1] == 'b' || digits[1] == 'B');
          unsigned long long value = binary ? strtoull(digits.c_str() + 2, nullptr, 2) : strtoull(digits.c_str(), nullptr, 0);
          bool decimal = !hex && !(digits.size() > 1 && digits[0] == '0');
          if (longs < 2)
          {
            if (!isUnsigned && value <= 0x7fffffffull)
              return types.intType(32, false);
            if ((isUnsigned || !decimal) && value <= 0xffffffffull)
              return types.intType(32, true);
          }
          if (!isUnsigned && value <= 0x7fffffffffffffffull)
            return types.intType(64, false);
          return types.intType(64, true);
        }
        default:
          return types.unknown();
      }
    }

    // --- element type for range-for ---

    const Ty* elementTypeOf(const Ty* range)
    {
      const Ty* t = types.stripRef(range);
      if (!t || !t->isKnown())
        return types.unknown();
      if (t->kind == TyKind::Array || t->kind == TyKind::Pointer)
        return types.lref(t->inner);
      if (t->kind == TyKind::Class)
      {
        if (types.isDependent(t))
          return types.unknown();
        MemberResult begin = memberOf(t, impl.symBegin, nullptr, 0, false);
        const Ty* iterator = nullptr;
        if (begin.decl && begin.decl->kind == NodeKind::FunctionDecl)
        {
          bool ambiguous;
          Span<Expr*> none;
          if (FunctionDecl* f = pickOverload(begin.decl, begin.overloads, {}, &begin.subst, t, ambiguous))
            iterator = returnTypeOfCall(f, &begin.subst, none, nullptr);
        }
        if (!iterator || !iterator->isKnown())
          return types.unknown();
        const Ty* it = types.stripRef(iterator);
        if (it->kind == TyKind::Pointer)
          return types.lref(it->inner);
        if (it->kind == TyKind::Class && !types.isDependent(it))
        {
          MemberResult deref = memberOf(it, impl.opSymbols[uint32_t(P::Star)], nullptr, 0, false);
          if (deref.decl && deref.decl->kind == NodeKind::FunctionDecl)
          {
            bool ambiguous;
            Span<Expr*> none;
            if (FunctionDecl* f = pickOverload(deref.decl, deref.overloads, {}, &deref.subst, it, ambiguous))
              return returnTypeOfCall(f, &deref.subst, none, nullptr);
          }
        }
        return types.unknown();
      }
      return types.unknown();
    }

    // --- generic walking ---

    void walkNode(Node* n)
    {
      if (!n)
        return;
      if (uint8_t(n->kind) <= uint8_t(NodeKind::PackType))
        walkType(static_cast<Type*>(n));
      else if (uint8_t(n->kind) >= uint8_t(NodeKind::NameExpr))
        walkExpr(static_cast<Expr*>(n));
      else if (uint8_t(n->kind) >= uint8_t(NodeKind::BlockStmt))
        walkStmt(static_cast<Stmt*>(n));
      else
        walkDecl(static_cast<Decl*>(n));
    }

    void walkType(Type* t)
    {
      if (!t)
        return;
      switch (t->kind)
      {
        case NodeKind::NamedType: resolveTypeName(static_cast<NamedType*>(t)); break;
        case NodeKind::PointerType: walkType(static_cast<PointerType*>(t)->pointee); break;
        case NodeKind::LRefType: walkType(static_cast<LRefType*>(t)->referenced); break;
        case NodeKind::RRefType: walkType(static_cast<RRefType*>(t)->referenced); break;
        case NodeKind::ConstType: walkType(static_cast<ConstType*>(t)->inner); break;
        case NodeKind::PackType: walkType(static_cast<PackType*>(t)->pattern); break;
        case NodeKind::ArrayType:
          walkType(static_cast<ArrayType*>(t)->element);
          walkExpr(static_cast<ArrayType*>(t)->size);
          break;
        case NodeKind::FunctionType:
        {
          auto* f = static_cast<FunctionType*>(t);
          walkType(f->result);
          size_t mark = locals.mark();
          for (Decl* p : f->params)
            walkParam(static_cast<ParamDecl*>(p));
          locals.pop(mark);
          break;
        }
        case NodeKind::DecltypeType: walkExpr(static_cast<DecltypeType*>(t)->expr); break;
        case NodeKind::MemberPointerType:
          walkType(static_cast<MemberPointerType*>(t)->classType);
          walkType(static_cast<MemberPointerType*>(t)->pointee);
          break;
        default: break;
      }
    }

    void walkParam(ParamDecl* p)
    {
      walkType(p->type);
      walkExpr(p->defaultValue);
      locals.push(p->name, p);
    }

    void pushTemplateParams(Span<Decl*> params)
    {
      for (Decl* d : params)
      {
        auto* p = static_cast<TemplateParamDecl*>(d);
        walkType(p->valueType);
        walkNode(p->defaultArg);
        locals.push(p->name, p);
      }
    }

    // Scope in which the body of a (possibly out-of-line) member is resolved.
    Scope* semanticScopeFor(const QualifiedName& name)
    {
      if (name.parts.size() <= 1)
        return scope;
      QualifiedName qualifier = name;
      qualifier.parts.count -= 1;
      Lookup l = impl.lookupQualified(qualifier, scope, &locals, 0);
      return l.scope ? l.scope : scope;
    }

    const Ty* computeThisType()
    {
      Decl* cls = enclosingClassDecl(scope);
      if (!cls)
        return nullptr;
      if (cls->kind == NodeKind::EnumDecl)
        return types.lref(types.enumType(cls)); // in an enum's methods, `this` is the value
      return types.pointer(impl.selfType(cls));
    }

    void walkDecls(Span<Decl*> decls)
    {
      for (Decl* d : decls)
        walkDecl(d);
    }

    void walkDecl(Decl* d)
    {
      if (!d)
        return;
      size_t mark = locals.mark();
      if (!d->templateParams.empty())
        pushTemplateParams(d->templateParams);
      switch (d->kind)
      {
        case NodeKind::NamespaceDecl:
        {
          auto* ns = static_cast<NamespaceDecl*>(d);
          Scope* saved = scope;
          if (ns->ownScope)
            scope = ns->ownScope;
          walkDecls(ns->decls);
          scope = saved;
          break;
        }
        case NodeKind::ClassDecl:
        {
          auto* c = static_cast<ClassDecl*>(d);
          Scope* saved = scope;
          const Ty* savedThis = thisType;
          for (const ClassDecl::Base& b : c->bases)
            walkType(b.type);
          if (c->ownScope)
            scope = c->ownScope;
          else if (c->name.parts.size() > 1)
            scope = semanticScopeFor(c->name);
          if (!activeSubst || c->ownScope != scope)
            thisType = computeThisType();
          walkDecls(c->members);
          scope = saved;
          thisType = savedThis;
          break;
        }
        case NodeKind::EnumDecl:
        {
          auto* e = static_cast<EnumDecl*>(d);
          walkType(e->underlying);
          Scope* saved = scope;
          const Ty* savedThis = thisType;
          if (e->ownScope)
          {
            scope = e->ownScope;
            thisType = computeThisType(); // the enum's methods see `this` as the value
          }
          for (Decl* en : e->enumerators)
            if (en->kind == NodeKind::EnumeratorDecl)
              walkExpr(static_cast<EnumeratorDecl*>(en)->value);
          walkDecls(e->members);
          scope = saved;
          thisType = savedThis;
          break;
        }
        case NodeKind::FunctionDecl:
        {
          auto* f = static_cast<FunctionDecl*>(d);
          Scope* saved = scope;
          const Ty* savedThis = thisType;
          walkType(f->returnType);
          if (f->name.parts.size() > 1)
          {
            scope = semanticScopeFor(f->name);
            if (!activeSubst)
              thisType = computeThisType();
          }
          for (const NamePart& part : f->name.parts)
          {
            for (Node* arg : part.templateArgs)
              walkNode(arg);
            if (part.special == 3)
              walkType(part.convType);
          }
          for (Decl* p : f->params)
            walkParam(static_cast<ParamDecl*>(p));
          walkType(f->trailingReturn);
          walkExpr(f->requiresClause);
          walkExpr(f->condition);
          for (const FunctionDecl::CtorInit& init : f->ctorInits)
          {
            Lookup l = impl.lookupQualified(init.member, scope, &locals, 0);
            if (l.found == Found::No)
            {
              ++stats.namesUnresolved;
              countUnresolved(init.member, f->token);
            }
            else
              ++stats.namesMember;
            for (Expr* a : init.args)
              walkExpr(a);
          }
          bool autoReturn = f->returnType && f->returnType->kind == NodeKind::AutoType && !f->trailingReturn;
          if (autoReturn)
            returnFrames.push_back(nullptr);
          if (f->flags & DF_Static)
            thisType = nullptr;
          else if ((f->flags & DF_Const) && thisType && thisType->kind == TyKind::Pointer && !thisType->inner->isConst)
            thisType = impl.types.pointer(impl.types.qualified(thisType->inner, true, false)); // `const C* this` in a const method
          walkStmt(f->body);
          if (autoReturn)
          {
            const Ty* deduced = returnFrames.back();
            returnFrames.pop_back();
            if (deduced && deduced->isKnown() && !types.isDependent(deduced))
            {
              // Cache the deduced signature for callers in other files.
              const Ty* fnTy = impl.declaredTypeOf(f, nullptr, 0);
              std::vector<const Ty*> params(fnTy->args.begin(), fnTy->args.end());
              f->cachedType.store(types.function(types.decay(deduced), Span<const Ty*>{params.data(), uint32_t(params.size())}, fnTy->variadic),
                                  std::memory_order_release);
            }
          }
          scope = saved;
          thisType = savedThis;
          break;
        }
        case NodeKind::VariableDecl:
        {
          auto* v = static_cast<VariableDecl*>(d);
          walkType(v->type);
          walkExpr(v->bitfield);
          Scope* saved = scope;
          if (v->name.parts.size() > 1)
            scope = semanticScopeFor(v->name);
          walkExpr(v->init);
          scope = saved;
          // Deduce `auto` now that the initializer is typed, and cache it on the declaration.
          const Ty* declared = impl.declaredTypeOf(v, activeSubst, 0);
          if ((!declared || !declared->isKnown()) && v->type && !activeSubst)
          {
            // A type only the function's locals know (`using Match = Pair<int, int>;` in the body):
            // resolved here, where the locals are, and cached for everyone asking later.
            const Ty* local = typeOf(v->type);
            if (local && local->isKnown())
            {
              v->cachedType.store(local, std::memory_order_release);
              declared = local;
            }
          }
          if (v->init && !activeSubst && impl.isUndeducedClassTemplate(declared))
          {
            // `ScopedSetter guard(x, true)`: the class's arguments from a constructor (CTAD).
            Span<Expr*> args;
            if (v->init->kind == NodeKind::FunctionalCastExpr)
              args = static_cast<FunctionalCastExpr*>(v->init)->args;
            else if (v->init->kind == NodeKind::BraceInitExpr)
              args = static_cast<BraceInitExpr*>(v->init)->args;
            if (const Ty* deduced = impl.deduceClassArgs(declared, argTypesOf(args)))
            {
              v->cachedType.store(deduced, std::memory_order_release);
              if (v->init->kind == NodeKind::FunctionalCastExpr)
                record(v->init, deduced);
              declared = deduced;
            }
          }
          if (impl.containsAuto(declared))
          {
            const Ty* deduced = v->init && v->init->ty ? impl.deduceAuto(declared, v->init->ty) : types.unknown();
            if (deduced->isKnown() && !activeSubst)
              v->cachedType.store(deduced, std::memory_order_release);
            else if (deduced->isKnown())
              locals.push(UINT32_MAX, nullptr); // Keep stack shape; deduced types inside instantiations are not cached.
            else
              ++stats.unknownAuto;
          }
          break;
        }
        case NodeKind::ParamDecl: walkParam(static_cast<ParamDecl*>(d)); break;
        case NodeKind::UsingAliasDecl: walkType(static_cast<UsingAliasDecl*>(d)->type); break;
        case NodeKind::UsingNameDecl:
        {
          auto* u = static_cast<UsingNameDecl*>(d);
          Lookup l = impl.lookupQualified(u->name, scope, &locals, 0);
          if (l.found == Found::No)
          {
            ++stats.typesUnresolved;
            countUnresolved(u->name, u->token);
          }
          else
            ++stats.typesResolved;
          break;
        }
        case NodeKind::UsingDirectiveDecl:
        {
          auto* u = static_cast<UsingDirectiveDecl*>(d);
          Lookup l = impl.lookupQualified(u->name, scope, &locals, 0);
          if (l.found == Found::No)
          {
            ++stats.typesUnresolved;
            countUnresolved(u->name, u->token);
          }
          else
            ++stats.typesResolved;
          break;
        }
        case NodeKind::StaticAssertDecl: walkExpr(static_cast<StaticAssertDecl*>(d)->cond); break;
        case NodeKind::FriendDecl: walkType(static_cast<FriendDecl*>(d)->type); break;
        default: break;
      }
      locals.pop(mark);
    }

    // Declarations inside function bodies: add their names to the local stack.
    void declareLocal(Decl* d)
    {
      switch (d->kind)
      {
        case NodeKind::VariableDecl:
        {
          auto* v = static_cast<VariableDecl*>(d);
          if (v->name.parts.size() == 1)
            locals.push(v->name.parts[0].symbol, v);
          for (Expr* b : v->structuredBindings)
            locals.push(static_cast<NameExpr*>(b)->name.parts[0].symbol, v);
          break;
        }
        case NodeKind::ClassDecl:
        {
          auto* c = static_cast<ClassDecl*>(d);
          if (c->name.parts.size() == 1)
            locals.push(c->name.parts[0].symbol, c);
          break;
        }
        case NodeKind::EnumDecl:
        {
          auto* e = static_cast<EnumDecl*>(d);
          if (e->name.parts.size() == 1)
            locals.push(e->name.parts[0].symbol, e);
          for (Decl* en : e->enumerators)
            if (en->kind == NodeKind::EnumeratorDecl)
              locals.push(static_cast<EnumeratorDecl*>(en)->name, en);
          break;
        }
        case NodeKind::UsingAliasDecl: locals.push(static_cast<UsingAliasDecl*>(d)->name, d); break;
        case NodeKind::FunctionDecl:
        {
          auto* f = static_cast<FunctionDecl*>(d);
          if (f->name.parts.size() == 1 && f->name.parts[0].special == 0)
            locals.push(f->name.parts[0].symbol, f);
          break;
        }
        default: break;
      }
    }

    void walkLocalDecl(Decl* d)
    {
      // Resolve the declaration first (its type/initializer cannot see the name being declared,
      // except for classes), then make the name visible.
      if (d->kind == NodeKind::ClassDecl || d->kind == NodeKind::EnumDecl)
      {
        declareLocal(d);
        walkDecl(d);
      }
      else
      {
        walkDecl(d);
        declareLocal(d);
      }
    }

    void walkStmt(Stmt* s)
    {
      if (!s)
        return;
      switch (s->kind)
      {
        case NodeKind::BlockStmt:
        {
          size_t mark = locals.mark();
          for (Stmt* inner : static_cast<BlockStmt*>(s)->stmts)
            walkStmt(inner);
          locals.pop(mark);
          break;
        }
        case NodeKind::ExprStmt: walkExpr(static_cast<ExprStmt*>(s)->expr); break;
        case NodeKind::AssertStmt: walkExpr(static_cast<AssertStmt*>(s)->cond); break;
        case NodeKind::DeclStmt:
          for (Decl* d : static_cast<DeclStmt*>(s)->decls)
            walkLocalDecl(d);
          break;
        case NodeKind::IfStmt:
        {
          auto* i = static_cast<IfStmt*>(s);
          size_t mark = locals.mark();
          walkStmt(i->init);
          walkNode(i->cond);
          walkStmt(i->then);
          walkStmt(i->else_);
          locals.pop(mark);
          break;
        }
        case NodeKind::ForStmt:
        {
          auto* f = static_cast<ForStmt*>(s);
          size_t mark = locals.mark();
          walkStmt(f->init);
          walkNode(f->cond);
          walkExpr(f->step);
          walkStmt(f->body);
          locals.pop(mark);
          break;
        }
        case NodeKind::RangeForStmt:
        {
          auto* f = static_cast<RangeForStmt*>(s);
          size_t mark = locals.mark();
          const Ty* range = walkExpr(f->range);
          if (f->range->kind == NodeKind::BraceInitExpr)
          {
            // `for (X* kids : {&a, &b})`: an array of the first element's type.
            auto* b = static_cast<BraceInitExpr*>(f->range);
            const Ty* first = !b->args.empty() && b->args[0]->ty ? types.decay(b->args[0]->ty) : nullptr;
            if (first && first->isKnown())
              range = record(f->range, types.array(first, uint32_t(b->args.size())));
          }
          auto* v = static_cast<VariableDecl*>(f->var);
          walkType(v->type);
          const Ty* declared = impl.declaredTypeOf(v, activeSubst, 0);
          if (impl.containsAuto(declared))
          {
            const Ty* element = elementTypeOf(range);
            const Ty* deduced = element->isKnown() ? impl.deduceAuto(declared, element) : types.unknown();
            if (deduced->isKnown())
              v->cachedType.store(deduced, std::memory_order_release);
            else
              ++stats.unknownAuto;
          }
          declareLocal(v);
          walkStmt(f->body);
          locals.pop(mark);
          break;
        }
        case NodeKind::WhileStmt:
        {
          auto* w = static_cast<WhileStmt*>(s);
          size_t mark = locals.mark();
          walkNode(w->cond);
          walkStmt(w->body);
          locals.pop(mark);
          break;
        }
        case NodeKind::DoStmt:
          walkStmt(static_cast<DoStmt*>(s)->body);
          walkExpr(static_cast<DoStmt*>(s)->cond);
          break;
        case NodeKind::SwitchStmt:
        {
          auto* w = static_cast<SwitchStmt*>(s);
          size_t mark = locals.mark();
          walkStmt(w->init);
          walkNode(w->cond);
          walkStmt(w->body);
          locals.pop(mark);
          break;
        }
        case NodeKind::CaseStmt:
          walkExpr(static_cast<CaseStmt*>(s)->value);
          walkExpr(static_cast<CaseStmt*>(s)->rangeEnd);
          break;
        case NodeKind::ReturnStmt:
        {
          const Ty* t = walkExpr(static_cast<ReturnStmt*>(s)->value);
          if (!returnFrames.empty() && !returnFrames.back() && t && t->isKnown())
            returnFrames.back() = t;
          break;
        }
        case NodeKind::TryStmt:
        {
          auto* t = static_cast<TryStmt*>(s);
          walkStmt(t->body);
          for (const TryStmt::Handler& h : t->handlers)
          {
            size_t mark = locals.mark();
            if (h.param)
              walkParam(static_cast<ParamDecl*>(h.param));
            walkStmt(h.body);
            locals.pop(mark);
          }
          break;
        }
        default: break;
      }
    }

    void walkExprs(Span<Expr*> exprs)
    {
      for (Expr* e : exprs)
        walkExpr(e);
    }

    // Resolves names in and computes the type of an expression.
    const Ty* walkExpr(Expr* e)
    {
      if (!e)
        return nullptr;
      switch (e->kind)
      {
        case NodeKind::NameExpr: return record(e, resolveName(static_cast<NameExpr*>(e)));
        case NodeKind::CallerExpr: return record(e, types.unknown()); // typed by the parameter it defaults
        case NodeKind::NameofExpr:
          walkExpr(static_cast<NameofExpr*>(e)->operand);
          return record(e, types.pointer(types.qualified(types.charType(), true, false)));
        case NodeKind::LiteralExpr: return record(e, typeOfLiteral(static_cast<LiteralExpr*>(e)));
        case NodeKind::ThisExpr: return record(e, thisType ? thisType : unknown(Why::Unsupported));
        case NodeKind::CallExpr: return record(e, typeOfCall(static_cast<CallExpr*>(e)));
        case NodeKind::IndexExpr:
        {
          auto* i = static_cast<IndexExpr*>(e);
          const Ty* base = walkExpr(i->base);
          walkExprs(i->args);
          const Ty* b = types.stripRef(base);
          if (!b || !b->isKnown())
            return record(e, types.unknown());
          if (b->kind == TyKind::Pointer || b->kind == TyKind::Array)
            return record(e, types.lref(b->inner));
          if (b->kind == TyKind::Param)
            return record(e, b);
          if (b->kind == TyKind::Class)
          {
            if (types.isDependent(b))
              return record(e, types.unknown());
            MemberResult r = memberOf(b, impl.opSymbols[uint32_t(P::LBracket)], nullptr, i->token, false);
            if (r.decl && r.decl->kind == NodeKind::FunctionDecl)
            {
              bool ambiguous;
              if (FunctionDecl* f = pickOverload(r.decl, r.overloads, argTypesOf(i->args), &r.subst, b, ambiguous))
                return record(e, returnTypeOfCall(f, &r.subst, i->args, nullptr));
            }
            return record(e, unknown(Why::Operator));
          }
          return record(e, unknown(Why::Operator));
        }
        case NodeKind::MemberExpr: return record(e, typeOfMember(static_cast<MemberExpr*>(e)));
        case NodeKind::UnaryExpr: return record(e, typeOfUnary(static_cast<UnaryExpr*>(e)));
        case NodeKind::BinaryExpr: return record(e, typeOfBinary(static_cast<BinaryExpr*>(e)));
        case NodeKind::ConditionalExpr:
        {
          auto* c = static_cast<ConditionalExpr*>(e);
          const Ty* cond = walkExpr(c->cond);
          const Ty* then = walkExpr(c->then);
          const Ty* other = walkExpr(c->else_);
          if (!c->then)
            return record(e, cond); // Elvis
          const Ty* a = types.stripRef(then);
          const Ty* b = types.stripRef(other);
          if (!a || !b || !a->isKnown())
            return record(e, b && b->isKnown() ? other : types.unknown());
          if (!b->isKnown())
            return record(e, then);
          if (a->kind == TyKind::Nullptr)
            return record(e, other);
          if (b->kind == TyKind::Nullptr)
            return record(e, then);
          if (a->isArithmetic() && b->isArithmetic() && a != b)
            return record(e, arithmetic(a, b));
          return record(e, then);
        }
        case NodeKind::CastExpr:
        {
          auto* c = static_cast<CastExpr*>(e);
          walkType(c->type);
          walkExpr(c->operand);
          return record(e, typeOf(c->type));
        }
        case NodeKind::NamedCastExpr:
        {
          auto* c = static_cast<NamedCastExpr*>(e);
          walkType(c->type);
          walkExpr(c->operand);
          return record(e, typeOf(c->type));
        }
        case NodeKind::SizeofExpr:
          walkType(static_cast<SizeofExpr*>(e)->type);
          walkExpr(static_cast<SizeofExpr*>(e)->operand);
          return record(e, types.sizeType());
        case NodeKind::NewExpr:
        {
          auto* n = static_cast<NewExpr*>(e);
          walkExprs(n->placement);
          walkType(n->type);
          walkExprs(n->args);
          const Ty* t = typeOf(n->type);
          if (t->kind == TyKind::Array)
            t = t->inner;
          return record(e, t->isKnown() ? types.pointer(t) : t);
        }
        case NodeKind::DeleteExpr:
          walkExpr(static_cast<DeleteExpr*>(e)->operand);
          return record(e, types.voidType());
        case NodeKind::LambdaExpr:
        {
          auto* l = static_cast<LambdaExpr*>(e);
          size_t mark = locals.mark();
          for (const LambdaCapture& cap : l->captures)
            if (cap.decl)
              walkLocalDecl(cap.decl); // init-capture: a variable of the closure, typed from its initializer
          pushTemplateParams(l->templateParams);
          std::vector<const Ty*> params;
          for (Decl* p : l->params)
          {
            walkParam(static_cast<ParamDecl*>(p));
            params.push_back(impl.declaredTypeOf(p, activeSubst, 0));
          }
          walkType(l->returnType);
          const Ty* result = nullptr;
          if (l->returnType)
            result = typeOf(l->returnType);
          returnFrames.push_back(nullptr);
          walkStmt(l->body);
          const Ty* deduced = returnFrames.back();
          returnFrames.pop_back();
          if (!result)
            result = deduced ? types.decay(deduced) : types.voidType();
          locals.pop(mark);
          // In a template, the lambda is a different type in each instantiation (its body differs).
          int64_t instance = 0;
          if (activeSubst)
          {
            uint64_t h = 1469598103934665603ull;
            for (auto& [d, ty] : activeSubst->pairs)
            {
              h = (h ^ uint64_t(reinterpret_cast<uintptr_t>(d))) * 1099511628211ull;
              h = (h ^ uint64_t(reinterpret_cast<uintptr_t>(ty))) * 1099511628211ull;
            }
            instance = int64_t(h ? h : 1);
          }
          return record(e, types.lambda(result, Span<const Ty*>{params.data(), uint32_t(params.size())}, l->closure, instance));
        }
        case NodeKind::BraceInitExpr:
          walkExprs(static_cast<BraceInitExpr*>(e)->args);
          return record(e, unknown(Why::Unsupported));
        case NodeKind::ThrowExpr:
          walkExpr(static_cast<ThrowExpr*>(e)->operand);
          return record(e, types.voidType());
        case NodeKind::FunctionalCastExpr:
        {
          auto* c = static_cast<FunctionalCastExpr*>(e);
          walkType(c->type);
          walkExprs(c->args);
          const Ty* t = typeOf(c->type);
          if (t->kind == TyKind::Auto && c->args.size() == 1 && c->args[0]->ty)
            t = types.decay(c->args[0]->ty); // auto(x)
          return record(e, t);
        }
        case NodeKind::TypeidExpr:
        {
          auto* t = static_cast<TypeidExpr*>(e);
          walkNode(t->operand);
          if (t->operand && uint8_t(t->operand->kind) <= uint8_t(NodeKind::PackType))
          {
            // typeid(Widget), or typeid(q) with q a variable the parser read as a type name.
            auto* n = t->operand->kind == NodeKind::NamedType ? static_cast<NamedType*>(t->operand) : nullptr;
            Lookup l = n ? impl.lookupQualified(n->name, scope, &locals, 0, activeSubst) : Lookup{};
            if (l.decl && (l.decl->kind == NodeKind::VariableDecl || l.decl->kind == NodeKind::ParamDecl) && !(l.decl->flags & DF_Typedef))
            {
              t->operandDecl = l.decl;
              t->operandType = impl.declaredTypeOf(l.decl, activeSubst, 0);
            }
            else
              t->operandType = typeOf(static_cast<Type*>(t->operand));
          }
          if (Decl* info = impl.typeInfoDecl())
            return record(e, types.classType(info, {})); // the library's TypeInfo; the code generator fills in the name
          return record(e, unknown(Why::Unsupported));
        }
        case NodeKind::FoldExpr:
        {
          auto* f = static_cast<FoldExpr*>(e);
          const Ty* l = walkExpr(f->lhs);
          const Ty* r = walkExpr(f->rhs);
          if (f->op == uint32_t(P::Comma))
            return record(e, types.voidType());
          const Ty* pattern = l ? l : r;
          if (pattern && pattern->kind == TyKind::Pack)
            pattern = pattern->args.empty() ? types.voidType() : pattern->args[0];
          return record(e, pattern ? pattern : types.unknown());
        }
        case NodeKind::RequiresExpr:
          return record(e, types.boolType());
        case NodeKind::ParenExpr:
          return record(e, walkExpr(static_cast<ParenExpr*>(e)->inner));
        default:
          return record(e, unknown(Why::Unsupported));
      }
    }
  };
};

GlobalScope::GlobalScope(SymbolTable& symbols) : impl(new Impl(symbols)) {}
GlobalScope::~GlobalScope() { delete impl; }

void GlobalScope::indexFile(ParsedFile& file)
{
  impl->indexDecls(file.decls, &impl->root);
}

void GlobalScope::finishIndex()
{
  // `struct A::B { ... }`: resolve A, then index B inside it. Repeat for nested chains.
  for (int round = 0; round < 4 && !impl->deferredQualified.empty(); ++round)
  {
    std::vector<std::pair<Decl*, Scope*>> pending;
    pending.swap(impl->deferredQualified);
    for (auto& [d, context] : pending)
    {
      const QualifiedName& name = d->kind == NodeKind::ClassDecl ? static_cast<ClassDecl*>(d)->name
                                                                 : static_cast<EnumDecl*>(d)->name;
      QualifiedName qualifier = name;
      qualifier.parts.count -= 1;
      Lookup l = impl->lookupQualified(qualifier, context, nullptr, 0);
      if (!l.scope)
      {
        impl->deferredQualified.push_back({d, context});
        continue;
      }
      // Re-index as a simple name inside the found scope.
      QualifiedName simple;
      simple.parts.data = name.parts.data + (name.parts.size() - 1);
      simple.parts.count = 1;
      if (d->kind == NodeKind::ClassDecl)
        static_cast<ClassDecl*>(d)->name = simple;
      else
        static_cast<EnumDecl*>(d)->name = simple;
      impl->indexDecl(d, l.scope);
    }
  }
  // Attach out-of-line member function definitions to their class scope.
  for (auto& [f, context] : impl->deferredFunctions)
  {
    QualifiedName qualifier = f->name;
    qualifier.parts.count -= 1;
    LocalStack params;
    for (Decl* p : f->templateParams)
      params.push(static_cast<TemplateParamDecl*>(p)->name, p);
    Lookup l = impl->lookupQualified(qualifier, context, &params, 0);
    if (l.scope && l.scope->kind == ScopeKind::Class)
      l.scope->outOfLineMembers.push_back(f);
    else if (l.scope && l.scope->kind == ScopeKind::Namespace)
    {
      // `void StringUtil::f() {}` outside its namespace: the same as written inside it (its name
      // is found there, its body sees the namespace), as C++ has it.
      QualifiedName simple;
      simple.parts.data = f->name.parts.data + (f->name.parts.size() - 1);
      simple.parts.count = 1;
      f->name = simple;
      if (l.scope->owner)
        f->parent = l.scope->owner;
      impl->indexDecl(f, l.scope);
    }
  }
  impl->deferredFunctions.clear();
  // Attach out-of-line static data member definitions to the declarations in their classes.
  for (auto& [v, context] : impl->deferredVariables)
  {
    QualifiedName qualifier = v->name;
    qualifier.parts.count -= 1;
    LocalStack params;
    for (Decl* p : v->templateParams)
      params.push(static_cast<TemplateParamDecl*>(p)->name, p);
    Lookup l = impl->lookupQualified(qualifier, context, &params, 0);
    if (l.scope && l.scope->kind == ScopeKind::Class)
    {
      if (const std::vector<Entry>* entries = l.scope->find(v->name.last().symbol))
        for (const Entry& e : *entries)
          if (e.decl && e.decl->kind == NodeKind::VariableDecl)
          {
            impl->variableDefinitions[e.decl] = v;
            break;
          }
    }
    else if (l.scope && l.scope->kind == ScopeKind::Namespace)
    {
      // `int Config::limit = 5;` outside its namespace: the same as written inside it.
      QualifiedName simple;
      simple.parts.data = v->name.parts.data + (v->name.parts.size() - 1);
      simple.parts.count = 1;
      v->name = simple;
      if (l.scope->owner)
        v->parent = l.scope->owner;
      impl->indexDecl(v, l.scope);
    }
  }
  impl->deferredVariables.clear();
}

void GlobalScope::resolveFile(ParsedFile& file, const std::vector<Token>& tokens, std::string_view text, ResolveStats& stats)
{
  Impl::Walker walker(*impl, stats, tokens, text);
  walker.walkDecls(file.decls);
}

TypeTable& GlobalScope::types() { return impl->types; }


FunctionDecl* GlobalScope::findFreeOperator(uint32_t punctuator, const Ty* lhs, const Ty* rhs, Decl* context)
{
  uint32_t symbol = impl->opSymbols[punctuator];
  std::vector<Scope*> places;
  for (const Ty* t : {impl->types.stripRef(lhs), rhs ? impl->types.stripRef(rhs) : nullptr})
    if (t && t->kind == TyKind::Class && t->decl)
      places.push_back(impl->lexicalScopeOf(t->decl));
  places.push_back(impl->lexicalScopeOf(context));
  std::vector<const Ty*> argTypes{lhs};
  if (rhs)
    argTypes.push_back(rhs);
  for (Scope* place : places)
    for (Scope* s = place; s; s = s->parent)
    {
      Lookup look;
      if (!impl->lookupInScope(s, symbol, look, 0))
        continue;
      if (look.decl && look.decl->kind == NodeKind::FunctionDecl)
      {
        std::vector<FunctionDecl*> candidates;
        if (look.overloads)
        {
          for (const Entry& e : *look.overloads)
            if (e.decl && e.decl->kind == NodeKind::FunctionDecl)
              candidates.push_back(static_cast<FunctionDecl*>(e.decl));
        }
        else
          candidates.push_back(static_cast<FunctionDecl*>(look.decl));
        bool ambiguous = false;
        if (FunctionDecl* f = impl->chooseAmong(candidates, argTypes, nullptr, nullptr, ambiguous))
          return f;
      }
      break;
    }
  return nullptr;
}

const Ty* GlobalScope::typeOfDecl(Decl* d) { return impl->declaredTypeOf(d, nullptr, 0); }

const Ty* GlobalScope::typeOfAst(Type* t, Decl* context)
{
  Impl::DeclContext dc;
  impl->declContextFor(context, dc);
  ParsedFile* file = context ? context->file : nullptr;
  return impl->convertType(t, Impl::TypeContext{dc.scope, &dc.params, nullptr, file ? file->tokens : nullptr, file ? file->text : std::string_view()}, 0);
}

FunctionDecl* GlobalScope::declarationOf(FunctionDecl* def)
{
  if (!def || def->name.parts.size() <= 1)
    return nullptr;
  Decl* cls = owningClass(def);
  if (!cls || !cls->ownScope)
    return nullptr;
  uint32_t symbol = impl->functionSymbol(def->name);
  if (const std::vector<Entry>* entries = cls->ownScope->find(symbol))
    for (const Entry& e : *entries)
    {
      if (!e.decl || e.decl->kind != NodeKind::FunctionDecl || e.decl == def)
        continue;
      auto* decl = static_cast<FunctionDecl*>(e.decl);
      if (decl->body)
        continue;
      if (impl->typeAs(def, decl) == impl->declaredTypeOf(decl, nullptr, 0))
      {
        def->flags |= decl->flags & (DF_Static | DF_Virtual);
        return decl;
      }
    }
  return nullptr;
}

Decl* GlobalScope::owningClass(Decl* d)
{
  if (!d)
    return nullptr;
  if (d->parent && (d->parent->kind == NodeKind::ClassDecl || d->parent->kind == NodeKind::EnumDecl))
    return d->parent;
  if (d->kind == NodeKind::FunctionDecl)
  {
    auto* f = static_cast<FunctionDecl*>(d);
    if (f->name.parts.size() > 1)
    {
      QualifiedName qualifier = f->name;
      qualifier.parts.count -= 1;
      LocalStack params;
      for (Decl* p : f->templateParams)
        params.push(static_cast<TemplateParamDecl*>(p)->name, p);
      Lookup l = impl->lookupQualified(qualifier, impl->lexicalScopeOf(f), &params, 0);
      if (l.scope && l.scope->kind == ScopeKind::Class)
        return l.scope->owner;
    }
  }
  return nullptr;
}

Decl* GlobalScope::initializerListDecl() { return impl->initializerListDecl(); }
Decl* GlobalScope::typeInfoDecl() { return impl->typeInfoDecl(); }

Decl* GlobalScope::definitionOf(Decl* d)
{
  if (!d)
    return nullptr;
  if (d->kind == NodeKind::VariableDecl)
  {
    // A static data member: the out-of-line definition carries the initializer.
    auto it = impl->variableDefinitions.find(d);
    return it != impl->variableDefinitions.end() ? it->second : d;
  }
  if (d->kind == NodeKind::ClassDecl || d->kind == NodeKind::EnumDecl)
  {
    if (d->ownScope)
      return d;
    Entry e{d, nullptr};
    Scope* s = impl->scopeOfEntry(e, 0);
    return s ? s->owner : nullptr;
  }
  if (d->kind == NodeKind::FunctionDecl)
  {
    auto* f = static_cast<FunctionDecl*>(d);
    if (f->body)
      return f;
    Decl* cls = owningClass(f);
    if (cls && cls->ownScope)
    {
      uint32_t symbol = impl->functionSymbol(f->name);
      const Ty* ft = impl->declaredTypeOf(f, nullptr, 0);
      for (FunctionDecl* candidate : cls->ownScope->outOfLineMembers)
        if (impl->functionSymbol(candidate->name) == symbol && impl->typeAs(candidate, f) == ft)
        {
          // `static` and `virtual` are written on the declaration only; the definition is emitted
          // (with or without `this`) by what the declaration says.
          candidate->flags |= f->flags & (DF_Static | DF_Virtual);
          return candidate;
        }
      static bool debugCalls = getenv("KOV_DEBUG_CALLS") != nullptr;
      if (debugCalls)
      {
        std::string line = "definitionOf " + qualifiedName(f) + " : " + impl->types.toString(ft, &impl->symbols) + " candidates:";
        for (FunctionDecl* candidate : cls->ownScope->outOfLineMembers)
          if (impl->functionSymbol(candidate->name) == symbol)
            line += " " + impl->types.toString(impl->typeAs(candidate, f), &impl->symbols);
        line += " (" + std::to_string(cls->ownScope->outOfLineMembers.size()) + " out-of-line members)";
        fprintf(stderr, "%s\n", line.c_str());
      }
    }
    else if (!cls && f->name.parts.size() == 1)
    {
      // A free function declared and defined separately: find the entry with a body.
      Scope* s = impl->lexicalScopeOf(f);
      uint32_t symbol = impl->functionSymbol(f->name);
      const Ty* ft = impl->declaredTypeOf(f, nullptr, 0);
      if (const std::vector<Entry>* entries = s->find(symbol))
        for (const Entry& e : *entries)
          if (e.decl->kind == NodeKind::FunctionDecl && static_cast<FunctionDecl*>(e.decl)->body &&
              impl->typeAs(static_cast<FunctionDecl*>(e.decl), f) == ft)
            return e.decl;
    }
    return f;
  }
  return d;
}

std::string GlobalScope::qualifiedName(Decl* d)
{
  auto own = [&](Decl* x) -> std::string
  {
    switch (x->kind)
    {
      case NodeKind::NamespaceDecl:
      {
        uint32_t n = static_cast<NamespaceDecl*>(x)->name;
        return n == UINT32_MAX ? std::string() : std::string(impl->symbols.name(n));
      }
      case NodeKind::ClassDecl: case NodeKind::EnumDecl:
      {
        const QualifiedName& q = x->kind == NodeKind::ClassDecl ? static_cast<ClassDecl*>(x)->name : static_cast<EnumDecl*>(x)->name;
        return q.empty() || q.last().symbol == UINT32_MAX ? std::string("(anonymous)") : std::string(impl->symbols.name(q.last().symbol));
      }
      case NodeKind::FunctionDecl:
      {
        const NamePart& p = static_cast<FunctionDecl*>(x)->name.last();
        if (p.special == 1)
          return "~" + std::string(impl->symbols.name(p.symbol));
        if (p.special == 2 && p.opToken < uint32_t(P::Count))
        {
          P op = P(p.opToken);
          return op == P::LBracket ? "operator[]" : op == P::LParen ? "operator()" : "operator" + std::string(punctuatorSpelling(op));
        }
        if (p.special != 0 || p.symbol == UINT32_MAX)
          return "operator?";
        return std::string(impl->symbols.name(p.symbol));
      }
      case NodeKind::VariableDecl:
      {
        const QualifiedName& q = static_cast<VariableDecl*>(x)->name;
        return q.empty() ? std::string("(unnamed)") : std::string(impl->symbols.name(q.last().symbol));
      }
      case NodeKind::EnumeratorDecl: return std::string(impl->symbols.name(static_cast<EnumeratorDecl*>(x)->name));
      default: return "?";
    }
  };
  std::vector<std::string> parts;
  Decl* start = owningClass(d);
  if (!start || (d->parent == start))
    start = d->parent;
  for (Decl* p = start; p; p = p->parent)
  {
    if (p->kind == NodeKind::NamespaceDecl || p->kind == NodeKind::ClassDecl || p->kind == NodeKind::EnumDecl)
    {
      std::string n = own(p);
      if (!n.empty())
        parts.push_back(n);
    }
  }
  std::string result;
  for (size_t i = parts.size(); i-- > 0;)
    result += parts[i] + "::";
  return result + own(d);
}

bool GlobalScope::isExternC(FunctionDecl* f) { return (f->flags & DF_ExternC) != 0; }

bool GlobalScope::isTemplateContext(Decl* d)
{
  for (Decl* p = d; p; p = p->parent)
    if (!p->templateParams.empty())
      return true;
  return false;
}

namespace
{
  Subst toSubst(const TemplateArgs& args)
  {
    Subst s;
    for (auto& [d, t] : args.pairs)
      s.pairs.push_back({d, t});
    return s;
  }
}

const Ty* GlobalScope::typeOfDeclIn(Decl* d, const TemplateArgs& args)
{
  if (args.empty())
    return typeOfDecl(d);
  Subst s = toSubst(args);
  const Ty* t = impl->declaredTypeOf(d, &s, 0);
  if (impl->containsAuto(t) && d->kind == NodeKind::VariableDecl)
  {
    auto* v = static_cast<VariableDecl*>(d);
    if (v->init && v->init->ty)
      return impl->deduceAuto(t, v->init->ty);
  }
  return t;
}

const Ty* GlobalScope::typeOfAstIn(Type* t, Decl* context, const TemplateArgs& args)
{
  Impl::DeclContext dc;
  impl->declContextFor(context, dc);
  Subst s = toSubst(args);
  ParsedFile* file = context ? context->file : nullptr;
  return impl->convertType(t, Impl::TypeContext{dc.scope, &dc.params, args.empty() ? nullptr : &s, file ? file->tokens : nullptr,
                                                file ? file->text : std::string_view()}, 0);
}

std::optional<bool> GlobalScope::typeTraitValue(NameExpr* e, Decl* context, const TemplateArgs& args)
{
  if (e->name.parts.size() != 1 || !e->name.parts[0].hasTemplateArgs || !impl->isBoolTraitName(e->name.parts[0].symbol))
    return std::nullopt;
  Impl::DeclContext dc;
  impl->declContextFor(context, dc);
  Subst s = toSubst(args);
  ParsedFile* file = context ? context->file : nullptr;
  Impl::TypeContext ctx{dc.scope, &dc.params, args.empty() ? nullptr : &s, file ? file->tokens : nullptr, file ? file->text : std::string_view()};
  std::vector<const Ty*> a = impl->traitArgs(e->name.parts[0], ctx, 0);
  if (a.size() != e->name.parts[0].templateArgs.size())
    return std::nullopt;
  return impl->traitValue(impl->symbols.name(e->name.parts[0].symbol), a);
}

void GlobalScope::deduceArgs(FunctionDecl* f, const std::vector<const Ty*>& argTypes, TemplateArgs& args)
{
  Subst s = toSubst(args);
  const Ty* ft = impl->declaredTypeOf(f, s.empty() ? nullptr : &s, 0);
  impl->deduceCall(f, ft, argTypes, s);
  for (Decl* p : f->templateParams)
    if (const Ty* t = s.find(p))
    {
      bool known = false;
      for (auto& [d, ty] : args.pairs)
        known = known || d == p;
      if (!known)
        args.pairs.push_back({p, t});
    }
}

FunctionDecl* GlobalScope::chooseOverload(const std::vector<FunctionDecl*>& candidates, const std::vector<const Ty*>& argTypes,
                                          const TemplateArgs& classArgs, const Ty* objectType, bool* ambiguous)
{
  Subst s = toSubst(classArgs);
  bool amb = false;
  FunctionDecl* f = impl->chooseAmong(candidates, argTypes, classArgs.empty() ? nullptr : &s, objectType, amb);
  if (ambiguous)
    *ambiguous = amb;
  return f;
}

TemplateArgs GlobalScope::classArgsOf(const Ty* classType)
{
  TemplateArgs out;
  Subst s = impl->substFor(classType);
  for (const Subst::Pair& p : s.pairs)
    out.pairs.push_back({p.param, p.type});
  return out;
}

void GlobalScope::retypeFor(FunctionDecl* f, const TemplateArgs& args, const Ty* objectType)
{
  if (!f->file || !f->file->tokens)
    return;
  Subst subst = toSubst(args);
  ResolveStats scratch;
  Impl::Walker w(*impl, scratch, *f->file->tokens, f->file->text);
  for (Decl* p = f; p; p = p->parent)
    for (Decl* tp : p->templateParams)
      w.locals.push(static_cast<TemplateParamDecl*>(tp)->name, tp);
  // The qualifier of an out-of-line definition (`ScrollBar<Policy>::ScrollBar`) is looked up
  // from the definition's own namespace, where the class is visible.
  w.scope = impl->lexicalScopeOf(f);
  if (f->name.parts.size() > 1)
    w.scope = w.semanticScopeFor(f->name);
  w.activeSubst = &subst;
  static bool debugCalls = getenv("KOV_DEBUG_CALLS") != nullptr;
  if (debugCalls)
  {
    std::string line = "retype " + std::string(impl->symbols.name(f->name.last().symbol)) + ":";
    for (auto& [d, ty] : subst.pairs)
      line += " " + std::string(impl->symbols.name(static_cast<TemplateParamDecl*>(d)->name)) + "=" + impl->types.toString(ty, &impl->symbols);
    fprintf(stderr, "%s\n", line.c_str());
  }
  if (objectType)
    w.thisType = impl->types.pointer(impl->types.unqualified(impl->types.stripRef(objectType)));
  else if (Decl* cls = Impl::enclosingClassDecl(w.scope))
    w.thisType = impl->types.pointer(impl->substitute(impl->selfType(cls), subst));
  w.walkDecl(f);
}

size_t GlobalScope::scopeCount() const { return impl->scopes.load(); }
size_t GlobalScope::entryCount() const { return impl->entries.load(); }
size_t GlobalScope::typeCount() const { return impl->types.size(); }

GlobalScope::InstantiationStats GlobalScope::instantiationStats() const
{
  return {impl->instClasses.load(), impl->instFunctions.load(), impl->instBodies.load(), impl->instRequests.load(),
          impl->instWaits.load(), impl->instNanos.load() / 1e9};
}
