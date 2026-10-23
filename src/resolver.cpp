#include "resolver.hpp"
#include <atomic>
#include <memory>
#include <mutex>
#include <string_view>
#include <thread>

void ResolveStats::add(const ResolveStats& o)
{
  typesResolved += o.typesResolved;
  typesDependent += o.typesDependent;
  typesUnresolved += o.typesUnresolved;
  namesLocal += o.namesLocal;
  namesMember += o.namesMember;
  namesGlobal += o.namesGlobal;
  namesDependent += o.namesDependent;
  namesUnresolved += o.namesUnresolved;
  membersDeferred += o.membersDeferred;
  qualifiedUnresolvedStd += o.qualifiedUnresolvedStd;
  unresolvedMacroLike += o.unresolvedMacroLike;
  for (const auto& [symbol, count] : o.unresolvedSymbols)
    unresolvedSymbols[symbol] += count;
  for (const auto& [symbol, where] : o.unresolvedFirst)
    unresolvedFirst.emplace(symbol, where);
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
    Scope* scope = nullptr; // Scope opened by decl, if any.
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
  Scope root{ScopeKind::Namespace, nullptr, nullptr, 256};
  std::mutex deferredMutex;
  std::vector<std::pair<Decl*, Scope*>> deferredQualified; // Out-of-line class/enum definitions.
  std::atomic<size_t> scopes{1};
  std::atomic<size_t> entries{0};
  uint32_t symStd = UINT32_MAX;
  const SymbolTable* symbols = nullptr;

  // ---------------- phase 1: indexing ----------------

  static uint32_t simpleName(const QualifiedName& name)
  {
    return name.parts.size() == 1 && name.parts[0].special == 0 ? name.parts[0].symbol : UINT32_MAX;
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
          uint32_t symbol = f->name.parts[0].symbol;
          if (f->name.parts[0].special == 0 && symbol != UINT32_MAX)
          {
            scope->add(symbol, f, nullptr);
            ++entries;
          }
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

  Lookup lookupQualified(const QualifiedName& name, Scope* context, const LocalStack* locals, int depth)
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
      if (part.special != 0 || part.symbol == UINT32_MAX)
      {
        // operator names and `decltype(...)` components: the class scope is the result.
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
        out = step;
        out.found = Found::Dependent;
        return out; // `T::type`: dependent on a template parameter.
      }
      if (last)
        return step;
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

  // ---------------- phase 2: walking ----------------

  struct Walker
  {
    Impl& impl;
    ResolveStats& stats;
    const SymbolTable& symbols;
    LocalStack locals;
    Scope* scope; // Current lexical/semantic scope.

    Walker(Impl& impl, ResolveStats& stats, const SymbolTable& symbols) : impl(impl), stats(stats), symbols(symbols), scope(&impl.root) {}

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

    void resolveTypeName(NamedType* t)
    {
      if (t->name.empty())
        return;
      Lookup l = impl.lookupQualified(t->name, scope, &locals, 0);
      t->resolved = l.decl;
      if (l.found == Found::No)
      {
        ++stats.typesUnresolved;
        countUnresolved(t->name, t->token);
      }
      else if (l.found == Found::Dependent)
        ++stats.typesDependent;
      else
        ++stats.typesResolved;
      for (const NamePart& part : t->name.parts)
        for (Node* arg : part.templateArgs)
          walkNode(arg);
    }

    void resolveName(NameExpr* e)
    {
      if (e->name.empty())
        return;
      Lookup l = impl.lookupQualified(e->name, scope, &locals, 0);
      e->resolved = l.decl;
      switch (l.found)
      {
        case Found::No:
          ++stats.namesUnresolved;
          countUnresolved(e->name, e->token);
          break;
        case Found::Local: ++stats.namesLocal; break;
        case Found::Member: ++stats.namesMember; break;
        case Found::Global: ++stats.namesGlobal; break;
        case Found::Dependent: ++stats.namesDependent; break;
      }
      for (const NamePart& part : e->name.parts)
        for (Node* arg : part.templateArgs)
          walkNode(arg);
    }

    void walkNode(Node* n)
    {
      if (!n)
        return;
      switch (n->kind)
      {
        case NodeKind::NamedType: case NodeKind::PointerType: case NodeKind::LRefType: case NodeKind::RRefType:
        case NodeKind::ConstType: case NodeKind::ArrayType: case NodeKind::FunctionType: case NodeKind::DecltypeType:
        case NodeKind::AutoType: case NodeKind::BuiltinType: case NodeKind::MemberPointerType: case NodeKind::PackType:
          walkType(static_cast<Type*>(n));
          break;
        default:
          if (uint8_t(n->kind) >= uint8_t(NodeKind::NameExpr))
            walkExpr(static_cast<Expr*>(n));
          else if (uint8_t(n->kind) >= uint8_t(NodeKind::BlockStmt))
            walkStmt(static_cast<Stmt*>(n));
          else
            walkDecl(static_cast<Decl*>(n));
      }
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
          Scope* semantic = c->ownScope;
          if (!semantic && c->name.parts.size() > 1)
            semantic = nullptr;
          // Base classes are resolved in the enclosing scope (plus template parameters).
          for (const ClassDecl::Base& b : c->bases)
            walkType(b.type);
          if (c->ownScope)
            scope = c->ownScope;
          else if (c->name.parts.size() > 1)
            scope = semanticScopeFor(c->name);
          walkDecls(c->members);
          scope = saved;
          break;
        }
        case NodeKind::EnumDecl:
        {
          auto* e = static_cast<EnumDecl*>(d);
          walkType(e->underlying);
          Scope* saved = scope;
          if (e->ownScope)
            scope = e->ownScope;
          for (Decl* en : e->enumerators)
            if (en->kind == NodeKind::EnumeratorDecl)
              walkExpr(static_cast<EnumeratorDecl*>(en)->value);
          walkDecls(e->members);
          scope = saved;
          break;
        }
        case NodeKind::FunctionDecl:
        {
          auto* f = static_cast<FunctionDecl*>(d);
          Scope* saved = scope;
          walkType(f->returnType);
          if (f->name.parts.size() > 1)
            scope = semanticScopeFor(f->name);
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
          walkStmt(f->body);
          scope = saved;
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
          if (i->cond && i->cond->kind == NodeKind::DeclStmt)
          {
          }
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
          walkExpr(f->range);
          walkLocalDecl(f->var);
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
        case NodeKind::ReturnStmt: walkExpr(static_cast<ReturnStmt*>(s)->value); break;
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

    void walkExpr(Expr* e)
    {
      if (!e)
        return;
      switch (e->kind)
      {
        case NodeKind::NameExpr: resolveName(static_cast<NameExpr*>(e)); break;
        case NodeKind::CallExpr:
          walkExpr(static_cast<CallExpr*>(e)->callee);
          walkExprs(static_cast<CallExpr*>(e)->args);
          break;
        case NodeKind::IndexExpr:
          walkExpr(static_cast<IndexExpr*>(e)->base);
          walkExprs(static_cast<IndexExpr*>(e)->args);
          break;
        case NodeKind::MemberExpr:
        {
          auto* m = static_cast<MemberExpr*>(e);
          walkExpr(m->base);
          if (m->base && m->base->kind == NodeKind::ThisExpr && !m->member.empty())
          {
            // `this->x`: resolvable in the class scope without type checking.
            Lookup l = impl.lookupQualified(m->member, scope, nullptr, 0);
            if (l.found == Found::No)
            {
              ++stats.namesUnresolved;
              countUnresolved(m->member, m->token);
            }
            else if (l.found == Found::Dependent)
              ++stats.namesDependent;
            else
              ++stats.namesMember;
          }
          else
            ++stats.membersDeferred;
          for (const NamePart& part : m->member.parts)
            for (Node* arg : part.templateArgs)
              walkNode(arg);
          break;
        }
        case NodeKind::UnaryExpr: walkExpr(static_cast<UnaryExpr*>(e)->operand); break;
        case NodeKind::BinaryExpr:
          walkExpr(static_cast<BinaryExpr*>(e)->lhs);
          walkExpr(static_cast<BinaryExpr*>(e)->rhs);
          break;
        case NodeKind::ConditionalExpr:
          walkExpr(static_cast<ConditionalExpr*>(e)->cond);
          walkExpr(static_cast<ConditionalExpr*>(e)->then);
          walkExpr(static_cast<ConditionalExpr*>(e)->else_);
          break;
        case NodeKind::CastExpr:
          walkType(static_cast<CastExpr*>(e)->type);
          walkExpr(static_cast<CastExpr*>(e)->operand);
          break;
        case NodeKind::NamedCastExpr:
          walkType(static_cast<NamedCastExpr*>(e)->type);
          walkExpr(static_cast<NamedCastExpr*>(e)->operand);
          break;
        case NodeKind::SizeofExpr:
          walkType(static_cast<SizeofExpr*>(e)->type);
          walkExpr(static_cast<SizeofExpr*>(e)->operand);
          break;
        case NodeKind::NewExpr:
        {
          auto* n = static_cast<NewExpr*>(e);
          walkExprs(n->placement);
          walkType(n->type);
          walkExprs(n->args);
          break;
        }
        case NodeKind::DeleteExpr: walkExpr(static_cast<DeleteExpr*>(e)->operand); break;
        case NodeKind::LambdaExpr:
        {
          auto* l = static_cast<LambdaExpr*>(e);
          size_t mark = locals.mark();
          pushTemplateParams(l->templateParams);
          for (Decl* p : l->params)
            walkParam(static_cast<ParamDecl*>(p));
          walkType(l->returnType);
          walkStmt(l->body);
          locals.pop(mark);
          break;
        }
        case NodeKind::BraceInitExpr: walkExprs(static_cast<BraceInitExpr*>(e)->args); break;
        case NodeKind::ThrowExpr: walkExpr(static_cast<ThrowExpr*>(e)->operand); break;
        case NodeKind::FunctionalCastExpr:
          walkType(static_cast<FunctionalCastExpr*>(e)->type);
          walkExprs(static_cast<FunctionalCastExpr*>(e)->args);
          break;
        case NodeKind::TypeidExpr: walkNode(static_cast<TypeidExpr*>(e)->operand); break;
        case NodeKind::FoldExpr:
          walkExpr(static_cast<FoldExpr*>(e)->lhs);
          walkExpr(static_cast<FoldExpr*>(e)->rhs);
          break;
        case NodeKind::ParenExpr: walkExpr(static_cast<ParenExpr*>(e)->inner); break;
        default: break;
      }
    }
  };
};

GlobalScope::GlobalScope() : impl(new Impl) {}
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
}

void GlobalScope::resolveFile(ParsedFile& file, const SymbolTable& symbols, ResolveStats& stats)
{
  if (impl->symStd == UINT32_MAX)
  {
    // Interning is thread-safe and idempotent.
    impl->symStd = const_cast<SymbolTable&>(symbols).intern("std");
  }
  Impl::Walker walker(*impl, stats, symbols);
  walker.walkDecls(file.decls);
}

size_t GlobalScope::scopeCount() const { return impl->scopes.load(); }
size_t GlobalScope::entryCount() const { return impl->entries.load(); }
