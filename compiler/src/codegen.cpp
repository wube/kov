#include "codegen.hpp"
#include <set>
#include <unordered_set>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <optional>
#include <unordered_map>

// Object model of the v0 code generator:
// - Scalars are SSA values. Class objects always live in memory; a Value of class type holds the
//   object's address.
// - A class parameter is passed as a pointer to a caller-made copy that the callee owns and
//   destroys. A class return value is constructed into a hidden `sret` slot provided by the
//   caller.
// - Locals (and temporaries) of class types that need destruction are destroyed at the end of
//   their scope, in reverse order; `return`, `break` and `continue` destroy the scopes they leave.
// - Templates: a struct layout exists per instantiation type; an instantiated function is
//   emitted on first use from a worklist, after the type checker re-typed its body for the
//   concrete arguments. All declaration types are read through declType(), which applies the
//   arguments of the instantiation being emitted (or of the class whose metadata is inspected).
namespace
{
  struct StructInfo
  {
    std::string name;                        // %struct.N
    std::vector<const Ty*> fieldTypes;       // In layout order: bases first, then data members.
    std::unordered_map<Decl*, uint32_t> fieldIndex;
    std::vector<std::pair<Decl*, uint32_t>> bases; // base class definition -> field index
    std::vector<Decl*> fieldDecls;           // data members in layout order
    std::optional<bool> needsDestruction;
    // Virtual functions: a polymorphic class has a vtable pointer at offset 0 (its own, or the
    // one of its first base). Slots are shared with the base and extended in declaration order.
    bool polymorphic = false;
    std::vector<std::string> vtableKeys;      // name(param types) [const], "~" for the destructor
    std::vector<FunctionDecl*> vtable;        // implementation per slot (null for pure virtual)
    std::vector<const Ty*> vtableOwners;      // class type each implementation belongs to
    std::string vtableName;                   // @"vtable.Class"
    std::string vtableText;                   // its definition, emitted into every module that stores it
    bool isUnion = false;                     // all members at offset 0, storage sized for the largest
    std::vector<std::pair<Decl*, uint32_t>> anonymous; // anonymous struct/union members -> field index
  };

  struct Emitter
  {
    GlobalScope& global;
    TypeTable& types;
    const SymbolTable& symbols;
    CodegenResult& result;

    std::string decls;   // struct types, globals, string constants, declares
    std::string body;    // function definitions (single module), or unused when split
    std::string shared;  // definitions every module carries (linkonce_odr): enum str(), the assert handler
    unsigned moduleCount = 1;
    unsigned currentModule = 0;                                  // where the function being emitted goes
    std::vector<std::string> bodies;                             // per module
    std::vector<std::string> moduleDecls;                        // per module: its string constants and vtables
    std::vector<std::unordered_set<std::string>> vtablesIn;      // per module: the vtables it already has
    bool emittingShared = false;                                 // string constants go to the shared decls
    std::unordered_map<ParsedFile*, unsigned> fileModule;        // file -> module
    std::unordered_map<std::string, unsigned> definedIn;         // function name -> module
    std::unordered_map<std::string, std::string> declareLines;   // function name -> its declare line
    std::string fn;      // current function text (after the allocas)
    std::string allocas; // entry-block allocas of the current function
    uint32_t temp = 0;
    uint32_t label = 0;
    uint32_t stringCount = 0;
    bool blockTerminated = false;
    bool debug = true;        // config::debug: asserts are compiled in
    bool usesAssert = false;  // some assert was emitted: the module needs kov_assert_failed
    std::string currentBlock = "entry";
    FunctionDecl* currentFunction = nullptr;
    const Ty* currentReturn = nullptr;
    std::string sretAddr; // Hidden return slot when returning a class.
    ParsedFile* currentFile = nullptr;

    // Template arguments in effect: of the instantiation being emitted, or temporarily of a class
    // whose constructors/fields are being inspected.
    const TemplateArgs* currentArgs = nullptr;
    const Ty* currentClassTy = nullptr; // Instantiated class of the method being emitted.
    struct ArgsScope
    {
      Emitter& e;
      const TemplateArgs* saved;
      ArgsScope(Emitter& e, const TemplateArgs* args) : e(e), saved(e.currentArgs) { e.currentArgs = args; }
      ~ArgsScope() { e.currentArgs = saved; }
    };

    struct Local
    {
      std::string addr; // Address of the variable (or of the pointer slot for references).
      const Ty* ty;
      bool isRef;
    };
    struct Value
    {
      std::string text;         // IR operand; for class types the object's address
      const Ty* ty;             // Value type (never a reference)
      bool isTemporary = false; // Class temporary owned by the current full-expression
    };
    std::unordered_map<Decl*, Local> locals;
    std::unordered_map<Decl*, std::vector<Local>> packLocals; // one local per element of a parameter pack
    std::unordered_map<const Ty*, StructInfo> structs;
    std::unordered_map<Decl*, std::string> globals;
    std::unordered_map<std::string, bool> declaredNames; // IR names with a declare/define line
    std::unordered_map<Decl*, int64_t> enumeratorValues;

    struct Instantiation
    {
      FunctionDecl* f;
      TemplateArgs args;
      const Ty* classTy;
      unsigned module; // the module of the function that asked for it
    };

    // A lambda's closure: one struct per lambda, fields for the captures, one function.
    struct ClosureInfo
    {
      std::string structName;
      std::vector<Decl*> captured;       // enclosing local per field (null for `this`)
      std::vector<bool> byRef;
      std::vector<const Ty*> fieldTypes; // pointee type for references and `this`
      const Ty* enclosingClass = nullptr;
      std::string functionName;
      bool emitted = false;
    };
    std::unordered_map<Decl*, ClosureInfo> closures; // keyed by the closure ClassDecl
    static bool isLambda(const Ty* t) { return t && t->kind == TyKind::Lambda && t->decl; }
    static bool isAggregate(const Ty* t) { return isClass(t) || isLambda(t); }
    std::deque<Instantiation> worklist;
    std::unordered_map<std::string, bool> requested; // instantiated names queued or emitted

    struct Object
    {
      std::string addr;
      const Ty* ty;
    };
    std::vector<std::vector<Object>> scopes; // Objects to destroy, per open scope.
    std::vector<Object> statementTemps;       // Temporaries of the current full-expression.
    struct Loop
    {
      std::string breakLabel, continueLabel;
      size_t scopeDepth;
    };
    std::vector<Loop> loops;
    bool mallocDeclared = false;

    Emitter(GlobalScope& global, const SymbolTable& symbols, CodegenResult& result)
      : global(global), types(global.types()), symbols(symbols), result(result)
    {
    }

    // --- diagnostics ---

    void error(Node* at, const std::string& message)
    {
      std::string where = "?";
      if (currentFile && currentFile->tokens && at)
      {
        const std::vector<Token>& toks = *currentFile->tokens;
        const Token& t = toks[std::min<size_t>(at->token, toks.size() - 1)];
        uint32_t line = 1;
        for (uint32_t i = 0; i < t.offset && i < currentFile->text.size(); ++i)
          if (currentFile->text[i] == '\n')
            ++line;
        where = currentFile->path + ":" + std::to_string(line);
      }
      result.errors.push_back(where + ": " + message);
    }

    struct Unsupported
    {
    };
    [[noreturn]] void unsupported(Node* at, const std::string& what)
    {
      error(at, "codegen does not support " + what + " yet");
      throw Unsupported{};
    }

    // --- names and types ---

    std::string sym(uint32_t s) const { return s == UINT32_MAX ? "?" : std::string(symbols.name(s)); }
    std::string newTemp() { return "%t" + std::to_string(temp++); }
    std::string newLabel(const char* hint) { return std::string(hint) + std::to_string(label++); }

    static bool isClass(const Ty* t) { return t && t->kind == TyKind::Class; }

    const Ty* declType(Decl* d) { return currentArgs ? global.typeOfDeclIn(d, *currentArgs) : global.typeOfDecl(d); }

    const Ty* classKey(const Ty* t) { return types.unqualified(types.stripRef(t)); }

    std::string llType(const Ty* t, Node* at = nullptr)
    {
      if (!t)
        unsupported(at, "an expression without a type");
      switch (t->kind)
      {
        case TyKind::Void: return "void";
        case TyKind::Bool: return "i1";
        case TyKind::Char: case TyKind::Int: return "i" + std::to_string(t->bits);
        case TyKind::Float: return t->bits == 32 ? "float" : "double";
        case TyKind::Nullptr: case TyKind::Pointer: case TyKind::LRef: case TyKind::RRef: case TyKind::Function:
        case TyKind::MemberPointer:
          return "ptr";
        case TyKind::Lambda: return t->decl ? closureFor(t, at).structName : "ptr";
        case TyKind::Array:
          return "[" + std::to_string(t->count) + " x " + llType(t->inner, at) + "]";
        case TyKind::Class: return structFor(t, at).name;
        case TyKind::Enum: return llType(enumUnderlying(t->decl), at);
        default:
          unsupported(at, "the type " + types.toString(t, &symbols) + (at ? std::string(" of ") + nodeKindName(at->kind) : std::string()));
      }
    }

    const Ty* enumUnderlying(Decl* d)
    {
      Decl* def = global.definitionOf(d);
      auto* e = static_cast<EnumDecl*>(def ? def : d);
      if (e->underlying)
        if (const Ty* u = global.typeOfAst(e->underlying, e))
          if (u->isKnown())
            return u;
      return types.intType(32);
    }

    uint64_t sizeOf(const Ty* t, Node* at = nullptr)
    {
      switch (t->kind)
      {
        case TyKind::Bool: return 1;
        case TyKind::Char: case TyKind::Int: case TyKind::Float: return t->bits / 8;
        case TyKind::Nullptr: case TyKind::Pointer: case TyKind::LRef: case TyKind::RRef: case TyKind::Function:
        case TyKind::MemberPointer:
          return 8;
        case TyKind::Lambda:
        {
          if (!t->decl)
            return 8;
          uint64_t size = 0;
          for (const Ty* f : closureFor(t, at).fieldTypes)
            size += 8 > sizeOf(f, at) ? 8 : sizeOf(f, at); // generous: fields aligned to 8
          return std::max<uint64_t>(size, 1);
        }
        case TyKind::Array: return t->count * sizeOf(t->inner, at);
        case TyKind::Enum: return sizeOf(enumUnderlying(t->decl), at);
        case TyKind::Class:
        {
          StructInfo& info = structFor(t, at);
          uint64_t size = 0, align = 1;
          for (const Ty* f : info.fieldTypes)
          {
            uint64_t a = alignOf(f, at);
            size = (size + a - 1) / a * a + sizeOf(f, at);
            align = std::max(align, a);
          }
          size = (size + align - 1) / align * align;
          return std::max<uint64_t>(size, 1);
        }
        default:
          unsupported(at, "sizeof " + types.toString(t, &symbols));
      }
    }

    uint64_t alignOf(const Ty* t, Node* at = nullptr)
    {
      switch (t->kind)
      {
        case TyKind::Array: return alignOf(t->inner, at);
        case TyKind::Class:
        {
          uint64_t align = 1;
          for (const Ty* f : structFor(t, at).fieldTypes)
            align = std::max(align, alignOf(f, at));
          return align;
        }
        default: return std::max<uint64_t>(1, sizeOf(t, at));
      }
    }

    ClassDecl* classDef(Decl* decl, Node* at)
    {
      Decl* def = global.definitionOf(decl);
      if (!def || def->kind != NodeKind::ClassDecl)
        unsupported(at, "a class without a definition (" + global.qualifiedName(decl) + ")");
      return static_cast<ClassDecl*>(def);
    }

    // The instantiation type with the definition as its decl (forward declarations map here).
    const Ty* canonicalClass(const Ty* t, Node* at)
    {
      t = classKey(t);
      ClassDecl* def = classDef(t->decl, at);
      return def == t->decl ? t : types.classType(def, t->args);
    }

    StructInfo& structFor(const Ty* classTy, Node* at)
    {
      const Ty* key = canonicalClass(classTy, at);
      auto it = structs.find(key);
      if (it != structs.end())
        return it->second;
      ClassDecl* c = static_cast<ClassDecl*>(key->decl);
      TemplateArgs args = global.classArgsOf(key);
      if (c->keyword == Kw::union_)
      {
        // A union is one block of storage; every member lives at its start.
        StructInfo& info = structs[key];
        info.name = "%struct." + std::to_string(structs.size());
        info.isUnion = true;
        uint64_t size = 1, align = 1;
        for (Decl* m : c->members)
        {
          const Ty* ft = nullptr;
          if (m->kind == NodeKind::VariableDecl && !(m->flags & (DF_Static | DF_Typedef)))
            ft = global.typeOfDeclIn(m, args);
          else if (m->kind == NodeKind::ClassDecl && static_cast<ClassDecl*>(m)->name.empty() && static_cast<ClassDecl*>(m)->isDefinition)
            ft = types.classType(m, {});
          else
            continue;
          if (!ft || !ft->isKnown() || types.isDependent(ft))
            unsupported(m, "a union member of unknown type");
          info.fieldIndex[m] = 0;
          info.fieldDecls.push_back(m);
          if (m->kind == NodeKind::ClassDecl)
            info.anonymous.push_back({m, 0});
          size = std::max(size, sizeOf(ft, c));
          align = std::max(align, alignOf(ft, c));
        }
        info.fieldTypes.push_back(types.array(types.intType(uint8_t(align * 8)), uint32_t((size + align - 1) / align)));
        decls += info.name + " = type { " + llType(info.fieldTypes[0], c) + " }\n";
        ++result.structs;
        return structs[key];
      }
      if (!c->templateParams.empty() && args.pairs.size() != c->templateParams.size())
        unsupported(at, "the class template " + global.qualifiedName(c) + " used without all its arguments");
      StructInfo& info = structs[key];
      info.name = "%struct." + std::to_string(structs.size());
      // Bases first (resolved before deciding on the vtable pointer).
      std::vector<const Ty*> baseKeys;
      for (const ClassDecl::Base& b : c->bases)
      {
        const Ty* bt = global.typeOfAstIn(b.type, c, args);
        if (!bt || bt->kind != TyKind::Class || types.isDependent(bt))
          unsupported(c, "a base class that is not a known class");
        const Ty* baseKey = canonicalClass(bt, c);
        structFor(baseKey, c);
        baseKeys.push_back(baseKey);
      }
      // Virtual methods of this class: declared virtual/override, or matching an inherited slot.
      bool primaryPolymorphic = !baseKeys.empty() && structs[baseKeys[0]].polymorphic;
      for (size_t i = 1; i < baseKeys.size(); ++i)
        if (structs[baseKeys[i]].polymorphic)
          unsupported(c, "a polymorphic base class that is not the first base");
      std::vector<std::string> keys;
      std::vector<FunctionDecl*> impls;
      std::vector<const Ty*> owners;
      if (primaryPolymorphic)
      {
        keys = structs[baseKeys[0]].vtableKeys;
        impls = structs[baseKeys[0]].vtable;
        owners = structs[baseKeys[0]].vtableOwners;
      }
      {
        ArgsScope scope(*this, &args);
        for (Decl* m : c->members)
        {
          if (m->kind != NodeKind::FunctionDecl || (m->flags & (DF_Static | DF_Constructor)) || !m->templateParams.empty())
            continue;
          auto* f = static_cast<FunctionDecl*>(m);
          std::string k = methodKey(f);
          size_t slot = keys.size();
          for (size_t i = 0; i < keys.size(); ++i)
            if (keys[i] == k)
              slot = i;
          bool isVirtual = (f->flags & (DF_Virtual | DF_Override)) || slot < keys.size();
          if (!isVirtual)
            continue;
          if (slot == keys.size())
          {
            keys.push_back(k);
            impls.push_back(f);
            owners.push_back(key);
          }
          else
          {
            impls[slot] = f;
            owners[slot] = key;
          }
        }
      }
      if (!keys.empty())
      {
        info.polymorphic = true;
        if (!primaryPolymorphic)
          info.fieldTypes.push_back(types.pointer(types.voidType())); // own vtable pointer at offset 0
      }
      for (const Ty* baseKey : baseKeys)
      {
        info.bases.push_back({baseKey->decl, uint32_t(info.fieldTypes.size())});
        info.fieldTypes.push_back(baseKey);
      }
      info.vtableKeys = keys;
      info.vtable = impls;
      info.vtableOwners = owners;
      for (Decl* m : c->members)
      {
        if (m->kind == NodeKind::ClassDecl && static_cast<ClassDecl*>(m)->name.empty() && static_cast<ClassDecl*>(m)->isDefinition)
        {
          // Anonymous struct/union member: a field whose own members are reachable by name.
          const Ty* ft = types.classType(m, {});
          structFor(ft, c);
          info.anonymous.push_back({m, uint32_t(info.fieldTypes.size())});
          info.fieldIndex[m] = uint32_t(info.fieldTypes.size());
          info.fieldDecls.push_back(m);
          info.fieldTypes.push_back(ft);
          continue;
        }
        if (m->kind != NodeKind::VariableDecl || (m->flags & (DF_Static | DF_Typedef)))
          continue;
        const Ty* ft = global.typeOfDeclIn(m, args);
        if (!ft || !ft->isKnown() || types.isDependent(ft))
          unsupported(m, "a member of unknown type (" + sym(static_cast<VariableDecl*>(m)->name.last().symbol) + ")");
        info.fieldIndex[m] = uint32_t(info.fieldTypes.size());
        info.fieldDecls.push_back(m);
        info.fieldTypes.push_back(ft);
      }
      std::string line = info.name + " = type { ";
      for (size_t i = 0; i < info.fieldTypes.size(); ++i)
        line += (i ? ", " : "") + llType(info.fieldTypes[i], c);
      line += " }\n";
      decls += line;
      ++result.structs;
      if (structs[key].polymorphic)
      {
        std::string vt = "@\"vtable." + types.toString(key, &symbols) + "\"";
        structs[key].vtableName = vt;
        std::vector<FunctionDecl*> impls2 = structs[key].vtable;
        std::vector<const Ty*> owners2 = structs[key].vtableOwners;
        std::string entries;
        for (size_t i = 0; i < impls2.size(); ++i)
        {
          FunctionDecl* f = impls2[i];
          std::string entry = "ptr null";
          if (f && !(f->flags & DF_Pure))
            entry = "ptr " + useFunction(f, global.classArgsOf(owners2[i]), owners2[i]);
          entries += (i ? ", " : "") + entry;
        }
        structs[key].vtableText = "$" + vt.substr(1) + " = comdat any\n" + vt + " = linkonce_odr constant [" + std::to_string(impls2.size()) +
                                  " x ptr] [" + entries + "], comdat\n";
      }
      return structs[key];
    }

    // Identity of a method for override matching: name, parameter types and constness.
    std::string methodKey(FunctionDecl* f)
    {
      if (f->flags & DF_Destructor)
        return "~";
      std::string full = global.qualifiedName(f);
      size_t pos = full.rfind("::");
      std::string k = pos == std::string::npos ? full : full.substr(pos + 2);
      const Ty* ft = declType(f);
      k += "(";
      for (size_t i = 0; ft && ft->kind == TyKind::Function && i < ft->args.size(); ++i)
        k += (i ? ", " : "") + types.toString(ft->args[i], &symbols);
      k += ")";
      if (f->flags & DF_Const)
        k += " const";
      return k;
    }

    // Slot of f in the vtable of classTy, or -1 if f is not virtual there.
    int slotOf(const Ty* classTy, FunctionDecl* f, Node* at)
    {
      StructInfo& info = structFor(classTy, at);
      if (!info.polymorphic)
        return -1;
      TemplateArgs args = global.classArgsOf(canonicalClass(classTy, at));
      ArgsScope scope(*this, &args);
      std::string k = methodKey(f);
      for (size_t i = 0; i < info.vtableKeys.size(); ++i)
        if (info.vtableKeys[i] == k)
          return int(i);
      return -1;
    }

    // Loads the function pointer of a vtable slot from the object at addr.
    std::string loadSlot(const std::string& addr, int slot)
    {
      std::string vptr = newTemp();
      fn += "  " + vptr + " = load ptr, ptr " + addr + "\n";
      std::string entry = newTemp();
      fn += "  " + entry + " = getelementptr inbounds ptr, ptr " + vptr + ", i64 " + std::to_string(slot) + "\n";
      std::string fp = newTemp();
      fn += "  " + fp + " = load ptr, ptr " + entry + "\n";
      return fp;
    }

    std::string gep(const std::string& structName, const std::string& addr, uint32_t index)
    {
      std::string r = newTemp();
      fn += "  " + r + " = getelementptr inbounds " + structName + ", ptr " + addr + ", i32 0, i32 " + std::to_string(index) + "\n";
      return r;
    }

    bool fieldAddress(const Ty* classTy, Decl* member, const std::string& objectAddr, std::string& out, Node* at)
    {
      StructInfo& info = structFor(classTy, at);
      auto it = info.fieldIndex.find(member);
      if (it != info.fieldIndex.end())
      {
        out = info.isUnion ? objectAddr : gep(info.name, objectAddr, it->second);
        return true;
      }
      std::vector<std::pair<Decl*, uint32_t>> anonymous = info.anonymous;
      std::vector<std::pair<Decl*, uint32_t>> bases = info.bases;
      std::vector<const Ty*> fieldTypes = info.fieldTypes;
      bool isUnion = info.isUnion;
      std::string name = info.name;
      for (auto& [anon, index] : anonymous)
      {
        std::string addr = isUnion ? objectAddr : gep(name, objectAddr, index);
        if (fieldAddress(types.classType(anon, {}), member, addr, out, at))
          return true;
      }
      for (auto& [baseDef, index] : bases)
      {
        std::string baseAddr = gep(name, objectAddr, index);
        if (fieldAddress(fieldTypes[index], member, baseAddr, out, at))
          return true;
      }
      return false;
    }

    // --- class metadata: constructors, destructors ---

    FunctionDecl* findConstructor(const Ty* classTy, const std::vector<const Ty*>& argTypes, Node* at)
    {
      ClassDecl* c = classDef(classTy->decl, at);
      TemplateArgs args = global.classArgsOf(canonicalClass(classTy, at));
      std::vector<FunctionDecl*> candidates;
      for (Decl* m : c->members)
        if (m->kind == NodeKind::FunctionDecl && (m->flags & DF_Constructor) && m->templateParams.empty())
          candidates.push_back(static_cast<FunctionDecl*>(m));
      return global.chooseOverload(candidates, argTypes, args, nullptr);
    }

    static std::vector<const Ty*> argTypesOf(Span<Expr*> args)
    {
      std::vector<const Ty*> out;
      for (Expr* a : args)
        out.push_back(a ? a->ty : nullptr);
      return out;
    }

    FunctionDecl* findDestructor(ClassDecl* c)
    {
      for (Decl* m : c->members)
        if (m->kind == NodeKind::FunctionDecl && (m->flags & DF_Destructor))
          return static_cast<FunctionDecl*>(m);
      return nullptr;
    }

    bool hasUserConstructors(ClassDecl* c)
    {
      for (Decl* m : c->members)
        if (m->kind == NodeKind::FunctionDecl && (m->flags & DF_Constructor))
          return true;
      return false;
    }

    bool needsDestruction(const Ty* t, Node* at)
    {
      t = types.stripRef(t);
      if (t->kind == TyKind::Array)
        return needsDestruction(t->inner, at);
      if (!isClass(t))
        return false;
      StructInfo& info = structFor(t, at);
      if (info.needsDestruction)
        return *info.needsDestruction;
      info.needsDestruction = false; // Guard against recursion.
      ClassDecl* c = classDef(t->decl, at);
      bool result = findDestructor(c) != nullptr;
      for (const Ty* ft : std::vector<const Ty*>(info.fieldTypes))
        result = result || needsDestruction(ft, at);
      structFor(t, at).needsDestruction = result;
      return result;
    }

    // Destroys the object at addr: user destructor if any (it destroys members itself), else
    // members and bases that need it, in reverse order.
    void destroy(const std::string& addr, const Ty* t, Node* at, bool dynamic = false)
    {
      t = types.stripRef(t);
      if (t->kind == TyKind::Array)
      {
        if (!needsDestruction(t->inner, at))
          return;
        for (uint32_t i = t->count; i-- > 0;)
        {
          std::string e = newTemp();
          fn += "  " + e + " = getelementptr inbounds " + llType(t, at) + ", ptr " + addr + ", i64 0, i64 " + std::to_string(i) + "\n";
          destroy(e, t->inner, at);
        }
        return;
      }
      if (!isClass(t) || !needsDestruction(t, at))
        return;
      ClassDecl* c = classDef(t->decl, at);
      if (dynamic)
      {
        StructInfo& info = structFor(t, at);
        for (size_t i = 0; i < info.vtableKeys.size(); ++i)
          if (info.vtableKeys[i] == "~")
          {
            std::string fp = loadSlot(addr, int(i));
            fn += "  call void " + fp + "(ptr " + addr + ")\n";
            return;
          }
      }
      if (FunctionDecl* dtor = findDestructor(c))
      {
        TemplateArgs args = global.classArgsOf(canonicalClass(t, at));
        fn += "  call void " + useFunction(dtor, args, canonicalClass(t, at)) + "(ptr " + addr + ")\n";
        return;
      }
      destroyMembers(addr, t, at);
    }

    void destroyMembers(const std::string& addr, const Ty* t, Node* at)
    {
      StructInfo& info = structFor(t, at);
      std::string name = info.name;
      std::vector<Decl*> fieldDecls = info.fieldDecls;
      std::vector<const Ty*> fieldTypes = info.fieldTypes;
      std::unordered_map<Decl*, uint32_t> fieldIndex = info.fieldIndex;
      std::vector<std::pair<Decl*, uint32_t>> bases = info.bases;
      for (size_t i = fieldDecls.size(); i-- > 0;)
      {
        const Ty* ft = fieldTypes[fieldIndex[fieldDecls[i]]];
        if (needsDestruction(ft, at))
          destroy(gep(name, addr, fieldIndex[fieldDecls[i]]), ft, at);
      }
      for (size_t i = bases.size(); i-- > 0;)
      {
        const Ty* bt = fieldTypes[bases[i].second];
        if (needsDestruction(bt, at))
          destroy(gep(name, addr, bases[i].second), bt, at);
      }
    }

    // Default construction: zero, then default member initializers, then class members' and
    // bases' constructors.
    void defaultConstruct(const std::string& addr, const Ty* t, Node* at)
    {
      ClassDecl* c = classDef(t->decl, at);
      if (hasUserConstructors(c))
      {
        FunctionDecl* ctor = findConstructor(t, {}, at);
        if (!ctor)
          unsupported(at, "default construction of " + global.qualifiedName(c) + ", which has no default constructor");
        TemplateArgs args = global.classArgsOf(canonicalClass(t, at));
        fn += "  call void " + useFunction(ctor, args, canonicalClass(t, at)) + "(ptr " + addr + ")\n";
        return;
      }
      fn += "  store " + llType(t, at) + " zeroinitializer, ptr " + addr + "\n";
      initializeMembers(addr, t, {}, at);
    }

    // Initialises bases and members of the object at addr from constructor initializers (or
    // default member initializers / default construction when absent).
    void initializeMembers(const std::string& addr, const Ty* t, Span<FunctionDecl::CtorInit> inits, Node* at)
    {
      StructInfo info = structFor(t, at); // copy: nested calls may add structs
      auto initFor = [&](uint32_t symbol) -> const FunctionDecl::CtorInit*
      {
        for (const FunctionDecl::CtorInit& init : inits)
          if (!init.member.empty() && init.member.last().symbol == symbol)
            return &init;
        return nullptr;
      };
      for (auto& [baseDef, index] : info.bases)
      {
        const Ty* bt = info.fieldTypes[index];
        std::string baseAddr = gep(info.name, addr, index);
        const QualifiedName& bn = static_cast<ClassDecl*>(baseDef)->name;
        const FunctionDecl::CtorInit* init = bn.empty() ? nullptr : initFor(bn.last().symbol);
        if (init)
          constructInto(baseAddr, bt, init->args, at);
        else if (hasUserConstructors(static_cast<ClassDecl*>(baseDef)))
          defaultConstruct(baseAddr, bt, at);
        else
          initializeMembers(baseAddr, bt, {}, at);
      }
      if (info.polymorphic)
      {
        useVtable(info);
        fn += "  store ptr " + info.vtableName + ", ptr " + addr + "\n";
      }
      if (info.isUnion)
        return;
      for (Decl* d : info.fieldDecls)
      {
        if (d->kind != NodeKind::VariableDecl)
          continue; // anonymous struct member: zero-initialised with the whole object
        auto* v = static_cast<VariableDecl*>(d);
        const Ty* ft = info.fieldTypes[info.fieldIndex[d]];
        std::string fieldAddr = gep(info.name, addr, info.fieldIndex[d]);
        const FunctionDecl::CtorInit* init = v->name.empty() ? nullptr : initFor(v->name.last().symbol);
        if (init)
        {
          if (isClass(ft))
            constructInto(fieldAddr, ft, init->args, at);
          else if (init->args.size() == 1)
            store(rvalue(init->args[0]), fieldAddr, ft, at);
          else if (init->args.empty())
            fn += "  store " + llType(ft, at) + " zeroinitializer, ptr " + fieldAddr + "\n";
          else
            unsupported(at, "this member initializer");
        }
        else if (v->init)
        {
          ParsedFile* saved = currentFile;
          currentFile = v->file;
          initializeFrom(fieldAddr, ft, v->init, v);
          currentFile = saved;
        }
        else if (isClass(ft) && hasUserConstructors(classDef(ft->decl, at)))
          defaultConstruct(fieldAddr, ft, at);
        else if (isClass(ft))
          initializeMembers(fieldAddr, ft, {}, at);
        else if (ft->kind == TyKind::Array && needsDestruction(ft, at))
          unsupported(at, "arrays of objects with constructors");
        else
          fn += "  store " + llType(ft, at) + " zeroinitializer, ptr " + fieldAddr + "\n";
      }
    }

    // Constructs an object of class type t at addr from constructor arguments.
    void constructInto(const std::string& addr, const Ty* t, Span<Expr*> args, Node* at)
    {
      ClassDecl* c = classDef(t->decl, at);
      if (args.empty())
      {
        defaultConstruct(addr, t, at);
        return;
      }
      if (args.size() == 1 && args[0]->kind == NodeKind::BraceInitExpr)
      {
        constructInto(addr, t, static_cast<BraceInitExpr*>(args[0])->args, at);
        return;
      }
      const Ty* firstTy = args.size() == 1 ? args[0]->ty : nullptr;
      FunctionDecl* ctor = findConstructor(t, argTypesOf(args), at);
      if (ctor)
      {
        callFunction(ctor, global.classArgsOf(canonicalClass(t, at)), canonicalClass(t, at), addr, "", args, at);
        return;
      }
      if (args.size() == 1 && firstTy && isClass(types.stripRef(firstTy)) && classKey(firstTy) == classKey(t))
      {
        Value src = rvalue(args[0]);
        if (src.isTemporary)
          moveTemporary(addr, src, t, at);
        else
          copyConstruct(addr, src.text, t, at);
        return;
      }
      if (!hasUserConstructors(c))
      {
        StructInfo info = structFor(t, at);
        fn += "  store " + llType(t, at) + " zeroinitializer, ptr " + addr + "\n";
        size_t i = 0;
        for (Decl* d : info.fieldDecls)
        {
          if (i >= args.size())
            break;
          std::string fieldAddr = gep(info.name, addr, info.fieldIndex[d]);
          initializeFrom(fieldAddr, info.fieldTypes[info.fieldIndex[d]], args[i++], at);
        }
        return;
      }
      unsupported(at, "a constructor of " + global.qualifiedName(c) + " taking " + std::to_string(args.size()) + " arguments");
    }

    // Copy-constructs the object at src into dst (user copy constructor or bitwise copy).
    void copyConstruct(const std::string& dst, const std::string& src, const Ty* t, Node* at)
    {
      ClassDecl* c = classDef(t->decl, at);
      TemplateArgs args = global.classArgsOf(canonicalClass(t, at));
      FunctionDecl* copy = nullptr;
      {
        ArgsScope scope(*this, &args);
        for (Decl* m : c->members)
        {
          if (m->kind != NodeKind::FunctionDecl || !(m->flags & DF_Constructor))
            continue;
          auto* f = static_cast<FunctionDecl*>(m);
          if (f->params.size() != 1)
            continue;
          const Ty* pt = declType(f->params[0]);
          if (pt && pt->isReference() && isClass(pt->inner) && classKey(pt->inner) == classKey(t))
            copy = f;
        }
      }
      if (copy)
      {
        fn += "  call void " + useFunction(copy, args, canonicalClass(t, at)) + "(ptr " + dst + ", ptr " + src + ")\n";
        return;
      }
      if (findDestructor(c))
        unsupported(at, "copying " + global.qualifiedName(c) + ", which has a destructor but no copy constructor");
      if (!needsDestruction(t, at))
      {
        std::string v = newTemp();
        fn += "  " + v + " = load " + llType(t, at) + ", ptr " + src + "\n";
        fn += "  store " + llType(t, at) + " " + v + ", ptr " + dst + "\n";
        return;
      }
      // Memberwise: bases and members in order, each with its own copy semantics.
      StructInfo info = structFor(t, at);
      if (info.polymorphic)
      {
        useVtable(info);
        fn += "  store ptr " + info.vtableName + ", ptr " + dst + "\n";
      }
      for (auto& [baseDef, index] : info.bases)
        copyConstruct(gep(info.name, dst, index), gep(info.name, src, index), info.fieldTypes[index], at);
      for (Decl* d : info.fieldDecls)
      {
        uint32_t index = info.fieldIndex[d];
        const Ty* ft = info.fieldTypes[index];
        std::string to = gep(info.name, dst, index), from = gep(info.name, src, index);
        if (isClass(ft))
          copyConstruct(to, from, ft, at);
        else
        {
          std::string v = newTemp();
          fn += "  " + v + " = load " + llType(ft, at) + ", ptr " + from + "\n";
          fn += "  store " + llType(ft, at) + " " + v + ", ptr " + to + "\n";
        }
      }
    }

    // Implicit copy assignment: user operator= where one exists, memberwise otherwise.
    void assignObject(const std::string& dst, const std::string& src, const Ty* t, Node* at)
    {
      const Ty* foundIn = nullptr;
      std::vector<const Ty*> argTypes{types.lref(types.qualified(t, true, false))};
      if (FunctionDecl* op = findMemberFunction(t, "operator=", argTypes, t, at, &foundIn))
      {
        std::string thisArg = dst;
        if (canonicalClass(t, at) != foundIn)
          baseAddress(t, foundIn, dst, thisArg, at);
        std::string name = useFunction(op, global.classArgsOf(foundIn), foundIn);
        Signature s;
        {
          TemplateArgs args = global.classArgsOf(foundIn);
          ArgsScope scope(*this, &args);
          s = signatureOf(op);
        }
        if (s.returnType == "void")
          fn += "  call void " + name + "(ptr " + thisArg + ", ptr " + src + ")\n";
        else
        {
          std::string r = newTemp();
          fn += "  " + r + " = call " + s.returnType + " " + name + "(ptr " + thisArg + ", ptr " + src + ")\n";
        }
        return;
      }
      if (!needsDestruction(t, at))
      {
        std::string v = newTemp();
        fn += "  " + v + " = load " + llType(t, at) + ", ptr " + src + "\n";
        fn += "  store " + llType(t, at) + " " + v + ", ptr " + dst + "\n";
        return;
      }
      if (findDestructor(classDef(t->decl, at)))
        unsupported(at, "assignment to " + global.qualifiedName(t->decl) + ", which has a destructor but no operator=");
      StructInfo info = structFor(t, at);
      for (auto& [baseDef, index] : info.bases)
        assignObject(gep(info.name, dst, index), gep(info.name, src, index), info.fieldTypes[index], at);
      for (Decl* d : info.fieldDecls)
      {
        uint32_t index = info.fieldIndex[d];
        const Ty* ft = info.fieldTypes[index];
        std::string to = gep(info.name, dst, index), from = gep(info.name, src, index);
        if (isClass(ft))
          assignObject(to, from, ft, at);
        else
        {
          std::string v = newTemp();
          fn += "  " + v + " = load " + llType(ft, at) + ", ptr " + from + "\n";
          fn += "  store " + llType(ft, at) + " " + v + ", ptr " + to + "\n";
        }
      }
    }

    // Initialises the object/scalar at addr of type t from an initializer expression,
    // constructing in place where the expression is a temporary.
    void initializeFrom(const std::string& addr, const Ty* t, Expr* init, Node* at)
    {
      t = types.stripRef(t);
      if (isLambda(t))
      {
        Value v = rvalue(init);
        if (!isLambda(v.ty))
          unsupported(at, "initialising a lambda variable from something else");
        std::string tmp = newTemp();
        fn += "  " + tmp + " = load " + llType(t, at) + ", ptr " + v.text + "\n";
        fn += "  store " + llType(t, at) + " " + tmp + ", ptr " + addr + "\n";
        return;
      }
      if (isClass(t))
      {
        if (init->kind == NodeKind::FunctionalCastExpr && isClass(types.stripRef(init->ty)))
        {
          constructInto(addr, t, static_cast<FunctionalCastExpr*>(init)->args, at);
          return;
        }
        if (init->kind == NodeKind::BraceInitExpr)
        {
          constructInto(addr, t, static_cast<BraceInitExpr*>(init)->args, at);
          return;
        }
        if (init->kind == NodeKind::CallExpr && isClass(types.stripRef(init->ty)) && !init->ty->isReference())
        {
          callValue(static_cast<CallExpr*>(init), addr);
          return;
        }
        if (init->kind == NodeKind::ParenExpr)
        {
          initializeFrom(addr, t, static_cast<ParenExpr*>(init)->inner, at);
          return;
        }
        Value src = rvalue(init);
        if (!isClass(src.ty))
          unsupported(at, "initialising a class from a non-class value");
        if (src.isTemporary)
          moveTemporary(addr, src, t, at);
        else
          copyConstruct(addr, src.text, t, at);
        return;
      }
      if (init->kind == NodeKind::BraceInitExpr)
      {
        auto* bi = static_cast<BraceInitExpr*>(init);
        if (bi->args.empty())
        {
          fn += "  store " + llType(t, at) + " zeroinitializer, ptr " + addr + "\n";
          return;
        }
        if (bi->args.size() != 1)
          unsupported(at, "this brace initializer");
        init = bi->args[0];
      }
      store(rvalue(init), addr, t, at);
    }

    // A temporary that is used to initialise an object is moved bitwise and no longer destroyed.
    void moveTemporary(const std::string& dst, Value src, const Ty* t, Node* at)
    {
      for (size_t i = statementTemps.size(); i-- > 0;)
        if (statementTemps[i].addr == src.text)
        {
          statementTemps.erase(statementTemps.begin() + i);
          break;
        }
      std::string v = newTemp();
      fn += "  " + v + " = load " + llType(t, at) + ", ptr " + src.text + "\n";
      fn += "  store " + llType(t, at) + " " + v + ", ptr " + dst + "\n";
    }

    std::string newObjectSlot(const Ty* t, Node* at)
    {
      std::string slot = "%o" + std::to_string(temp++);
      allocas += "  " + slot + " = alloca " + llType(t, at) + "\n";
      return slot;
    }

    // --- parameter packs ---

    // The pack parameter an expression refers to (the first one found), or null.
    Decl* packIn(Node* n, int depth = 0)
    {
      if (!n || depth > 32)
        return nullptr;
      switch (n->kind)
      {
        case NodeKind::NameExpr:
        {
          Decl* d = static_cast<NameExpr*>(n)->resolved;
          return d && packLocals.count(d) ? d : nullptr;
        }
        case NodeKind::ParenExpr: return packIn(static_cast<ParenExpr*>(n)->inner, depth + 1);
        case NodeKind::UnaryExpr: return packIn(static_cast<UnaryExpr*>(n)->operand, depth + 1);
        case NodeKind::CastExpr: return packIn(static_cast<CastExpr*>(n)->operand, depth + 1);
        case NodeKind::NamedCastExpr: return packIn(static_cast<NamedCastExpr*>(n)->operand, depth + 1);
        case NodeKind::MemberExpr: return packIn(static_cast<MemberExpr*>(n)->base, depth + 1);
        case NodeKind::BinaryExpr:
          if (Decl* d = packIn(static_cast<BinaryExpr*>(n)->lhs, depth + 1))
            return d;
          return packIn(static_cast<BinaryExpr*>(n)->rhs, depth + 1);
        case NodeKind::CallExpr:
        {
          auto* c = static_cast<CallExpr*>(n);
          if (Decl* d = packIn(c->callee, depth + 1))
            return d;
          for (Expr* a : c->args)
            if (Decl* d = packIn(a, depth + 1))
              return d;
          return nullptr;
        }
        case NodeKind::IndexExpr:
        {
          auto* i = static_cast<IndexExpr*>(n);
          if (Decl* d = packIn(i->base, depth + 1))
            return d;
          for (Expr* a : i->args)
            if (Decl* d = packIn(a, depth + 1))
              return d;
          return nullptr;
        }
        case NodeKind::FunctionalCastExpr:
          for (Expr* a : static_cast<FunctionalCastExpr*>(n)->args)
            if (Decl* d = packIn(a, depth + 1))
              return d;
          return nullptr;
        default:
          return nullptr;
      }
    }

    // The template parameter pack behind a pack parameter.
    Decl* templatePackOf(Decl* param)
    {
      for (Decl* p = param->parent; p; p = p->parent)
        for (Decl* tp : p->templateParams)
          if (static_cast<TemplateParamDecl*>(tp)->isPack)
            return tp;
      return nullptr;
    }

    // Runs body(i) for each element of the pack that `pattern` expands over, with the pack
    // parameter bound to that element and the function body re-typed for it.
    template<typename F>
    void forEachPackElement(Expr* pattern, Node* at, F&& body)
    {
      Decl* param = packIn(pattern);
      if (!param)
        unsupported(at, "a pack expansion that does not name a parameter pack");
      Decl* tp = templatePackOf(param);
      if (!tp || !currentArgs)
        unsupported(at, "a pack expansion outside an instantiation");
      std::vector<Local> elements = packLocals[param];
      const Ty* packTy = nullptr;
      for (auto& [d, t] : currentArgs->pairs)
        if (d == tp)
          packTy = t;
      if (!packTy || packTy->kind != TyKind::Pack || packTy->args.size() != elements.size())
        unsupported(at, "a pack whose elements are unknown");
      TemplateArgs original = *currentArgs;
      for (size_t i = 0; i < elements.size(); ++i)
      {
        TemplateArgs elementArgs = original;
        for (auto& [d, t] : elementArgs.pairs)
          if (d == tp)
            t = packTy->args[i];
        global.retypeFor(currentFunction, elementArgs, currentClassTy);
        locals[param] = elements[i];
        body(i);
      }
      locals.erase(param);
      global.retypeFor(currentFunction, original, currentClassTy);
    }

    bool isPackExpansion(Expr* e)
    {
      return e && e->kind == NodeKind::UnaryExpr && !static_cast<UnaryExpr*>(e)->isKeyword &&
             P(static_cast<UnaryExpr*>(e)->op) == P::Ellipsis && static_cast<UnaryExpr*>(e)->postfix;
    }

    // --- lambdas ---

    // Collects the enclosing locals a lambda body refers to (for default captures) and whether
    // it uses `this`.
    void scanCaptures(Node* n, std::vector<Decl*>& found, bool& usesThis, int depth = 0)
    {
      if (!n || depth > 64)
        return;
      auto visit = [&](Node* c) { scanCaptures(c, found, usesThis, depth + 1); };
      auto visitExprs = [&](Span<Expr*> es) { for (Expr* e : es) visit(e); };
      switch (n->kind)
      {
        case NodeKind::NameExpr:
        {
          auto* e = static_cast<NameExpr*>(n);
          Decl* d = e->resolved;
          if (d && locals.count(d) && std::find(found.begin(), found.end(), d) == found.end())
            found.push_back(d);
          if (d && d->kind == NodeKind::VariableDecl && !locals.count(d) && global.owningClass(d) && !(d->flags & DF_Static))
            usesThis = true;
          if (d && d->kind == NodeKind::FunctionDecl && global.owningClass(d) && !(d->flags & DF_Static))
            usesThis = true;
          break;
        }
        case NodeKind::ThisExpr: usesThis = true; break;
        case NodeKind::CallExpr: visit(static_cast<CallExpr*>(n)->callee); visitExprs(static_cast<CallExpr*>(n)->args); break;
        case NodeKind::IndexExpr: visit(static_cast<IndexExpr*>(n)->base); visitExprs(static_cast<IndexExpr*>(n)->args); break;
        case NodeKind::MemberExpr: visit(static_cast<MemberExpr*>(n)->base); break;
        case NodeKind::UnaryExpr: visit(static_cast<UnaryExpr*>(n)->operand); break;
        case NodeKind::BinaryExpr: visit(static_cast<BinaryExpr*>(n)->lhs); visit(static_cast<BinaryExpr*>(n)->rhs); break;
        case NodeKind::ConditionalExpr:
          visit(static_cast<ConditionalExpr*>(n)->cond);
          visit(static_cast<ConditionalExpr*>(n)->then);
          visit(static_cast<ConditionalExpr*>(n)->else_);
          break;
        case NodeKind::CastExpr: visit(static_cast<CastExpr*>(n)->operand); break;
        case NodeKind::NamedCastExpr: visit(static_cast<NamedCastExpr*>(n)->operand); break;
        case NodeKind::FunctionalCastExpr: visitExprs(static_cast<FunctionalCastExpr*>(n)->args); break;
        case NodeKind::SizeofExpr: visit(static_cast<SizeofExpr*>(n)->operand); break;
        case NodeKind::NewExpr: visitExprs(static_cast<NewExpr*>(n)->placement); visitExprs(static_cast<NewExpr*>(n)->args); break;
        case NodeKind::DeleteExpr: visit(static_cast<DeleteExpr*>(n)->operand); break;
        case NodeKind::BraceInitExpr: visitExprs(static_cast<BraceInitExpr*>(n)->args); break;
        case NodeKind::ThrowExpr: visit(static_cast<ThrowExpr*>(n)->operand); break;
        case NodeKind::ParenExpr: visit(static_cast<ParenExpr*>(n)->inner); break;
        case NodeKind::LambdaExpr:
        {
          auto* l = static_cast<LambdaExpr*>(n);
          for (const LambdaCapture& c : l->captures)
            visit(c.init);
          visit(l->body);
          break;
        }
        case NodeKind::BlockStmt: for (Stmt* s : static_cast<BlockStmt*>(n)->stmts) visit(s); break;
        case NodeKind::ExprStmt: visit(static_cast<ExprStmt*>(n)->expr); break;
        case NodeKind::DeclStmt:
          for (Decl* d : static_cast<DeclStmt*>(n)->decls)
            if (d->kind == NodeKind::VariableDecl)
              visit(static_cast<VariableDecl*>(d)->init);
          break;
        case NodeKind::IfStmt:
          visit(static_cast<IfStmt*>(n)->init);
          visit(static_cast<IfStmt*>(n)->cond);
          visit(static_cast<IfStmt*>(n)->then);
          visit(static_cast<IfStmt*>(n)->else_);
          break;
        case NodeKind::ForStmt:
          visit(static_cast<ForStmt*>(n)->init);
          visit(static_cast<ForStmt*>(n)->cond);
          visit(static_cast<ForStmt*>(n)->step);
          visit(static_cast<ForStmt*>(n)->body);
          break;
        case NodeKind::RangeForStmt: visit(static_cast<RangeForStmt*>(n)->range); visit(static_cast<RangeForStmt*>(n)->body); break;
        case NodeKind::WhileStmt: visit(static_cast<WhileStmt*>(n)->cond); visit(static_cast<WhileStmt*>(n)->body); break;
        case NodeKind::DoStmt: visit(static_cast<DoStmt*>(n)->body); visit(static_cast<DoStmt*>(n)->cond); break;
        case NodeKind::SwitchStmt:
          visit(static_cast<SwitchStmt*>(n)->init);
          visit(static_cast<SwitchStmt*>(n)->cond);
          visit(static_cast<SwitchStmt*>(n)->body);
          break;
        case NodeKind::CaseStmt: visit(static_cast<CaseStmt*>(n)->value); break;
        case NodeKind::ReturnStmt: visit(static_cast<ReturnStmt*>(n)->value); break;
        case NodeKind::AssertStmt: visit(static_cast<AssertStmt*>(n)->cond); break;
        case NodeKind::NameofExpr: visit(static_cast<NameofExpr*>(n)->operand); break;
        default: break;
      }
    }

    Decl* localByName(uint32_t symbol)
    {
      for (auto& [d, local] : locals)
      {
        if (!d)
          continue;
        if (d->kind == NodeKind::VariableDecl && !static_cast<VariableDecl*>(d)->name.empty() &&
            static_cast<VariableDecl*>(d)->name.last().symbol == symbol)
          return d;
        if (d->kind == NodeKind::ParamDecl && static_cast<ParamDecl*>(d)->name == symbol)
          return d;
      }
      return nullptr;
    }

    // The closure of a lambda type, created from its capture list in the current function.
    ClosureInfo& closureFor(const Ty* t, Node* at)
    {
      auto it = closures.find(t->decl);
      if (it != closures.end())
        return it->second;
      auto* c = static_cast<ClassDecl*>(t->decl);
      LambdaExpr* l = c->lambda;
      if (!l)
        unsupported(at, "a lambda without its expression");
      ClosureInfo& info = closures[t->decl];
      info.structName = "%closure." + std::to_string(closures.size());
      info.enclosingClass = currentClassTy;
      bool usesThis = false;
      std::vector<Decl*> explicitDecls;
      bool thisCaptured = false;
      for (const LambdaCapture& cap : l->captures)
      {
        if (cap.init)
          unsupported(l, "init-captures");
        if (cap.isThis)
        {
          thisCaptured = true;
          continue;
        }
        Decl* d = localByName(cap.symbol);
        if (!d)
          unsupported(l, "a capture that is not a local variable (" + sym(cap.symbol) + ")");
        info.captured.push_back(d);
        info.byRef.push_back(cap.byRef);
      }
      if (l->defaultCapture != 0)
      {
        std::vector<Decl*> found;
        scanCaptures(l->body, found, usesThis);
        for (Decl* d : found)
          if (std::find(info.captured.begin(), info.captured.end(), d) == info.captured.end())
          {
            info.captured.push_back(d);
            info.byRef.push_back(l->defaultCapture == 2);
          }
        if (usesThis)
          thisCaptured = true;
      }
      for (size_t i = 0; i < info.captured.size(); ++i)
      {
        Local& local = locals[info.captured[i]];
        const Ty* vt = types.stripRef(local.ty);
        info.fieldTypes.push_back(vt);
      }
      if (thisCaptured)
      {
        if (!currentClassTy)
          unsupported(l, "capturing this outside a method");
        info.captured.push_back(nullptr);
        info.byRef.push_back(true);
        info.fieldTypes.push_back(currentClassTy);
      }
      std::string line = info.structName + " = type { ";
      for (size_t i = 0; i < info.fieldTypes.size(); ++i)
        line += (i ? ", " : "") + (info.byRef[i] ? std::string("ptr") : llType(info.fieldTypes[i], at));
      line += info.fieldTypes.empty() ? "i8 }\n" : " }\n";
      decls += line;
      const Ty* ft = t;
      std::string params;
      for (size_t i = 0; i < ft->args.size(); ++i)
        params += (i ? ", " : "") + types.toString(ft->args[i], &symbols);
      info.functionName = "@\"" + std::string(symbols.name(c->name.last().symbol)) + "::operator()(" + params + ")\"";
      return closures[t->decl];
    }

    // Builds the closure object of a lambda expression in a fresh slot and emits its function.
    Value makeClosure(LambdaExpr* l)
    {
      const Ty* t = l->ty;
      if (!isLambda(t))
        unsupported(l, "a lambda whose type is unknown");
      ClosureInfo info = closureFor(t, l);
      std::string slot = newObjectSlot(t, l);
      for (size_t i = 0; i < info.captured.size(); ++i)
      {
        std::string field = gep(info.structName, slot, uint32_t(i));
        Decl* d = info.captured[i];
        if (!d)
        {
          fn += "  store ptr " + thisPointer(l) + ", ptr " + field + "\n";
          continue;
        }
        Local& local = locals[d];
        std::string addr = local.addr;
        if (local.isRef)
        {
          addr = newTemp();
          fn += "  " + addr + " = load ptr, ptr " + local.addr + "\n";
        }
        if (info.byRef[i])
          fn += "  store ptr " + addr + ", ptr " + field + "\n";
        else if (isAggregate(info.fieldTypes[i]))
        {
          if (isClass(info.fieldTypes[i]))
            copyConstruct(field, addr, info.fieldTypes[i], l);
          else
          {
            std::string v = newTemp();
            fn += "  " + v + " = load " + llType(info.fieldTypes[i], l) + ", ptr " + addr + "\n";
            fn += "  store " + llType(info.fieldTypes[i], l) + " " + v + ", ptr " + field + "\n";
          }
        }
        else
          store(load(addr, info.fieldTypes[i], l), field, info.fieldTypes[i], l);
      }
      if (!closures[t->decl].emitted)
      {
        closures[t->decl].emitted = true;
        emitLambda(l, t);
      }
      return {slot, t, false};
    }

    struct FunctionState
    {
      std::string fn, allocas, currentBlock, sretAddr;
      std::unordered_map<Decl*, Local> locals;
      std::vector<Loop> loops;
      std::vector<std::vector<Object>> scopes;
      std::vector<Object> statementTemps;
      uint32_t temp, label;
      bool blockTerminated;
      FunctionDecl* currentFunction;
      const Ty* currentReturn;
      const Ty* currentClassTy;
    };

    FunctionState saveState()
    {
      return {fn, allocas, currentBlock, sretAddr, locals, loops, scopes, statementTemps, temp, label, blockTerminated,
              currentFunction, currentReturn, currentClassTy};
    }

    void restoreState(FunctionState& s)
    {
      fn = s.fn;
      allocas = s.allocas;
      currentBlock = s.currentBlock;
      sretAddr = s.sretAddr;
      locals = s.locals;
      loops = s.loops;
      scopes = s.scopes;
      statementTemps = s.statementTemps;
      temp = s.temp;
      label = s.label;
      blockTerminated = s.blockTerminated;
      currentFunction = s.currentFunction;
      currentReturn = s.currentReturn;
      currentClassTy = s.currentClassTy;
    }

    // Emits the lambda's function while another function is being emitted: the enclosing
    // function's state is saved and restored around it.
    void emitLambda(LambdaExpr* l, const Ty* t)
    {
      ClosureInfo info = closureFor(t, l);
      FunctionState saved = saveState();
      fn.clear();
      allocas.clear();
      locals.clear();
      loops.clear();
      scopes.clear();
      statementTemps.clear();
      temp = 0;
      label = 0;
      blockTerminated = false;
      currentBlock = "entry";
      currentReturn = t->inner ? t->inner : types.voidType();
      sretAddr.clear();
      currentClassTy = info.enclosingClass;
      try
      {
        std::string params = "ptr %closure";
        bool hasSret = isClass(currentReturn);
        if (hasSret)
        {
          params += ", ptr %sret";
          sretAddr = "%sret";
        }
        std::vector<std::string> paramNames;
        for (size_t i = 0; i < t->args.size(); ++i)
        {
          const Ty* pt = t->args[i];
          std::string pn = "%p" + std::to_string(i);
          paramNames.push_back(pn);
          params += ", " + std::string(isAggregate(pt) || pt->isReference() ? "ptr" : llType(pt, l)) + " " + pn;
        }
        std::string retType = hasSret ? "void" : currentReturn->isReference() ? "ptr" : llType(currentReturn, l);
        std::string header = "define " + retType + " " + info.functionName + "(" + params + ") {\nentry:\n";
        pushScope();
        // Captures become locals: by-value captures live in the closure, references point out.
        for (size_t i = 0; i < info.captured.size(); ++i)
        {
          std::string field = gep(info.structName, "%closure", uint32_t(i));
          Decl* d = info.captured[i];
          if (!d)
          {
            std::string thisSlot = "%v" + std::to_string(temp++);
            allocas += "  " + thisSlot + " = alloca ptr\n";
            std::string loaded = newTemp();
            fn += "  " + loaded + " = load ptr, ptr " + field + "\n";
            fn += "  store ptr " + loaded + ", ptr " + thisSlot + "\n";
            locals[nullptr] = {thisSlot, nullptr, false};
            continue;
          }
          locals[d] = {field, info.byRef[i] ? types.lref(info.fieldTypes[i]) : info.fieldTypes[i], info.byRef[i]};
        }
        for (size_t i = 0; i < l->params.size() && i < t->args.size(); ++i)
        {
          const Ty* pt = t->args[i];
          if (isAggregate(pt))
          {
            locals[l->params[i]] = {paramNames[i], pt, false};
            if (needsDestruction(pt, l))
              scopes.back().push_back({paramNames[i], pt});
            continue;
          }
          bool isRef = pt->isReference();
          std::string ps = "%v" + std::to_string(temp++);
          allocas += "  " + ps + " = alloca " + (isRef ? std::string("ptr") : llType(pt, l)) + "\n";
          fn += "  store " + (isRef ? std::string("ptr") : llType(pt, l)) + " " + paramNames[i] + ", ptr " + ps + "\n";
          locals[l->params[i]] = {ps, pt, isRef};
        }
        statement(l->body);
        if (!blockTerminated)
        {
          popScope();
          if (currentReturn->kind == TyKind::Void || hasSret)
            fn += "  ret void\n";
          else
            fn += "  unreachable\n";
        }
        addDefinition(info.functionName, header + allocas + fn + "}\n\n");
        ++result.functions;
      }
      catch (Unsupported&)
      {
        restoreState(saved);
        throw;
      }
      restoreState(saved);
    }

    // Calls a closure value with arguments.
    Value callClosure(Value closure, Span<Expr*> callArgs, const std::string& sret, Node* at)
    {
      const Ty* t = closure.ty;
      ClosureInfo info = closureFor(t, at);
      const Ty* ret = t->inner ? t->inner : types.voidType();
      std::string argText = "ptr " + closure.text;
      std::string resultAddr = sret;
      bool hasSret = isClass(ret);
      if (hasSret)
      {
        if (resultAddr.empty())
        {
          resultAddr = newObjectSlot(ret, at);
          statementTemps.push_back({resultAddr, ret});
        }
        argText += ", ptr " + resultAddr;
      }
      std::string sig = (hasSret ? "void" : ret->isReference() ? "ptr" : llType(ret, at)) + std::string(" (ptr") + (hasSret ? ", ptr" : "");
      size_t position = 0;
      for (Expr* arg : callArgs)
      {
        if (isPackExpansion(arg))
        {
          Expr* pattern = static_cast<UnaryExpr*>(arg)->operand;
          forEachPackElement(pattern, at, [&](size_t)
          {
            const Ty* pt = position < t->args.size() ? t->args[position] : nullptr;
            if (!pt)
              unsupported(at, "too many arguments to a lambda");
            sig += ", " + std::string(isAggregate(pt) || pt->isReference() ? "ptr" : llType(pt, at));
            argText += ", " + argumentText(pt, pattern, at);
            ++position;
          });
          continue;
        }
        const Ty* pt = position < t->args.size() ? t->args[position] : nullptr;
        if (!pt)
          unsupported(at, "too many arguments to a lambda");
        sig += ", " + std::string(isAggregate(pt) || pt->isReference() ? "ptr" : llType(pt, at));
        argText += ", " + argumentText(pt, arg, at);
        ++position;
      }
      sig += ")";
      if (hasSret || ret->kind == TyKind::Void)
      {
        fn += "  call " + sig + " " + info.functionName + "(" + argText + ")\n";
        return hasSret ? Value{resultAddr, ret, sret.empty()} : Value{"", types.voidType(), false};
      }
      std::string r = newTemp();
      fn += "  " + r + " = call " + sig + " " + info.functionName + "(" + argText + ")\n";
      return {r, ret, false};
    }

    // --- functions ---

    // IR name of a function (for an instantiation: with the class and template arguments).
    std::string functionName(FunctionDecl* f, const TemplateArgs& args, const Ty* classTy)
    {
      if (global.isExternC(f))
        return "@" + sym(f->name.last().symbol);
      if (f->name.parts.size() == 1 && sym(f->name.parts[0].symbol) == "main" && !global.owningClass(f))
        return "@main";
      ArgsScope scope(*this, &args);
      std::string q;
      if (classTy && !classTy->args.empty())
      {
        // Class path with the instantiation's arguments, then the member's own name.
        std::string full = global.qualifiedName(f);
        std::string cls = global.qualifiedName(classTy->decl);
        q = types.toString(classTy, &symbols);
        size_t pos = full.rfind("::");
        q += pos == std::string::npos ? "::" + full : full.substr(pos);
      }
      else
      {
        q = global.qualifiedName(f);
        if (Decl* owner = global.owningClass(f); owner && owner->kind == NodeKind::ClassDecl && (owner->flags & DF_Specialization))
        {
          // Members of `template<> struct Limits<int>`: the arguments tell the specialisations apart.
          const NamePart& last = static_cast<ClassDecl*>(owner)->name.last();
          std::string spelled;
          for (Node* a : last.templateArgs)
          {
            const Ty* t = nullptr;
            switch (a->kind)
            {
              case NodeKind::NamedType: case NodeKind::PointerType: case NodeKind::LRefType: case NodeKind::RRefType:
              case NodeKind::ConstType: case NodeKind::ArrayType: case NodeKind::FunctionType: case NodeKind::BuiltinType:
              case NodeKind::DecltypeType: case NodeKind::AutoType: case NodeKind::MemberPointerType:
                t = global.typeOfAst(static_cast<Type*>(a), owner);
                break;
              default: break;
            }
            std::string one;
            if (t && t->isKnown())
              one = types.toString(t, &symbols);
            else if (owner->file && owner->file->tokens && a->token < owner->file->tokens->size())
            {
              const Token& tok = (*owner->file->tokens)[a->token];
              one = std::string(owner->file->text.substr(tok.offset, tok.length));
            }
            spelled += (spelled.empty() ? "" : ", ") + one;
          }
          size_t pos = q.rfind("::");
          if (pos != std::string::npos)
            q.insert(pos, "<" + spelled + ">");
        }
      }
      if (!f->templateParams.empty())
      {
        q += "<";
        for (size_t i = 0; i < f->templateParams.size(); ++i)
        {
          const Ty* t = nullptr;
          for (auto& [d, ty] : args.pairs)
            if (d == f->templateParams[i])
              t = ty;
          q += (i ? ", " : "") + (t ? types.toString(t, &symbols) : std::string("?"));
        }
        q += ">";
      }
      const Ty* ft = declType(f);
      std::string params;
      for (size_t i = 0; ft && i < ft->args.size(); ++i)
        params += (i ? ", " : "") + types.toString(ft->args[i], &symbols);
      // A const method is a different function from its non-const overload.
      return "@\"" + q + "(" + params + ")" + ((f->flags & DF_Const) && !(f->flags & DF_Static) ? " const" : "") + "\"";
    }

    struct Signature
    {
      std::string returnType;
      std::vector<std::string> paramTypes;
      std::vector<const Ty*> paramTys; // null for this/sret
      bool hasThis = false, hasSret = false, variadic = false;
    };

    // Signature under the arguments currently in effect.
    Signature signatureOf(FunctionDecl* f)
    {
      const Ty* ft = declType(f);
      if (!ft || ft->kind != TyKind::Function)
        unsupported(f, "a function whose type is unknown");
      Signature s;
      bool method = global.owningClass(f) && !(f->flags & DF_Static) && !global.isExternC(f);
      const Ty* ret = ft->inner;
      if (isClass(ret))
      {
        s.hasSret = true;
        s.returnType = "void";
      }
      else
        s.returnType = ret->isReference() ? "ptr" : llType(ret, f);
      if (method)
      {
        s.hasThis = true;
        s.paramTypes.push_back("ptr");
        s.paramTys.push_back(nullptr);
      }
      if (s.hasSret)
      {
        s.paramTypes.push_back("ptr");
        s.paramTys.push_back(nullptr);
      }
      for (const Ty* pt : ft->args)
      {
        if (!pt->isKnown() || types.isDependent(pt))
          unsupported(f, "a parameter of unknown type");
        s.paramTypes.push_back(isAggregate(pt) || pt->isReference() ? "ptr" : llType(pt, f));
        s.paramTys.push_back(pt);
      }
      s.variadic = ft->variadic;
      return s;
    }

    std::string signatureText(const Signature& s)
    {
      std::string t = s.returnType + " (";
      for (size_t i = 0; i < s.paramTypes.size(); ++i)
        t += (i ? ", " : "") + s.paramTypes[i];
      if (s.variadic)
        t += s.paramTypes.empty() ? "..." : ", ...";
      t += ")";
      return t;
    }

    // Makes a function usable from the current function: declares external ones, queues
    // instantiations. Returns its IR name.
    std::string useFunction(FunctionDecl* f, const TemplateArgs& args, const Ty* classTy)
    {
      std::string name = functionName(f, args, classTy);
      Decl* defDecl = global.definitionOf(f);
      FunctionDecl* def = defDecl && defDecl->kind == NodeKind::FunctionDecl ? static_cast<FunctionDecl*>(defDecl) : f;
      bool templated = !args.empty();
      if (def->file && def->file->isImported)
        def = f, templated = false; // inline C functions are declared, the library provides them
      if (def->body && templated)
      {
        if (!requested[name])
        {
          requested[name] = true;
          worklist.push_back({def, args, classTy, currentModule});
        }
        return name;
      }
      if (def->body && !(def->file && def->file->isImported))
        return name; // Defined by the main pass.
      if (declaredNames[name])
        return name;
      declaredNames[name] = true;
      if (f->builtin == BUILTIN_EnumStr)
      {
        emitEnumStr(f, name);
        return name;
      }
      ArgsScope scope(*this, &args);
      Signature s = signatureOf(f);
      std::string params;
      for (size_t i = 0; i < s.paramTypes.size(); ++i)
        params += (i ? ", " : "") + s.paramTypes[i];
      if (s.variadic)
        params += params.empty() ? "..." : ", ...";
      decls += "declare " + s.returnType + " " + name + "(" + params + ")\n";
      return name;
    }

    static size_t enumeratorCount(Decl* e)
    {
      size_t n = 0;
      for (Decl* d : static_cast<EnumDecl*>(e)->enumerators)
        n += d->kind == NodeKind::EnumeratorDecl;
      return n;
    }

    // The generated `const char* str() const` of a Kov enum: a switch over the enumerators.
    void emitEnumStr(FunctionDecl* f, const std::string& name)
    {
      emittingShared = true;
      auto* e = static_cast<EnumDecl*>(f->parent);
      std::string k = llType(enumUnderlying(e), f), cases, blocks;
      int i = 0;
      for (Decl* d : e->enumerators)
        if (d->kind == NodeKind::EnumeratorDecl)
        {
          auto* en = static_cast<EnumeratorDecl*>(d);
          std::string l = "e" + std::to_string(i++);
          cases += "    " + k + " " + std::to_string(enumeratorValue(en)) + ", label %" + l + "\n";
          blocks += l + ":\n  ret ptr " + cStringConstant(sym(en->name)) + "\n";
        }
      shared += "$" + name.substr(1) + " = comdat any\n"
                "define linkonce_odr ptr " + name + "(ptr %this) comdat {\nentry:\n  %v = load " + k + ", ptr %this\n  switch " + k +
                " %v, label %other [\n" + cases + "  ]\n" + blocks + "other:\n  ret ptr " + cStringConstant("?") + "\n}\n";
      emittingShared = false;
    }

    // Template arguments for calling f on an object of classTy, with the call's deduced arguments.
    TemplateArgs calleeArgs(FunctionDecl* f, const Ty* classTy, CallExpr* call, Node* at)
    {
      TemplateArgs args;
      if (classTy)
        args = global.classArgsOf(canonicalClass(classTy, at));
      if (!f->templateParams.empty())
      {
        if (!call || call->templateArgCount != f->templateParams.size())
          unsupported(at, "a call to " + global.qualifiedName(f) + " whose template arguments could not be deduced");
        for (size_t i = 0; i < f->templateParams.size(); ++i)
          args.pairs.push_back({f->templateParams[i], call->templateArgs[i]});
      }
      return args;
    }

    // Emits a call. thisArg/sret may be empty. For class returns the result is the sret address.
    Value callFunction(FunctionDecl* f, const TemplateArgs& args, const Ty* classTy, const std::string& thisArg,
                       const std::string& sret, Span<Expr*> callArgs, Node* at, const std::string& indirect = "")
    {
      if (!conditionHolds(f))
        return {"", types.voidType(), false}; // conditional(false): no call, arguments not evaluated
      std::string name = indirect.empty() ? useFunction(f, args, classTy) : indirect;
      Signature s;
      const Ty* ret;
      {
        ArgsScope scope(*this, &args);
        s = signatureOf(f);
        ret = declType(f)->inner;
      }
      std::string argText;
      size_t slot = 0;
      if (s.hasThis)
      {
        if (thisArg.empty())
          unsupported(at, "a method call without an object");
        argText += "ptr " + thisArg;
        ++slot;
      }
      std::string resultAddr = sret;
      if (s.hasSret)
      {
        if (resultAddr.empty())
        {
          resultAddr = newObjectSlot(ret, at);
          statementTemps.push_back({resultAddr, ret});
        }
        argText += std::string(argText.empty() ? "" : ", ") + "ptr " + resultAddr;
        ++slot;
      }
      size_t position = slot;
      for (Expr* arg : callArgs)
      {
        if (isPackExpansion(arg))
        {
          Expr* pattern = static_cast<UnaryExpr*>(arg)->operand;
          std::vector<std::string> texts;
          forEachPackElement(pattern, at, [&](size_t)
          {
            const Ty* pt = position < s.paramTys.size() ? s.paramTys[position] : nullptr;
            texts.push_back(argumentText(pt, pattern, at));
            ++position;
          });
          for (const std::string& t : texts)
            argText += std::string(argText.empty() ? "" : ", ") + t;
          continue;
        }
        const Ty* pt = position < s.paramTys.size() ? s.paramTys[position] : nullptr;
        argText += std::string(argText.empty() ? "" : ", ") + argumentText(pt, arg, at);
        ++position;
      }
      if (callArgs.size() < f->params.size())
      {
        // Parameters the call left out take their default arguments, evaluated at the call site
        // (the expression's literals belong to the callee's file; a `caller` location to this one).
        ParsedFile* callerFile = currentFile;
        for (size_t i = callArgs.size(); i < f->params.size(); ++i)
        {
          auto* pd = static_cast<ParamDecl*>(f->params[i]);
          if (!pd->defaultValue || pd->isPack)
            break;
          const Ty* pt = position < s.paramTys.size() ? s.paramTys[position] : nullptr;
          std::string text;
          if (pd->defaultValue->kind == NodeKind::CallerExpr)
            text = "ptr " + callerLocation(pt, at);
          else
          {
            currentFile = f->file;
            text = argumentText(pt, pd->defaultValue, at);
            currentFile = callerFile;
          }
          argText += std::string(argText.empty() ? "" : ", ") + text;
          ++position;
        }
      }
      std::string sig = (s.variadic || !indirect.empty()) ? signatureText(s) + " " : s.returnType + " ";
      if (s.returnType == "void")
      {
        fn += "  call " + sig + name + "(" + argText + ")\n";
        if (s.hasSret)
          return {resultAddr, ret, sret.empty()};
        return {"", types.voidType(), false};
      }
      std::string r = newTemp();
      fn += "  " + r + " = call " + sig + name + "(" + argText + ")\n";
      return {r, ret, false};
    }

    // One call argument as "<type> <operand>", converted to the parameter type (null: variadic).
    std::string argumentText(const Ty* pt, Expr* arg, Node* at)
    {
      if (pt && pt->isReference())
      {
        if (isAggregate(types.stripRef(pt)))
        {
          Value o = rvalue(arg);
          if (!isAggregate(o.ty))
            unsupported(at, "binding a non-class to a class reference");
          std::string addr = o.text;
          if (isClass(o.ty) && classKey(o.ty) != classKey(pt))
            if (!baseAddress(o.ty, types.stripRef(pt), o.text, addr, at))
              unsupported(at, "binding an unrelated class to a reference");
          return "ptr " + addr;
        }
        return "ptr " + lvalue(arg);
      }
      if (pt && isAggregate(pt))
      {
        Value v = rvalue(arg);
        if (!isAggregate(v.ty))
          unsupported(at, "passing a non-class to a class parameter");
        std::string copy = newObjectSlot(pt, at);
        if (isLambda(pt))
        {
          std::string tmp = newTemp();
          fn += "  " + tmp + " = load " + llType(pt, at) + ", ptr " + v.text + "\n";
          fn += "  store " + llType(pt, at) + " " + tmp + ", ptr " + copy + "\n";
        }
        else if (v.isTemporary)
          moveTemporary(copy, v, pt, at);
        else
          copyConstruct(copy, v.text, pt, at);
        return "ptr " + copy;
      }
      Value a = rvalue(arg);
      if (isAggregate(a.ty))
        unsupported(at, "passing a class where a scalar is expected");
      if (pt)
        a = convert(a, pt, at);
      else if (a.ty->kind == TyKind::Float && a.ty->bits == 32)
        a = convert(a, types.floatType(64), at);
      else if ((a.ty->kind == TyKind::Int || a.ty->kind == TyKind::Char) && a.ty->bits < 32)
        a = convert(a, types.intType(32), at);
      else if (a.ty->kind == TyKind::Bool)
        a = convert(a, types.intType(32), at);
      return llType(a.ty, at) + " " + a.text;
    }

    // --- constants ---

    std::string_view spelling(uint32_t token) const
    {
      const std::vector<Token>& toks = *currentFile->tokens;
      const Token& t = toks[std::min<size_t>(token, toks.size() - 1)];
      return currentFile->text.substr(t.offset, t.length);
    }

    std::optional<int64_t> evalConst(Expr* e, int depth = 0)
    {
      if (!e || depth > 32)
        return std::nullopt;
      switch (e->kind)
      {
        case NodeKind::LiteralExpr:
        {
          auto* l = static_cast<LiteralExpr*>(e);
          if (l->literalKind == TokenKind::Keyword)
            return l->keyword == Kw::true_ ? 1 : 0;
          if (l->literalKind == TokenKind::Number)
          {
            std::string digits;
            for (char c : spelling(l->token))
              if (c != '\'')
                digits += c;
            bool hex = digits.rfind("0x", 0) == 0 || digits.rfind("0X", 0) == 0;
            if (!hex && digits.find_first_of(".eE") != std::string::npos)
              return std::nullopt;
            while (!digits.empty() && strchr("uUlLzZ", digits.back()))
              digits.pop_back();
            try
            {
              return int64_t(std::stoull(digits, nullptr, 0));
            }
            catch (...)
            {
              return std::nullopt;
            }
          }
          if (l->literalKind == TokenKind::Char)
          {
            std::string s = decodeString(spelling(l->token));
            return s.empty() ? 0 : (unsigned char)s[0];
          }
          return std::nullopt;
        }
        case NodeKind::ParenExpr: return evalConst(static_cast<ParenExpr*>(e)->inner, depth + 1);
        case NodeKind::UnaryExpr:
        {
          auto* u = static_cast<UnaryExpr*>(e);
          auto v = evalConst(u->operand, depth + 1);
          if (!v || u->isKeyword)
            return std::nullopt;
          switch (P(u->op))
          {
            case P::Minus: return -*v;
            case P::Plus: return *v;
            case P::Tilde: return ~*v;
            case P::Bang: return !*v;
            default: return std::nullopt;
          }
        }
        case NodeKind::BinaryExpr:
        {
          auto* b = static_cast<BinaryExpr*>(e);
          auto l = evalConst(b->lhs, depth + 1);
          auto r = evalConst(b->rhs, depth + 1);
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
            case P::Amp: return *l & *r;
            case P::Pipe: return *l | *r;
            case P::Caret: return *l ^ *r;
            case P::Less: return *l < *r;
            case P::Greater: return *l > *r;
            case P::EqEq: return *l == *r;
            case P::NotEq: return *l != *r;
            default: return std::nullopt;
          }
        }
        case NodeKind::NameExpr:
        {
          Decl* d = static_cast<NameExpr*>(e)->resolved;
          if (!d)
            return std::nullopt;
          if (d->kind == NodeKind::EnumeratorDecl)
            return enumeratorValue(static_cast<EnumeratorDecl*>(d));
          if (d->kind == NodeKind::VariableDecl)
          {
            auto* v = static_cast<VariableDecl*>(d);
            if (v->builtin == BUILTIN_EnumCount && v->parent)
              return int64_t(enumeratorCount(v->parent));
            const Ty* t = declType(v);
            if (t && t->isConst && v->init && !locals.count(d))
            {
              ParsedFile* saved = currentFile;
              currentFile = v->file;
              auto r = evalConst(v->init, depth + 1);
              currentFile = saved;
              return r;
            }
          }
          return std::nullopt;
        }
        case NodeKind::MemberExpr:
        {
          Decl* d = static_cast<MemberExpr*>(e)->resolved;
          if (d && d->kind == NodeKind::EnumeratorDecl)
            return enumeratorValue(static_cast<EnumeratorDecl*>(d));
          return std::nullopt;
        }
        case NodeKind::SizeofExpr:
        {
          auto* s = static_cast<SizeofExpr*>(e);
          const Ty* t = nullptr;
          if (s->type && s->type->kind == NodeKind::NamedType && static_cast<NamedType*>(s->type)->resolved &&
              (static_cast<NamedType*>(s->type)->resolved->kind == NodeKind::VariableDecl ||
               static_cast<NamedType*>(s->type)->resolved->kind == NodeKind::ParamDecl))
            t = declType(static_cast<NamedType*>(s->type)->resolved);
          else if (s->type)
            t = currentArgs ? global.typeOfAstIn(s->type, currentFunction, *currentArgs) : global.typeOfAst(s->type, currentFunction);
          else
            t = s->operand ? s->operand->ty : nullptr;
          if (!t || !t->isKnown() || types.isDependent(t))
            return std::nullopt;
          return int64_t(sizeOf(types.stripRef(t), e));
        }
        case NodeKind::CastExpr: return evalConst(static_cast<CastExpr*>(e)->operand, depth + 1);
        case NodeKind::NamedCastExpr: return evalConst(static_cast<NamedCastExpr*>(e)->operand, depth + 1);
        case NodeKind::FunctionalCastExpr:
        {
          auto* c = static_cast<FunctionalCastExpr*>(e);
          return c->args.size() == 1 ? evalConst(c->args[0], depth + 1) : std::nullopt;
        }
        case NodeKind::ConditionalExpr:
        {
          auto* c = static_cast<ConditionalExpr*>(e);
          auto cond = evalConst(c->cond, depth + 1);
          if (!cond || !c->then)
            return std::nullopt;
          return evalConst(*cond ? c->then : c->else_, depth + 1);
        }
        default:
          return std::nullopt;
      }
    }

    int64_t enumeratorValue(EnumeratorDecl* en)
    {
      auto it = enumeratorValues.find(en);
      if (it != enumeratorValues.end())
        return it->second;
      auto* e = static_cast<EnumDecl*>(en->parent);
      int64_t next = 0;
      ParsedFile* saved = currentFile;
      if (e && e->file)
        currentFile = e->file;
      if (e)
        for (Decl* d : e->enumerators)
        {
          if (d->kind != NodeKind::EnumeratorDecl)
            continue;
          auto* cur = static_cast<EnumeratorDecl*>(d);
          if (cur->value)
            if (auto v = evalConst(cur->value))
              next = *v;
          enumeratorValues[cur] = next;
          ++next;
        }
      currentFile = saved;
      it = enumeratorValues.find(en);
      return it == enumeratorValues.end() ? 0 : it->second;
    }

    static std::string decodeString(std::string_view s)
    {
      size_t q = s.find_first_of("\"'");
      if (q == std::string_view::npos)
        return {};
      char quote = s[q];
      bool raw = q > 0 && s[q - 1] == 'R';
      std::string out;
      if (raw)
      {
        size_t open = s.find('(', q);
        size_t close = s.rfind(')');
        if (open != std::string_view::npos && close != std::string_view::npos && close > open)
          out.assign(s.substr(open + 1, close - open - 1));
        return out;
      }
      for (size_t i = q + 1; i < s.size() && s[i] != quote; ++i)
      {
        char c = s[i];
        if (c != '\\')
        {
          out += c;
          continue;
        }
        if (++i >= s.size())
          break;
        char e = s[i];
        switch (e)
        {
          case 'n': out += '\n'; break;
          case 't': out += '\t'; break;
          case 'r': out += '\r'; break;
          case '0': out += '\0'; break;
          case 'a': out += '\a'; break;
          case 'b': out += '\b'; break;
          case 'f': out += '\f'; break;
          case 'v': out += '\v'; break;
          case 'x':
          {
            int v = 0;
            while (i + 1 < s.size() && isxdigit((unsigned char)s[i + 1]))
            {
              ++i;
              v = v * 16 + (isdigit((unsigned char)s[i]) ? s[i] - '0' : (tolower(s[i]) - 'a' + 10));
            }
            out += char(v);
            break;
          }
          default: out += e; break;
        }
      }
      return out;
    }

    std::string stringConstant(LiteralExpr* l)
    {
      std::string bytes;
      const std::vector<Token>& toks = *currentFile->tokens;
      for (size_t i = l->token; i < toks.size() && toks[i].getKind() == TokenKind::String; ++i)
        bytes += decodeString(spelling(uint32_t(i)));
      return cStringConstant(bytes);
    }

    // A NUL-terminated string constant; returns its global name.
    std::string cStringConstant(std::string bytes)
    {
      std::string& out = emittingShared ? decls : moduleDecls[currentModule];
      bytes += '\0';
      std::string name = "@.str" + std::to_string(stringCount++);
      std::string escaped;
      for (unsigned char c : bytes)
      {
        if (c >= 0x20 && c < 0x7f && c != '"' && c != '\\')
          escaped += char(c);
        else
        {
          char buf[8];
          snprintf(buf, sizeof buf, "\\%02X", c);
          escaped += buf;
        }
      }
      out += name + " = private unnamed_addr constant [" + std::to_string(bytes.size()) + " x i8] c\"" + escaped + "\"\n";
      return name;
    }

    // The vtable of a class is defined in every module that stores it (linkonce_odr).
    void useVtable(const StructInfo& info)
    {
      if (info.vtableText.empty() || !vtablesIn[currentModule].insert(info.vtableName).second)
        return;
      moduleDecls[currentModule] += info.vtableText;
    }

    // Line and column (1-based) of a token of the current file.
    std::pair<uint32_t, uint32_t> lineColumnOf(uint32_t token)
    {
      const std::vector<Token>& toks = *currentFile->tokens;
      const Token& t = toks[std::min<size_t>(token, toks.size() - 1)];
      uint32_t line = 1, lineStart = 0;
      for (uint32_t i = 0; i < t.offset && i < currentFile->text.size(); ++i)
        if (currentFile->text[i] == '\n')
        {
          ++line;
          lineStart = i + 1;
        }
      return {line, t.offset - lineStart + 1};
    }

    // The source text from one token up to (not including) another.
    std::string sourceText(uint32_t fromToken, uint32_t toToken)
    {
      const std::vector<Token>& toks = *currentFile->tokens;
      if (fromToken >= toks.size() || toToken <= fromToken)
        return "";
      const Token& first = toks[fromToken];
      const Token& last = toks[std::min<size_t>(toToken - 1, toks.size() - 1)];
      return std::string(currentFile->text.substr(first.offset, last.offset + last.length - first.offset));
    }

    // Builds a CallerLocation {file, line, column, function} for the call at `at`, in a fresh slot.
    std::string callerLocation(const Ty* pt, Node* at)
    {
      const Ty* t = types.stripRef(pt);
      if (!t || !isClass(t))
        unsupported(at, "a caller location parameter whose type is not a class");
      StructInfo& info = structFor(t, at);
      if (info.fieldTypes.size() < 4)
        unsupported(at, "a caller location class without its four fields (file, line, column, function)");
      std::string addr = newObjectSlot(t, at);
      uint32_t token = at->kind == NodeKind::CallExpr ? static_cast<CallExpr*>(at)->callee->token : at->token; // the callee's name
      auto [line, column] = lineColumnOf(token);
      std::string function = currentFunction ? global.qualifiedName(currentFunction) : std::string();
      fn += "  store ptr " + cStringConstant(currentFile->path) + ", ptr " + gep(info.name, addr, 0) + "\n";
      fn += "  store i32 " + std::to_string(line) + ", ptr " + gep(info.name, addr, 1) + "\n";
      fn += "  store i32 " + std::to_string(column) + ", ptr " + gep(info.name, addr, 2) + "\n";
      fn += "  store ptr " + cStringConstant(function) + ", ptr " + gep(info.name, addr, 3) + "\n";
      return addr;
    }

    // conditional(expr) on a function: false means the function and every call to it vanish.
    bool conditionHolds(FunctionDecl* f)
    {
      if (!f->condition)
        return true;
      ParsedFile* saved = currentFile;
      currentFile = f->file;
      std::optional<int64_t> v = evalConst(f->condition);
      currentFile = saved;
      if (!v)
        unsupported(f, "a conditional(...) whose condition is not a compile-time constant");
      return *v != 0;
    }

    // LLVM function attributes for [[noinline]] and [[forceinline]].
    static std::string attributeText(Decl* d)
    {
      std::string s;
      if (d->attributes & ATTR_NoInline)
        s += "noinline ";
      if (d->attributes & ATTR_ForceInline)
        s += "alwaysinline ";
      return s;
    }

    // A finished function definition: into the single body, or into the current module.
    void addDefinition(const std::string& name, const std::string& text)
    {
      if (moduleCount <= 1)
      {
        body += text;
        return;
      }
      bodies[currentModule] += text;
      definedIn[name] = currentModule;
      declareLines[name] = declareFromHeader(text);
    }

    // "declare RET NAME(TYPES)" from a "define RET NAME(TYPE %name, ...) attrs {" header.
    static std::string declareFromHeader(const std::string& text)
    {
      size_t at = text.find(" @");
      size_t nameEnd = text[at + 2] == '"' ? text.find('"', at + 3) + 1 : text.find('(', at);
      size_t open = text.find('(', nameEnd);
      size_t close = text.find(") ", open);
      if (close == std::string::npos || text.find('{', open) < close)
        close = text.find(")", open);
      std::string params;
      size_t p = open + 1;
      while (p < close)
      {
        size_t comma = text.find(", ", p);
        if (comma == std::string::npos || comma > close)
          comma = close;
        std::string one = text.substr(p, comma - p);
        size_t pct = one.rfind(" %");
        if (pct != std::string::npos)
          one = one.substr(0, pct);
        params += (params.empty() ? "" : ", ") + one;
        p = comma + 2;
      }
      return "declare " + text.substr(7, nameEnd - 7) + "(" + params + ")";
    }

    // Module k: the shared declarations, then declares for what it uses from other modules, then its bodies.
    std::string moduleText(unsigned k, const std::string& prefix)
    {
      std::string text = moduleDecls[k] + bodies[k];
      std::string declares;
      std::unordered_set<std::string> seen;
      for (size_t i = text.find('@'); i != std::string::npos; i = text.find('@', i + 1))
      {
        size_t end;
        if (i + 1 < text.size() && text[i + 1] == '"')
          end = text.find('"', i + 2) + 1;
        else
        {
          end = i + 1;
          while (end < text.size() && (isalnum((unsigned char)text[end]) || strchr("-$._", text[end])))
            ++end;
        }
        std::string name = text.substr(i, end - i);
        if (!seen.insert(name).second)
          continue;
        auto it = definedIn.find(name);
        if (it != definedIn.end() && it->second != k)
          declares += declareLines[name] + "\n";
      }
      return prefix + decls + moduleDecls[k] + "\n" + shared + declares + "\n" + bodies[k];
    }

    // The default assert handler, unless the program defines `extern "C" kov_assert_failed`.
    void finishAssertRuntime()
    {
      if (!usesAssert || declaredNames["@kov_assert_failed"])
        return;
      emittingShared = true;
      std::string format = cStringConstant("%s:%u:%u: assertion failed: %s\n");
      if (!declaredNames["@fprintf"])
      {
        decls += "declare i32 @fprintf(ptr, ptr, ...)\n";
        declaredNames["@fprintf"] = true;
      }
      if (!declaredNames["@__acrt_iob_func"])
      {
        decls += "declare ptr @__acrt_iob_func(i32)\n";
        declaredNames["@__acrt_iob_func"] = true;
      }
      if (!declaredNames["@abort"])
      {
        decls += "declare void @abort()\n";
        declaredNames["@abort"] = true;
      }
      shared += "$kov_assert_failed = comdat any\n"
                "define linkonce_odr void @kov_assert_failed(ptr %file, i32 %line, i32 %column, ptr %expression) noinline comdat {\nentry:\n"
              "  %err = call ptr @__acrt_iob_func(i32 2)\n"
              "  %0 = call i32 (ptr, ptr, ...) @fprintf(ptr %err, ptr " + format + ", ptr %file, i32 %line, i32 %column, ptr %expression)\n"
              "  call void @abort()\n  unreachable\n}\n";
      declaredNames["@kov_assert_failed"] = true;
      emittingShared = false;
    }

    // --- values ---

    Value convert(Value v, const Ty* to, Node* at)
    {
      const Ty* from = v.ty;
      to = types.stripRef(to);
      if (!to || !to->isKnown())
        unsupported(at, "a conversion to an unknown type");
      if (from == to || types.unqualified(from) == types.unqualified(to))
        return {v.text, to, v.isTemporary};
      auto kindOf = [&](const Ty* t) { return t->kind == TyKind::Enum ? TyKind::Int : t->kind; };
      TyKind fk = kindOf(from), tk = kindOf(to);
      auto bitsOf = [&](const Ty* t) -> uint8_t { return t->kind == TyKind::Enum ? uint8_t(enumUnderlying(t->decl)->bits) : t->bits; };
      auto unsignedOf = [&](const Ty* t) { return t->kind == TyKind::Enum ? enumUnderlying(t->decl)->isUnsigned : t->isUnsigned; };
      if (fk == TyKind::Class || tk == TyKind::Class)
      {
        if (fk == TyKind::Class && tk == TyKind::Class)
        {
          std::string addr;
          if (baseAddress(from, to, v.text, addr, at))
            return {addr, to, v.isTemporary && addr == v.text};
        }
        unsupported(at, "a conversion from " + types.toString(from, &symbols) + " to " + types.toString(to, &symbols));
      }
      std::string r = newTemp();
      std::string fromT = llType(from, at), toT = llType(to, at);
      if ((fk == TyKind::Int || fk == TyKind::Char) && (tk == TyKind::Int || tk == TyKind::Char))
      {
        uint8_t fb = bitsOf(from), tb = bitsOf(to);
        if (fb == tb)
          return {v.text, to};
        fn += "  " + r + " = " + (fb > tb ? "trunc" : unsignedOf(from) ? "zext" : "sext") + " " + fromT + " " + v.text + " to " + toT + "\n";
        return {r, to};
      }
      if (fk == TyKind::Bool && (tk == TyKind::Int || tk == TyKind::Char))
      {
        fn += "  " + r + " = zext i1 " + v.text + " to " + toT + "\n";
        return {r, to};
      }
      if ((fk == TyKind::Int || fk == TyKind::Char) && tk == TyKind::Bool)
      {
        fn += "  " + r + " = icmp ne " + fromT + " " + v.text + ", 0\n";
        return {r, to};
      }
      if ((fk == TyKind::Pointer || fk == TyKind::Nullptr) && tk == TyKind::Bool)
      {
        fn += "  " + r + " = icmp ne ptr " + v.text + ", null\n";
        return {r, to};
      }
      if (fk == TyKind::Float && tk == TyKind::Bool)
      {
        fn += "  " + r + " = fcmp une " + fromT + " " + v.text + ", 0.0\n";
        return {r, to};
      }
      if (fk == TyKind::Float && (tk == TyKind::Int || tk == TyKind::Char))
      {
        fn += "  " + r + " = " + (unsignedOf(to) ? "fptoui" : "fptosi") + " " + fromT + " " + v.text + " to " + toT + "\n";
        return {r, to};
      }
      if ((fk == TyKind::Int || fk == TyKind::Char || fk == TyKind::Bool) && tk == TyKind::Float)
      {
        bool u = fk == TyKind::Bool || unsignedOf(from);
        fn += "  " + r + " = " + (u ? "uitofp" : "sitofp") + " " + fromT + " " + v.text + " to " + toT + "\n";
        return {r, to};
      }
      if (fk == TyKind::Float && tk == TyKind::Float)
      {
        fn += "  " + r + " = " + (from->bits > to->bits ? "fptrunc" : "fpext") + " " + fromT + " " + v.text + " to " + toT + "\n";
        return {r, to};
      }
      if (tk == TyKind::Pointer && (fk == TyKind::Pointer || fk == TyKind::Nullptr || fk == TyKind::Function || fk == TyKind::Array))
      {
        if (fk == TyKind::Pointer && isClass(from->inner) && isClass(to->inner) && classKey(from->inner) != classKey(to->inner))
        {
          std::string addr;
          if (baseAddress(from->inner, to->inner, v.text, addr, at))
            return {addr, to};
        }
        return {fk == TyKind::Nullptr ? "null" : v.text, to};
      }
      if (tk == TyKind::Pointer && fk == TyKind::Int)
      {
        fn += "  " + r + " = inttoptr " + fromT + " " + v.text + " to ptr\n";
        return {r, to};
      }
      if (fk == TyKind::Pointer && tk == TyKind::Int)
      {
        fn += "  " + r + " = ptrtoint ptr " + v.text + " to " + toT + "\n";
        return {r, to};
      }
      unsupported(at, "a conversion from " + types.toString(from, &symbols) + " to " + types.toString(to, &symbols));
    }

    // Address of the `base` subobject inside the `derived` object at addr (false if not a base).
    bool baseAddress(const Ty* derived, const Ty* base, const std::string& addr, std::string& out, Node* at)
    {
      if (canonicalClass(derived, at) == canonicalClass(base, at))
      {
        out = addr;
        return true;
      }
      StructInfo& info = structFor(derived, at);
      std::string name = info.name;
      std::vector<std::pair<Decl*, uint32_t>> bases = info.bases;
      std::vector<const Ty*> fieldTypes = info.fieldTypes;
      for (auto& [baseDef, index] : bases)
      {
        std::string sub = gep(name, addr, index);
        if (baseAddress(fieldTypes[index], base, sub, out, at))
          return true;
      }
      return false;
    }

    const Ty* commonArithmetic(const Ty* a, const Ty* b)
    {
      auto promote = [&](const Ty* t) -> const Ty*
      {
        t = types.unqualified(types.stripRef(t));
        if (t->kind == TyKind::Enum)
          return enumUnderlying(t->decl);
        if (t->kind == TyKind::Bool || t->kind == TyKind::Char || (t->kind == TyKind::Int && t->bits < 32))
          return types.intType(32);
        return t;
      };
      a = promote(a);
      b = promote(b);
      if (a->kind == TyKind::Float || b->kind == TyKind::Float)
        return types.floatType(std::max<uint8_t>(a->kind == TyKind::Float ? a->bits : 0, b->kind == TyKind::Float ? b->bits : 0));
      if (a->kind != TyKind::Int || b->kind != TyKind::Int)
        return a->kind == TyKind::Int ? a : b;
      if (a->bits != b->bits)
        return a->bits > b->bits ? a : b;
      return types.intType(a->bits, a->isUnsigned || b->isUnsigned);
    }

    Value load(const std::string& addr, const Ty* ty, Node* at)
    {
      ty = types.stripRef(ty);
      if (ty->kind == TyKind::Array)
      {
        std::string r = newTemp();
        fn += "  " + r + " = getelementptr inbounds " + llType(ty, at) + ", ptr " + addr + ", i64 0, i64 0\n";
        return {r, types.pointer(ty->inner)};
      }
      if (isAggregate(ty))
        return {addr, types.unqualified(ty)};
      std::string r = newTemp();
      fn += "  " + r + " = load " + llType(ty, at) + ", ptr " + addr + "\n";
      return {r, types.unqualified(ty)};
    }

    void store(Value v, const std::string& addr, const Ty* ty, Node* at)
    {
      ty = types.stripRef(ty);
      if (isClass(ty))
        unsupported(at, "storing a class value directly");
      Value c = convert(v, ty, at);
      fn += "  store " + llType(ty, at) + " " + c.text + ", ptr " + addr + "\n";
    }

    std::string thisPointer(Node* at)
    {
      auto it = locals.find(nullptr);
      if (it == locals.end())
        unsupported(at, "a member used outside a method");
      std::string r = newTemp();
      fn += "  " + r + " = load ptr, ptr " + it->second.addr + "\n";
      return r;
    }

    std::string operatorName(P op)
    {
      return op == P::LBracket ? "operator[]" : op == P::LParen ? "operator()" : "operator" + std::string(punctuatorSpelling(op));
    }

    std::string lvalue(Expr* e)
    {
      switch (e->kind)
      {
        case NodeKind::NameExpr:
        {
          auto* n = static_cast<NameExpr*>(e);
          Decl* d = n->resolved;
          if (!d)
            unsupported(e, "an unresolved name (" + sym(n->name.last().symbol) + ")");
          auto it = locals.find(d);
          if (it != locals.end())
          {
            if (it->second.isRef)
            {
              std::string r = newTemp();
              fn += "  " + r + " = load ptr, ptr " + it->second.addr + "\n";
              return r;
            }
            return it->second.addr;
          }
          if (d->kind == NodeKind::VariableDecl)
          {
            if (Decl* cls = global.owningClass(d); cls && !(d->flags & DF_Static))
            {
              if (!currentClassTy)
                unsupported(e, "a member used outside a method");
              std::string addr;
              if (!fieldAddress(currentClassTy, d, thisPointer(e), addr, e))
                unsupported(e, "a member that is not a field");
              return addr;
            }
            return globalFor(static_cast<VariableDecl*>(d), e);
          }
          unsupported(e, "taking the address of " + sym(n->name.last().symbol));
        }
        case NodeKind::MemberExpr:
        {
          auto* m = static_cast<MemberExpr*>(e);
          Decl* d = m->resolved;
          if (!d || d->kind != NodeKind::VariableDecl)
            unsupported(e, "a member access that did not resolve to a field");
          std::string object;
          const Ty* baseTy;
          if (m->op == P::Arrow)
          {
            Value p = rvalue(m->base);
            if (p.ty->kind != TyKind::Pointer)
              unsupported(e, "-> on something that is not a pointer");
            object = p.text;
            baseTy = p.ty->inner;
          }
          else
          {
            Value o = rvalue(m->base);
            if (!isClass(o.ty))
              unsupported(e, "a member access on a non-class object");
            object = o.text;
            baseTy = o.ty;
          }
          baseTy = types.stripRef(baseTy);
          if (!baseTy || !isClass(baseTy))
            unsupported(e, "a member access on a non-class object");
          if (d->flags & DF_Static)
            return globalFor(static_cast<VariableDecl*>(d), e);
          std::string addr;
          if (!fieldAddress(baseTy, d, object, addr, e))
            unsupported(e, "a member that is not a field of the object's class");
          return addr;
        }
        case NodeKind::UnaryExpr:
        {
          auto* u = static_cast<UnaryExpr*>(e);
          if (!u->isKeyword && P(u->op) == P::Star)
          {
            Value p = rvalue(u->operand);
            if (isClass(p.ty))
            {
              Value v = operatorCall(p, operatorName(P::Star), {}, e, true);
              return v.text;
            }
            if (p.ty->kind != TyKind::Pointer)
              unsupported(e, "* on something that is not a pointer");
            return p.text;
          }
          if (!u->isKeyword && !u->postfix && (P(u->op) == P::PlusPlus || P(u->op) == P::MinusMinus))
          {
            rvalue(e);
            return lvalue(u->operand);
          }
          break;
        }
        case NodeKind::IndexExpr:
        {
          auto* i = static_cast<IndexExpr*>(e);
          if (i->args.size() != 1)
            unsupported(e, "multi-argument indexing");
          Value base = rvalue(i->base);
          if (isClass(base.ty))
          {
            Value v = operatorCall(base, operatorName(P::LBracket), i->args, e, true);
            return v.text;
          }
          Value index = convert(rvalue(i->args[0]), types.intType(64), e);
          if (base.ty->kind != TyKind::Pointer)
            unsupported(e, "indexing a non-pointer");
          std::string r = newTemp();
          fn += "  " + r + " = getelementptr inbounds " + llType(base.ty->inner, e) + ", ptr " + base.text + ", i64 " + index.text + "\n";
          return r;
        }
        case NodeKind::ParenExpr: return lvalue(static_cast<ParenExpr*>(e)->inner);
        case NodeKind::BinaryExpr:
        {
          auto* b = static_cast<BinaryExpr*>(e);
          if (!b->isKeyword && isAssignment(P(b->op)))
          {
            Value v = rvalue(e);
            if (isClass(v.ty))
              return v.text;
            return lvalue(b->lhs);
          }
          if (!b->isKeyword && P(b->op) == P::Comma)
          {
            rvalue(b->lhs);
            return lvalue(b->rhs);
          }
          break;
        }
        case NodeKind::CallExpr:
        {
          Value v = callValue(static_cast<CallExpr*>(e), "");
          if (isClass(v.ty) || (v.ty && v.ty->isReference()))
            return v.text;
          std::string slot = newObjectSlot(v.ty, e);
          fn += "  store " + llType(v.ty, e) + " " + v.text + ", ptr " + slot + "\n";
          return slot;
        }
        default:
          break;
      }
      Value v = rvalue(e);
      if (isClass(v.ty))
        return v.text;
      std::string slot = newObjectSlot(v.ty, e);
      fn += "  store " + llType(v.ty, e) + " " + v.text + ", ptr " + slot + "\n";
      return slot;
    }

    std::string globalFor(VariableDecl* v, Node* at)
    {
      auto it = globals.find(v);
      if (it != globals.end())
        return it->second;
      struct SharedScope
      {
        bool& flag;
        bool saved;
        SharedScope(bool& f) : flag(f), saved(f) { flag = true; }
        ~SharedScope() { flag = saved; }
      } sharedScope(emittingShared); // a global's initializer belongs to every module
      const Ty* t = global.typeOfDecl(v);
      if (!t || !t->isKnown())
        unsupported(at, "a global of unknown type");
      std::string name = "@\"" + global.qualifiedName(v) + "\"";
      globals[v] = name;
      if (v->builtin == BUILTIN_EnumValues && v->parent)
      {
        // E::values: a constant array of the enumerators in declaration order.
        auto* e = static_cast<EnumDecl*>(v->parent);
        std::string element = llType(enumUnderlying(e), at), elements;
        for (Decl* d : e->enumerators)
          if (d->kind == NodeKind::EnumeratorDecl)
            elements += std::string(elements.empty() ? "" : ", ") + element + " " + std::to_string(enumeratorValue(static_cast<EnumeratorDecl*>(d)));
        decls += name + " = internal constant " + llType(t, at) + " [" + elements + "]\n";
        return name;
      }
      std::string init = "zeroinitializer";
      const Ty* st = types.stripRef(t);
      ParsedFile* saved = currentFile;
      currentFile = v->file;
      if (v->init)
      {
        auto c = evalConst(v->init);
        if (c && (st->kind == TyKind::Int || st->kind == TyKind::Char || st->kind == TyKind::Enum))
          init = std::to_string(*c);
        else if (c && st->kind == TyKind::Bool)
          init = *c ? "true" : "false";
        else if (v->init->kind == NodeKind::LiteralExpr && static_cast<LiteralExpr*>(v->init)->literalKind == TokenKind::String &&
                 st->kind == TyKind::Pointer)
          init = stringConstant(static_cast<LiteralExpr*>(v->init));
        else
        {
          currentFile = saved;
          unsupported(at, "a global with a non-constant initializer");
        }
      }
      else if (isClass(st) && hasUserConstructors(classDef(st->decl, at)))
      {
        currentFile = saved;
        unsupported(at, "a global object with a constructor");
      }
      currentFile = saved;
      // Defined in every module, merged by the linker (one instance per program).
      decls += "$" + name.substr(1) + " = comdat any\n" + name + " = linkonce_odr " + (t->isConst ? "constant " : "global ") + llType(st, at) +
               " " + init + ", comdat\n";
      return name;
    }

    static bool isAssignment(P op)
    {
      switch (op)
      {
        case P::Assign: case P::PlusAssign: case P::MinusAssign: case P::StarAssign: case P::SlashAssign: case P::PercentAssign:
        case P::AmpAssign: case P::PipeAssign: case P::CaretAssign: case P::ShlAssign: case P::ShrAssign:
          return true;
        default:
          return false;
      }
    }

    static P baseOp(P op)
    {
      switch (op)
      {
        case P::PlusAssign: return P::Plus;
        case P::MinusAssign: return P::Minus;
        case P::StarAssign: return P::Star;
        case P::SlashAssign: return P::Slash;
        case P::PercentAssign: return P::Percent;
        case P::AmpAssign: return P::Amp;
        case P::PipeAssign: return P::Pipe;
        case P::CaretAssign: return P::Caret;
        case P::ShlAssign: return P::Shl;
        case P::ShrAssign: return P::Shr;
        default: return op;
      }
    }

    // Finds the best member with the given name (searching bases, nearest class wins) for the
    // argument types and the object's constness. Returns the class type it was found in.
    FunctionDecl* findMemberFunction(const Ty* classTy, const std::string& name, const std::vector<const Ty*>& argTypes,
                                     const Ty* objectTy, Node* at, const Ty** foundIn)
    {
      FunctionDecl* found = nullptr;
      std::function<void(const Ty*)> search = [&](const Ty* cls)
      {
        ClassDecl* c = classDef(cls->decl, at);
        std::vector<FunctionDecl*> candidates;
        for (Decl* m : c->members)
        {
          if (m->kind != NodeKind::FunctionDecl || !m->templateParams.empty())
            continue;
          auto* f = static_cast<FunctionDecl*>(m);
          if (global.qualifiedName(f).ends_with("::" + name))
            candidates.push_back(f);
        }
        if (!candidates.empty())
        {
          const Ty* obj = objectTy ? objectTy : cls;
          found = global.chooseOverload(candidates, argTypes, global.classArgsOf(cls), obj);
          if (found)
          {
            if (foundIn)
              *foundIn = cls;
            return;
          }
        }
        StructInfo& info = structFor(cls, at);
        std::vector<const Ty*> baseTypes;
        for (auto& [baseDef, index] : info.bases)
          baseTypes.push_back(info.fieldTypes[index]);
        for (const Ty* b : baseTypes)
          if (!found)
            search(b);
      };
      search(canonicalClass(classTy, at));
      return found;
    }

    FunctionDecl* findOperator(const Ty* classTy, const std::string& name, size_t argCount, Node* at, const Ty** foundIn)
    {
      std::vector<const Ty*> neutral(argCount, nullptr);
      return findMemberFunction(classTy, name, neutral, nullptr, at, foundIn);
    }

    // Calls an operator member on an object. The result of a reference-returning operator is
    // the referenced address (ty keeps the reference).
    Value operatorCall(Value object, const std::string& name, Span<Expr*> args, Node* at, bool required)
    {
      const Ty* foundIn = nullptr;
      FunctionDecl* f = findMemberFunction(object.ty, name, argTypesOf(args), object.ty, at, &foundIn);
      if (!f)
      {
        if (required)
          unsupported(at, global.qualifiedName(object.ty->decl) + " has no " + name);
        return {"", nullptr};
      }
      std::string thisArg = object.text;
      if (canonicalClass(object.ty, at) != foundIn)
      {
        std::string addr;
        if (baseAddress(object.ty, foundIn, object.text, addr, at))
          thisArg = addr;
      }
      return callFunction(f, global.classArgsOf(foundIn), foundIn, thisArg, "", args, at);
    }

    Value arithmetic(P op, Value l, Value r, Node* at)
    {
      bool lp = l.ty->kind == TyKind::Pointer, rp = r.ty->kind == TyKind::Pointer;
      std::string res = newTemp();
      if (op == P::Plus || op == P::Minus)
      {
        if (lp && rp && op == P::Minus)
        {
          std::string a = newTemp(), b = newTemp(), d = newTemp();
          fn += "  " + a + " = ptrtoint ptr " + l.text + " to i64\n";
          fn += "  " + b + " = ptrtoint ptr " + r.text + " to i64\n";
          fn += "  " + d + " = sub i64 " + a + ", " + b + "\n";
          fn += "  " + res + " = sdiv exact i64 " + d + ", " + std::to_string(std::max<uint64_t>(1, sizeOf(l.ty->inner, at))) + "\n";
          return {res, types.ptrdiffType()};
        }
        if (lp || rp)
        {
          if (rp)
            std::swap(l, r);
          Value idx = convert(r, types.intType(64), at);
          std::string i = idx.text;
          if (op == P::Minus)
          {
            std::string neg = newTemp();
            fn += "  " + neg + " = sub i64 0, " + i + "\n";
            i = neg;
          }
          fn += "  " + res + " = getelementptr inbounds " + llType(l.ty->inner, at) + ", ptr " + l.text + ", i64 " + i + "\n";
          return {res, l.ty};
        }
      }
      const Ty* ct = commonArithmetic(l.ty, r.ty);
      l = convert(l, ct, at);
      r = convert(r, ct, at);
      bool isFloat = ct->kind == TyKind::Float;
      bool isUnsigned = ct->isUnsigned;
      const char* ins = nullptr;
      switch (op)
      {
        case P::Plus: ins = isFloat ? "fadd" : "add"; break;
        case P::Minus: ins = isFloat ? "fsub" : "sub"; break;
        case P::Star: ins = isFloat ? "fmul" : "mul"; break;
        case P::Slash: ins = isFloat ? "fdiv" : isUnsigned ? "udiv" : "sdiv"; break;
        case P::Percent: ins = isFloat ? "frem" : isUnsigned ? "urem" : "srem"; break;
        case P::Amp: ins = "and"; break;
        case P::Pipe: ins = "or"; break;
        case P::Caret: ins = "xor"; break;
        case P::Shl: ins = "shl"; break;
        case P::Shr: ins = isUnsigned ? "lshr" : "ashr"; break;
        default: unsupported(at, "this operator");
      }
      fn += "  " + res + " = " + ins + " " + llType(ct, at) + " " + l.text + ", " + r.text + "\n";
      return {res, ct};
    }

    Value compare(P op, Value l, Value r, Node* at)
    {
      std::string res = newTemp();
      if (l.ty->kind == TyKind::Pointer || r.ty->kind == TyKind::Pointer || l.ty->kind == TyKind::Nullptr || r.ty->kind == TyKind::Nullptr)
      {
        const Ty* pt = l.ty->kind == TyKind::Pointer ? l.ty : r.ty->kind == TyKind::Pointer ? r.ty : types.pointer(types.voidType());
        l = convert(l, pt, at);
        r = convert(r, pt, at);
        const char* pred = op == P::EqEq ? "eq" : op == P::NotEq ? "ne" : op == P::Less ? "ult" : op == P::LessEq ? "ule" : op == P::Greater ? "ugt" : "uge";
        fn += "  " + res + " = icmp " + pred + " ptr " + l.text + ", " + r.text + "\n";
        return {res, types.boolType()};
      }
      const Ty* ct = (l.ty->kind == TyKind::Bool && r.ty->kind == TyKind::Bool) ? types.boolType() : commonArithmetic(l.ty, r.ty);
      l = convert(l, ct, at);
      r = convert(r, ct, at);
      if (ct->kind == TyKind::Float)
      {
        const char* pred = op == P::EqEq ? "oeq" : op == P::NotEq ? "une" : op == P::Less ? "olt" : op == P::LessEq ? "ole" : op == P::Greater ? "ogt" : "oge";
        fn += "  " + res + " = fcmp " + pred + " " + llType(ct, at) + " " + l.text + ", " + r.text + "\n";
      }
      else
      {
        bool u = ct->isUnsigned || ct->kind == TyKind::Bool;
        const char* pred = op == P::EqEq ? "eq" : op == P::NotEq ? "ne" : op == P::Less ? (u ? "ult" : "slt") : op == P::LessEq ? (u ? "ule" : "sle")
                         : op == P::Greater ? (u ? "ugt" : "sgt") : (u ? "uge" : "sge");
        fn += "  " + res + " = icmp " + pred + " " + llType(ct, at) + " " + l.text + ", " + r.text + "\n";
      }
      return {res, types.boolType()};
    }

    void emitBranch(const std::string& target)
    {
      if (!blockTerminated)
        fn += "  br label %" + target + "\n";
      blockTerminated = true;
    }

    void startBlock(const std::string& name)
    {
      fn += name + ":\n";
      blockTerminated = false;
      currentBlock = name;
    }

    // Call expression. sret: where to construct a class result (empty: a temporary).
    Value callValue(CallExpr* c, const std::string& sret)
    {
      Expr* callee = c->callee;
      if (callee->kind == NodeKind::NameExpr)
      {
        auto* n = static_cast<NameExpr*>(callee);
        Decl* d = n->resolved;
        if (d && d->kind == NodeKind::FunctionDecl)
        {
          auto* f = static_cast<FunctionDecl*>(d);
          std::string thisArg;
          const Ty* classTy = nullptr;
          if (Decl* owner = global.owningClass(f); owner && !(f->flags & DF_Static))
          {
            if (!currentClassTy)
              unsupported(c, "a method call outside a method");
            thisArg = thisPointer(c);
            classTy = isClass(currentClassTy) ? currentClassTy : nullptr;
            if (owner->kind == NodeKind::ClassDecl && classTy && classDef(owner, c) != classDef(currentClassTy->decl, c))
            {
              // Inherited method: find the base subobject of the right class.
              const Ty* base = baseInstantiation(currentClassTy, owner, c);
              std::string addr;
              if (base && baseAddress(currentClassTy, base, thisArg, addr, c))
              {
                thisArg = addr;
                classTy = base;
              }
            }
          }
          std::string indirect;
          if (classTy && n->name.parts.size() == 1 && !thisArg.empty())
            if (int slot = slotOf(classTy, f, c); slot >= 0)
              indirect = loadSlot(thisArg, slot);
          return callFunction(f, calleeArgs(f, classTy, c, c), classTy, thisArg, sret, c->args, c, indirect);
        }
        if (!d || d->kind == NodeKind::ClassDecl || d->kind == NodeKind::EnumDecl || d->kind == NodeKind::UsingAliasDecl ||
            d->kind == NodeKind::TemplateParamDecl)
        {
          const Ty* t = c->ty;
          if (!t || !t->isKnown())
            unsupported(c, "a call that did not resolve");
          if (isClass(t))
          {
            std::string addr = sret.empty() ? newObjectSlot(t, c) : sret;
            constructInto(addr, t, c->args, c);
            if (sret.empty())
              statementTemps.push_back({addr, t});
            return {addr, t, sret.empty()};
          }
          if (c->args.size() == 1)
            return convert(rvalue(c->args[0]), t, c);
          if (c->args.empty())
            return {t->kind == TyKind::Float ? "0.0" : t->kind == TyKind::Pointer ? "null" : "0", t};
          unsupported(c, "this functional cast");
        }
      }
      else if (callee->kind == NodeKind::MemberExpr)
      {
        auto* m = static_cast<MemberExpr*>(callee);
        if (!m->member.empty() && m->member.last().special == 1)
        {
          // Explicit destructor call.
          std::string addr;
          const Ty* objectTy;
          if (m->op == P::Arrow)
          {
            Value p = rvalue(m->base);
            if (p.ty->kind != TyKind::Pointer)
              unsupported(c, "-> on something that is not a pointer");
            addr = p.text;
            objectTy = p.ty->inner;
          }
          else
          {
            Value o = rvalue(m->base);
            addr = o.text;
            objectTy = o.ty;
          }
          if (isClass(objectTy))
            destroy(addr, objectTy, c);
          return {"", types.voidType()};
        }
        if (m->resolved && m->resolved->kind == NodeKind::FunctionDecl)
        {
          auto* f = static_cast<FunctionDecl*>(m->resolved);
          std::string thisArg;
          const Ty* objectTy = nullptr;
          if (!(f->flags & DF_Static))
          {
            if (m->op == P::Arrow)
            {
              Value p = rvalue(m->base);
              if (p.ty->kind != TyKind::Pointer)
                unsupported(c, "-> on something that is not a pointer");
              thisArg = p.text;
              objectTy = p.ty->inner;
            }
            else
            {
              Value o = rvalue(m->base);
              if (isClass(o.ty))
              {
                thisArg = o.text;
                objectTy = o.ty;
              }
              else if (o.ty && types.stripRef(o.ty)->kind == TyKind::Enum)
              {
                // A method of an enum: `this` is the value, passed by its address.
                const Ty* et = types.stripRef(o.ty);
                std::string slot = "%v" + std::to_string(temp++);
                allocas += "  " + slot + " = alloca " + llType(et, c) + "\n";
                fn += "  store " + llType(et, c) + " " + o.text + ", ptr " + slot + "\n";
                thisArg = slot;
                objectTy = et;
              }
              else
                unsupported(c, "a method call on a non-class object");
            }
            if (isClass(objectTy))
            {
              objectTy = canonicalClass(objectTy, c);
              Decl* owner = global.owningClass(f);
              if (owner && classDef(owner, c) != static_cast<ClassDecl*>(objectTy->decl))
              {
                const Ty* base = baseInstantiation(objectTy, owner, c);
                std::string addr;
                if (base && baseAddress(objectTy, base, thisArg, addr, c))
                {
                  thisArg = addr;
                  objectTy = base;
                }
              }
            }
            else
              objectTy = nullptr; // an enum: no class arguments, no bases
          }
          else if (m->base)
          {
            const Ty* bt = types.stripRef(m->base->ty);
            if (bt && bt->kind == TyKind::Pointer)
              bt = bt->inner;
            if (isClass(bt))
              objectTy = canonicalClass(bt, c);
          }
          std::string indirect;
          if (objectTy && !thisArg.empty() && m->member.parts.size() == 1)
            if (int slot = slotOf(objectTy, f, c); slot >= 0)
              indirect = loadSlot(thisArg, slot);
          return callFunction(f, calleeArgs(f, objectTy, c, c), objectTy, thisArg, sret, c->args, c, indirect);
        }
      }
      // Call through a closure or a function pointer.
      Value fnValue = rvalue(callee);
      if (isLambda(fnValue.ty))
        return callClosure(fnValue, c->args, sret, c);
      const Ty* ft = fnValue.ty->kind == TyKind::Pointer ? fnValue.ty->inner : fnValue.ty;
      if (!ft || ft->kind != TyKind::Function)
        unsupported(c, "a call that did not resolve to a function");
      std::string args;
      for (size_t i = 0; i < c->args.size(); ++i)
      {
        Value a = rvalue(c->args[i]);
        if (i < ft->args.size())
          a = convert(a, ft->args[i], c);
        args += (i ? ", " : "") + llType(a.ty, c) + " " + a.text;
      }
      std::string sig = llType(ft->inner, c) + " (";
      for (size_t i = 0; i < ft->args.size(); ++i)
        sig += (i ? ", " : "") + llType(ft->args[i], c);
      sig += std::string(ft->variadic ? (ft->args.empty() ? "..." : ", ...") : "") + ")";
      if (ft->inner->kind == TyKind::Void)
      {
        fn += "  call " + sig + " " + fnValue.text + "(" + args + ")\n";
        return {"", types.voidType()};
      }
      std::string r = newTemp();
      fn += "  " + r + " = call " + sig + " " + fnValue.text + "(" + args + ")\n";
      return {r, types.stripRef(ft->inner)};
    }

    // The instantiated base type of `derived` whose definition is `baseDecl`, or null.
    const Ty* baseInstantiation(const Ty* derived, Decl* baseDecl, Node* at)
    {
      ClassDecl* target = classDef(baseDecl, at);
      StructInfo& info = structFor(derived, at);
      std::vector<const Ty*> candidates;
      for (auto& [baseDef, index] : info.bases)
        candidates.push_back(info.fieldTypes[index]);
      for (const Ty* b : candidates)
      {
        if (static_cast<ClassDecl*>(canonicalClass(b, at)->decl) == target)
          return canonicalClass(b, at);
        if (const Ty* deeper = baseInstantiation(b, baseDecl, at))
          return deeper;
      }
      return nullptr;
    }

    // Value of a call result for use as an rvalue (reference results are loaded).
    Value callRvalue(Value v, Node* at)
    {
      if (v.ty && v.ty->isReference())
      {
        if (isClass(types.stripRef(v.ty)))
          return {v.text, types.unqualified(types.stripRef(v.ty)), false};
        return load(v.text, v.ty, at);
      }
      return v;
    }

    Value rvalue(Expr* e)
    {
      switch (e->kind)
      {
        case NodeKind::LiteralExpr:
        {
          auto* l = static_cast<LiteralExpr*>(e);
          const Ty* t = e->ty;
          if (l->literalKind == TokenKind::String)
            return {stringConstant(l), t};
          if (l->literalKind == TokenKind::Keyword)
            return {l->keyword == Kw::nullptr_ ? "null" : l->keyword == Kw::true_ ? "true" : "false", t};
          if (l->literalKind == TokenKind::Char)
          {
            std::string s = decodeString(spelling(l->token));
            return {std::to_string(s.empty() ? 0 : (unsigned char)s[0]), t};
          }
          if (t->kind == TyKind::Float)
          {
            std::string digits;
            for (char c : spelling(l->token))
              if (c != '\'' && c != 'f' && c != 'F' && c != 'l' && c != 'L')
                digits += c;
            double d = strtod(digits.c_str(), nullptr);
            // LLVM wants the exact value: the bits of the double (rounded through float first for f32).
            double exact = t->bits == 32 ? double(float(d)) : d;
            uint64_t bits;
            memcpy(&bits, &exact, sizeof bits);
            char buf[32];
            snprintf(buf, sizeof buf, "0x%016llX", (unsigned long long)bits);
            return {buf, t};
          }
          auto v = evalConst(e);
          if (!v)
            unsupported(e, "this literal");
          return {std::to_string(*v), t};
        }
        case NodeKind::NameExpr:
        {
          auto* n = static_cast<NameExpr*>(e);
          Decl* d = n->resolved;
          if (d && d->kind == NodeKind::EnumeratorDecl)
            return {std::to_string(enumeratorValue(static_cast<EnumeratorDecl*>(d))), e->ty};
          if (d && d->kind == NodeKind::FunctionDecl)
          {
            auto* f = static_cast<FunctionDecl*>(d);
            if (!f->templateParams.empty() || global.isTemplateContext(f))
              unsupported(e, "taking the address of a function template");
            return {useFunction(f, {}, nullptr), types.pointer(declType(d))};
          }
          if (d && d->kind == NodeKind::VariableDecl && !locals.count(d))
          {
            const Ty* t = types.stripRef(declType(d));
            if (t && t->isConst && (t->kind == TyKind::Int || t->kind == TyKind::Enum || t->kind == TyKind::Char || t->kind == TyKind::Bool))
              if (auto c = evalConst(e))
                return {t->kind == TyKind::Bool ? (*c ? "true" : "false") : std::to_string(*c), types.unqualified(t)};
          }
          std::string addr = lvalue(e);
          return load(addr, e->ty ? e->ty : (d ? declType(d) : nullptr), e);
        }
        case NodeKind::ThisExpr:
        {
          std::string p = thisPointer(e);
          const Ty* t = types.stripRef(e->ty);
          if (t && t->kind == TyKind::Enum)
          {
            std::string r = newTemp(); // in an enum's methods `this` is the value
            fn += "  " + r + " = load " + llType(t, e) + ", ptr " + p + "\n";
            return {r, t};
          }
          return {p, e->ty};
        }
        case NodeKind::ParenExpr: return rvalue(static_cast<ParenExpr*>(e)->inner);
        case NodeKind::MemberExpr:
        {
          auto* m = static_cast<MemberExpr*>(e);
          if (m->resolved && m->resolved->kind == NodeKind::EnumeratorDecl)
            return {std::to_string(enumeratorValue(static_cast<EnumeratorDecl*>(m->resolved))), e->ty};
          std::string addr = lvalue(e);
          return load(addr, e->ty, e);
        }
        case NodeKind::IndexExpr:
        {
          auto* i = static_cast<IndexExpr*>(e);
          Value base = rvalue(i->base);
          if (isClass(base.ty))
            return callRvalue(operatorCall(base, operatorName(P::LBracket), i->args, e, true), e);
          Value index = convert(rvalue(i->args[0]), types.intType(64), e);
          if (base.ty->kind != TyKind::Pointer)
            unsupported(e, "indexing a non-pointer");
          std::string r = newTemp();
          fn += "  " + r + " = getelementptr inbounds " + llType(base.ty->inner, e) + ", ptr " + base.text + ", i64 " + index.text + "\n";
          return load(r, base.ty->inner, e);
        }
        case NodeKind::CallExpr: return callRvalue(callValue(static_cast<CallExpr*>(e), ""), e);
        case NodeKind::NameofExpr:
        {
          Expr* o = static_cast<NameofExpr*>(e)->operand;
          std::string text;
          if (o->kind == NodeKind::NameExpr)
            text = sym(static_cast<NameExpr*>(o)->name.last().symbol);
          else if (o->kind == NodeKind::MemberExpr)
            text = sym(static_cast<MemberExpr*>(o)->member.last().symbol);
          else
            text = std::string(spelling(o->token));
          return {cStringConstant(text), e->ty};
        }
        case NodeKind::CastExpr:
          return convert(rvalue(static_cast<CastExpr*>(e)->operand), e->ty, e);
        case NodeKind::NamedCastExpr:
          return convert(rvalue(static_cast<NamedCastExpr*>(e)->operand), e->ty, e);
        case NodeKind::FunctionalCastExpr:
        {
          auto* c = static_cast<FunctionalCastExpr*>(e);
          const Ty* t = types.stripRef(e->ty);
          if (isClass(t))
          {
            std::string addr = newObjectSlot(t, e);
            constructInto(addr, t, c->args, e);
            statementTemps.push_back({addr, t});
            return {addr, t, true};
          }
          if (c->args.size() == 1)
          {
            Expr* a = c->args[0];
            if (a->kind == NodeKind::BraceInitExpr && static_cast<BraceInitExpr*>(a)->args.size() == 1)
              a = static_cast<BraceInitExpr*>(a)->args[0];
            return convert(rvalue(a), t, e);
          }
          if (c->args.empty())
            return {t->kind == TyKind::Float ? "0.0" : t->kind == TyKind::Pointer ? "null" : "0", t};
          unsupported(e, "this functional cast");
        }
        case NodeKind::SizeofExpr:
        {
          auto* s = static_cast<SizeofExpr*>(e);
          if (s->isPack)
          {
            Decl* param = nullptr;
            if (s->type && s->type->kind == NodeKind::NamedType)
              param = static_cast<NamedType*>(s->type)->resolved;
            if (!param && s->operand)
              param = packIn(s->operand);
            if (!param || !packLocals.count(param))
              unsupported(e, "sizeof... of something that is not a parameter pack");
            return {std::to_string(packLocals[param].size()), types.sizeType()};
          }
          auto v = evalConst(e);
          if (!v)
            unsupported(e, "this sizeof");
          return {std::to_string(*v), types.sizeType()};
        }
        case NodeKind::FoldExpr:
        {
          auto* f = static_cast<FoldExpr*>(e);
          Expr* pattern = f->lhs ? f->lhs : f->rhs;
          if (!pattern)
            unsupported(e, "an empty fold");
          P op = P(f->op);
          if (op == P::Comma)
          {
            forEachPackElement(pattern, e, [&](size_t)
            {
              rvalue(pattern);
              destroyTemps();
            });
            return {"", types.voidType()};
          }
          std::optional<Value> acc;
          forEachPackElement(pattern, e, [&](size_t)
          {
            Value v = rvalue(pattern);
            if (!acc)
              acc = v;
            else if (op == P::EqEq || op == P::NotEq || op == P::Less || op == P::LessEq || op == P::Greater || op == P::GreaterEq)
              acc = compare(op, *acc, v, e);
            else
              acc = arithmetic(op, *acc, v, e);
          });
          if (!acc)
            unsupported(e, "a fold over an empty pack");
          return *acc;
        }
        case NodeKind::UnaryExpr: return unary(static_cast<UnaryExpr*>(e));
        case NodeKind::BinaryExpr: return binary(static_cast<BinaryExpr*>(e));
        case NodeKind::ConditionalExpr:
        {
          auto* c = static_cast<ConditionalExpr*>(e);
          if (!c->then)
            unsupported(e, "the ?: elvis operator");
          const Ty* t = types.stripRef(e->ty);
          if (isClass(t))
            unsupported(e, "a conditional expression of class type");
          Value cond = convert(rvalue(c->cond), types.boolType(), e);
          std::string tL = newLabel("cond.then"), eL = newLabel("cond.else"), jL = newLabel("cond.end");
          fn += "  br i1 " + cond.text + ", label %" + tL + ", label %" + eL + "\n";
          startBlock(tL);
          Value a = t->kind == TyKind::Void ? rvalue(c->then) : convert(rvalue(c->then), t, e);
          std::string aEnd = currentBlock;
          emitBranch(jL);
          startBlock(eL);
          Value b = t->kind == TyKind::Void ? rvalue(c->else_) : convert(rvalue(c->else_), t, e);
          std::string bEnd = currentBlock;
          emitBranch(jL);
          startBlock(jL);
          if (t->kind == TyKind::Void)
            return {"", t};
          std::string r = newTemp();
          fn += "  " + r + " = phi " + llType(t, e) + " [ " + a.text + ", %" + aEnd + " ], [ " + b.text + ", %" + bEnd + " ]\n";
          return {r, t};
        }
        case NodeKind::NewExpr:
        {
          auto* n = static_cast<NewExpr*>(e);
          const Ty* t = e->ty && e->ty->kind == TyKind::Pointer ? e->ty->inner : nullptr;
          if (!t)
            unsupported(e, "new of an unknown type");
          std::string r;
          if (n->placement.size() == 1)
            r = convert(rvalue(n->placement[0]), types.pointer(types.voidType()), e).text;
          else if (!n->placement.empty())
            unsupported(e, "placement new with several arguments");
          else
          {
            ensureMalloc();
            r = newTemp();
            fn += "  " + r + " = call ptr @malloc(i64 " + std::to_string(sizeOf(t, e)) + ")\n";
          }
          if (isClass(t))
            constructInto(r, t, n->args, e);
          else if (!n->args.empty())
          {
            Expr* a = n->args[0];
            if (a->kind == NodeKind::BraceInitExpr && static_cast<BraceInitExpr*>(a)->args.size() == 1)
              a = static_cast<BraceInitExpr*>(a)->args[0];
            store(rvalue(a), r, t, e);
          }
          else
            fn += "  store " + llType(t, e) + " zeroinitializer, ptr " + r + "\n";
          return {r, e->ty};
        }
        case NodeKind::LambdaExpr: return makeClosure(static_cast<LambdaExpr*>(e));
        case NodeKind::DeleteExpr:
        {
          auto* d = static_cast<DeleteExpr*>(e);
          Value p = rvalue(d->operand);
          if (p.ty->kind != TyKind::Pointer)
            unsupported(e, "delete of a non-pointer");
          if (isClass(p.ty->inner))
            destroy(p.text, p.ty->inner, e, true);
          ensureMalloc();
          fn += "  call void @free(ptr " + p.text + ")\n";
          return {"", types.voidType()};
        }
        default:
          unsupported(e, std::string("the expression kind ") + nodeKindName(e->kind));
      }
    }

    void ensureMalloc()
    {
      if (mallocDeclared)
        return;
      mallocDeclared = true;
      if (!declaredNames["@malloc"])
        decls += "declare ptr @malloc(i64)\n";
      if (!declaredNames["@free"])
        decls += "declare void @free(ptr)\n";
      declaredNames["@malloc"] = declaredNames["@free"] = true;
    }

    Value unary(UnaryExpr* u)
    {
      if (u->isKeyword)
      {
        if (Kw(u->op) == Kw::not_)
        {
          Value v = convert(rvalue(u->operand), types.boolType(), u);
          std::string r = newTemp();
          fn += "  " + r + " = xor i1 " + v.text + ", true\n";
          return {r, types.boolType()};
        }
        unsupported(u, "this keyword operator");
      }
      P op = P(u->op);
      if (op == P::Amp)
        return {lvalue(u->operand), u->ty};
      if (op == P::Star)
      {
        Value p = rvalue(u->operand);
        if (isClass(p.ty))
          return callRvalue(operatorCall(p, operatorName(P::Star), {}, u, true), u);
        if (p.ty->kind != TyKind::Pointer)
          unsupported(u, "* on something that is not a pointer");
        return load(p.text, p.ty->inner, u);
      }
      if (op == P::PlusPlus || op == P::MinusMinus)
      {
        const Ty* ot = types.stripRef(u->operand->ty);
        if (isClass(ot))
        {
          Value o = rvalue(u->operand);
          return callRvalue(operatorCall(o, operatorName(op), {}, u, true), u);
        }
        std::string addr = lvalue(u->operand);
        Value old = load(addr, ot, u);
        Value updated = arithmetic(op == P::PlusPlus ? P::Plus : P::Minus, old, Value{"1", types.intType(32)}, u);
        store(updated, addr, ot, u);
        return u->postfix ? old : convert(updated, ot, u);
      }
      Value v = rvalue(u->operand);
      if (isClass(v.ty))
        return callRvalue(operatorCall(v, operatorName(op), {}, u, true), u);
      switch (op)
      {
        case P::Bang:
        {
          Value b = convert(v, types.boolType(), u);
          std::string r = newTemp();
          fn += "  " + r + " = xor i1 " + b.text + ", true\n";
          return {r, types.boolType()};
        }
        case P::Minus:
        {
          Value c = convert(v, u->ty, u);
          std::string r = newTemp();
          if (c.ty->kind == TyKind::Float)
            fn += "  " + r + " = fneg " + llType(c.ty, u) + " " + c.text + "\n";
          else
            fn += "  " + r + " = sub " + llType(c.ty, u) + " 0, " + c.text + "\n";
          return {r, c.ty};
        }
        case P::Plus: return convert(v, u->ty, u);
        case P::Tilde:
        {
          Value c = convert(v, u->ty, u);
          std::string r = newTemp();
          fn += "  " + r + " = xor " + llType(c.ty, u) + " " + c.text + ", -1\n";
          return {r, c.ty};
        }
        default:
          unsupported(u, "this unary operator");
      }
    }

    Value binary(BinaryExpr* b)
    {
      if (b->isKeyword)
      {
        Kw k = Kw(b->op);
        if (k == Kw::and_ || k == Kw::or_)
          return logical(k == Kw::and_, b->lhs, b->rhs, b);
        unsupported(b, "this keyword operator");
      }
      P op = P(b->op);
      if (op == P::AndAnd || op == P::OrOr)
        return logical(op == P::AndAnd, b->lhs, b->rhs, b);
      if (op == P::Comma)
      {
        rvalue(b->lhs);
        return rvalue(b->rhs);
      }
      const Ty* lt = types.stripRef(b->lhs->ty);
      if (isClass(lt))
      {
        Value l = rvalue(b->lhs);
        std::string name = operatorName(op);
        Expr* rhsExpr = b->rhs;
        Span<Expr*> one{&rhsExpr, 1};
        const Ty* foundIn = nullptr;
        if (findMemberFunction(l.ty, name, argTypesOf(one), l.ty, b, &foundIn))
          return callRvalue(operatorCall(l, name, one, b, true), b);
        if (op == P::Assign)
        {
          Value r = rvalue(b->rhs);
          if (!isClass(r.ty))
            unsupported(b, "assigning a non-class to a class");
          std::string src = r.text;
          if (classKey(r.ty) != classKey(l.ty))
            if (!baseAddress(r.ty, l.ty, r.text, src, b))
              unsupported(b, "assigning an unrelated class");
          assignObject(l.text, src, l.ty, b);
          return l;
        }
        unsupported(b, global.qualifiedName(l.ty->decl) + " has no " + name);
      }
      if (isAssignment(op))
      {
        std::string addr = lvalue(b->lhs);
        const Ty* t = types.stripRef(b->lhs->ty);
        if (!t || !t->isKnown())
          unsupported(b, "an assignment to an expression of unknown type");
        Value rhs = rvalue(b->rhs);
        if (op != P::Assign)
        {
          Value old = load(addr, t, b);
          rhs = arithmetic(baseOp(op), old, rhs, b);
        }
        store(rhs, addr, t, b);
        return convert(rhs, t, b);
      }
      Value l = rvalue(b->lhs);
      Value r = rvalue(b->rhs);
      switch (op)
      {
        case P::EqEq: case P::NotEq: case P::Less: case P::LessEq: case P::Greater: case P::GreaterEq:
          return compare(op, l, r, b);
        default:
          return arithmetic(op, l, r, b);
      }
    }

    Value logical(bool isAnd, Expr* lhs, Expr* rhs, Node* at)
    {
      Value l = convert(rvalue(lhs), types.boolType(), at);
      std::string rL = newLabel(isAnd ? "and.rhs" : "or.rhs"), eL = newLabel(isAnd ? "and.end" : "or.end");
      std::string lEnd = currentBlock;
      if (isAnd)
        fn += "  br i1 " + l.text + ", label %" + rL + ", label %" + eL + "\n";
      else
        fn += "  br i1 " + l.text + ", label %" + eL + ", label %" + rL + "\n";
      startBlock(rL);
      Value r = convert(rvalue(rhs), types.boolType(), at);
      std::string rEnd = currentBlock;
      emitBranch(eL);
      startBlock(eL);
      std::string res = newTemp();
      fn += "  " + res + " = phi i1 [ " + std::string(isAnd ? "false" : "true") + ", %" + lEnd + " ], [ " + r.text + ", %" + rEnd + " ]\n";
      return {res, types.boolType()};
    }

    // --- statements and scopes ---

    void destroyTemps()
    {
      for (size_t i = statementTemps.size(); i-- > 0;)
        destroy(statementTemps[i].addr, statementTemps[i].ty, nullptr);
      statementTemps.clear();
    }

    void pushScope() { scopes.emplace_back(); }

    void popScope()
    {
      if (!blockTerminated)
        for (size_t i = scopes.back().size(); i-- > 0;)
          destroy(scopes.back()[i].addr, scopes.back()[i].ty, nullptr);
      scopes.pop_back();
    }

    // Destroys the objects of the innermost scopes down to (not including) depth.
    void unwindTo(size_t depth)
    {
      for (size_t s = scopes.size(); s-- > depth;)
        for (size_t i = scopes[s].size(); i-- > 0;)
          destroy(scopes[s][i].addr, scopes[s][i].ty, nullptr);
    }

    void declareLocal(VariableDecl* v)
    {
      const Ty* t = declType(v);
      if (!t || !t->isKnown() || types.isDependent(t))
        unsupported(v, "a local of unknown type (" + sym(v->name.last().symbol) + ")");
      bool isRef = t->isReference();
      std::string slot = "%v" + std::to_string(temp++);
      allocas += "  " + slot + " = alloca " + (isRef ? std::string("ptr") : llType(t, v)) + "\n";
      locals[v] = {slot, t, isRef};
      if (isRef)
      {
        if (!v->init)
          unsupported(v, "a reference without an initializer");
        std::string addr;
        if (isClass(types.stripRef(t)))
        {
          Value o = rvalue(v->init);
          addr = o.text;
          if (o.isTemporary)
          {
            for (size_t i = statementTemps.size(); i-- > 0;)
              if (statementTemps[i].addr == addr)
                statementTemps.erase(statementTemps.begin() + i);
            scopes.back().push_back({addr, types.stripRef(t)});
          }
        }
        else
          addr = lvalue(v->init);
        fn += "  store ptr " + addr + ", ptr " + slot + "\n";
        return;
      }
      if (v->init)
        initializeFrom(slot, t, v->init, v);
      else if (isClass(t))
        defaultConstruct(slot, t, v);
      else if (t->kind == TyKind::Array && isClass(t->inner) && hasUserConstructors(classDef(t->inner->decl, v)))
        unsupported(v, "arrays of objects with constructors");
      else
        fn += "  store " + llType(t, v) + " zeroinitializer, ptr " + slot + "\n";
      if (needsDestruction(t, v))
        scopes.back().push_back({slot, t});
    }

    void statement(Stmt* s)
    {
      if (!s)
        return;
      if (blockTerminated)
        startBlock(newLabel("dead"));
      switch (s->kind)
      {
        case NodeKind::BlockStmt:
          pushScope();
          for (Stmt* inner : static_cast<BlockStmt*>(s)->stmts)
            statement(inner);
          popScope();
          break;
        case NodeKind::ExprStmt:
          if (static_cast<ExprStmt*>(s)->expr)
            rvalue(static_cast<ExprStmt*>(s)->expr);
          destroyTemps();
          break;
        case NodeKind::DeclStmt:
          for (Decl* d : static_cast<DeclStmt*>(s)->decls)
          {
            if (d->kind == NodeKind::VariableDecl)
            {
              declareLocal(static_cast<VariableDecl*>(d));
              destroyTemps();
            }
            else if (d->kind == NodeKind::ClassDecl || d->kind == NodeKind::EnumDecl || d->kind == NodeKind::UsingAliasDecl ||
                     d->kind == NodeKind::EmptyDecl || d->kind == NodeKind::StaticAssertDecl)
              continue;
            else
              unsupported(d, "this local declaration");
          }
          break;
        case NodeKind::EmptyStmt:
          break;
        case NodeKind::ReturnStmt:
        {
          auto* r = static_cast<ReturnStmt*>(s);
          std::string retText;
          if (currentReturn->kind == TyKind::Void)
          {
            if (r->value)
              rvalue(r->value);
          }
          else if (isClass(currentReturn))
          {
            if (!r->value)
              unsupported(s, "return without a value");
            initializeFrom(sretAddr, currentReturn, r->value, s);
          }
          else if (currentReturn->isReference())
            retText = isClass(types.stripRef(currentReturn)) ? rvalue(r->value).text : lvalue(r->value);
          else
          {
            if (!r->value)
              unsupported(s, "return without a value in a non-void function");
            retText = convert(rvalue(r->value), currentReturn, s).text;
          }
          destroyTemps();
          unwindTo(0);
          if (currentReturn->kind == TyKind::Void || isClass(currentReturn))
            fn += "  ret void\n";
          else
            fn += "  ret " + std::string(currentReturn->isReference() ? "ptr" : llType(currentReturn, s)) + " " + retText + "\n";
          blockTerminated = true;
          break;
        }
        case NodeKind::IfStmt:
        {
          auto* i = static_cast<IfStmt*>(s);
          if (i->isConstexpr && !i->init && i->cond && i->cond->kind != NodeKind::VariableDecl && i->cond->kind != NodeKind::DeclStmt)
            if (std::optional<int64_t> known = evalConst(static_cast<Expr*>(i->cond)))
            {
              // if constexpr with a known condition: only the taken branch exists.
              statement(*known ? i->then : i->else_);
              break;
            }
          pushScope();
          if (i->init)
            statement(i->init);
          Value cond = condition(i->cond, s);
          destroyTemps();
          std::string tL = newLabel("if.then"), eL = newLabel("if.else"), jL = newLabel("if.end");
          fn += "  br i1 " + cond.text + ", label %" + tL + ", label %" + (i->else_ ? eL : jL) + "\n";
          startBlock(tL);
          statement(i->then);
          emitBranch(jL);
          if (i->else_)
          {
            startBlock(eL);
            statement(i->else_);
            emitBranch(jL);
          }
          startBlock(jL);
          popScope();
          break;
        }
        case NodeKind::AssertStmt:
        {
          auto* a = static_cast<AssertStmt*>(s);
          if (!a->release && !debug)
            break; // assert compiles to nothing outside debug builds
          Value cond = condition(a->cond, s);
          destroyTemps();
          std::string okL = newLabel("assert.ok"), failL = newLabel("assert.fail");
          fn += "  br i1 " + cond.text + ", label %" + okL + ", label %" + failL + "\n";
          startBlock(failL);
          auto [line, column] = lineColumnOf(a->token);
          fn += "  call void @kov_assert_failed(ptr " + cStringConstant(currentFile->path) + ", i32 " + std::to_string(line) + ", i32 " +
                std::to_string(column) + ", ptr " + cStringConstant(sourceText(a->startToken, a->endToken)) + ")\n  unreachable\n";
          usesAssert = true;
          startBlock(okL);
          break;
        }
        case NodeKind::WhileStmt:
        {
          auto* w = static_cast<WhileStmt*>(s);
          std::string cL = newLabel("while.cond"), bL = newLabel("while.body"), eL = newLabel("while.end");
          emitBranch(cL);
          startBlock(cL);
          Value cond = condition(w->cond, s);
          destroyTemps();
          fn += "  br i1 " + cond.text + ", label %" + bL + ", label %" + eL + "\n";
          startBlock(bL);
          loops.push_back({eL, cL, scopes.size()});
          statement(w->body);
          loops.pop_back();
          emitBranch(cL);
          startBlock(eL);
          break;
        }
        case NodeKind::DoStmt:
        {
          auto* d = static_cast<DoStmt*>(s);
          std::string bL = newLabel("do.body"), cL = newLabel("do.cond"), eL = newLabel("do.end");
          emitBranch(bL);
          startBlock(bL);
          loops.push_back({eL, cL, scopes.size()});
          statement(d->body);
          loops.pop_back();
          emitBranch(cL);
          startBlock(cL);
          Value cond = convert(rvalue(d->cond), types.boolType(), s);
          destroyTemps();
          fn += "  br i1 " + cond.text + ", label %" + bL + ", label %" + eL + "\n";
          startBlock(eL);
          break;
        }
        case NodeKind::ForStmt:
        {
          auto* f = static_cast<ForStmt*>(s);
          pushScope();
          if (f->init)
            statement(f->init);
          std::string cL = newLabel("for.cond"), bL = newLabel("for.body"), sL = newLabel("for.step"), eL = newLabel("for.end");
          emitBranch(cL);
          startBlock(cL);
          if (f->cond)
          {
            Value cond = condition(f->cond, s);
            destroyTemps();
            fn += "  br i1 " + cond.text + ", label %" + bL + ", label %" + eL + "\n";
          }
          else
            fn += "  br label %" + bL + "\n";
          startBlock(bL);
          loops.push_back({eL, sL, scopes.size()});
          statement(f->body);
          loops.pop_back();
          emitBranch(sL);
          startBlock(sL);
          if (f->step)
          {
            rvalue(f->step);
            destroyTemps();
          }
          emitBranch(cL);
          startBlock(eL);
          popScope();
          break;
        }
        case NodeKind::BreakStmt:
          if (loops.empty())
            unsupported(s, "break outside a loop or switch");
          unwindTo(loops.back().scopeDepth);
          emitBranch(loops.back().breakLabel);
          break;
        case NodeKind::ContinueStmt:
        {
          for (size_t i = loops.size(); i-- > 0;)
            if (!loops[i].continueLabel.empty())
            {
              unwindTo(loops[i].scopeDepth);
              emitBranch(loops[i].continueLabel);
              return;
            }
          unsupported(s, "continue outside a loop");
        }
        case NodeKind::SwitchStmt:
          switchStatement(static_cast<SwitchStmt*>(s));
          break;
        case NodeKind::RangeForStmt:
          rangeFor(static_cast<RangeForStmt*>(s));
          break;
        case NodeKind::CaseStmt:
        case NodeKind::DefaultStmt:
          unsupported(s, "a case label outside a switch body");
        case NodeKind::FallthroughStmt:
          break;
        default:
          unsupported(s, std::string("the statement kind ") + nodeKindName(s->kind));
      }
    }

    // Finds a zero-argument member function by name (searching bases), with the class it lives in.
    FunctionDecl* findMember(const Ty* classTy, const std::string& name, Node* at, const Ty** foundIn)
    {
      return findOperator(classTy, name, 0, at, foundIn);
    }

    void rangeFor(RangeForStmt* f)
    {
      pushScope();
      const Ty* rt = types.stripRef(f->range->ty);
      if (!rt || !rt->isKnown())
        unsupported(f, "a range of unknown type");
      std::string beginV, endV;
      const Ty* elementTy;
      if (rt->kind == TyKind::Array)
      {
        std::string base = lvalue(f->range);
        beginV = newTemp();
        fn += "  " + beginV + " = getelementptr inbounds " + llType(rt, f) + ", ptr " + base + ", i64 0, i64 0\n";
        endV = newTemp();
        fn += "  " + endV + " = getelementptr inbounds " + llType(rt, f) + ", ptr " + base + ", i64 0, i64 " + std::to_string(rt->count) + "\n";
        elementTy = rt->inner;
      }
      else if (isClass(rt))
      {
        Value range = rvalue(f->range);
        if (range.isTemporary)
        {
          for (size_t i = statementTemps.size(); i-- > 0;)
            if (statementTemps[i].addr == range.text)
              statementTemps.erase(statementTemps.begin() + i);
          scopes.back().push_back({range.text, range.ty});
        }
        const Ty* in = nullptr;
        FunctionDecl* begin = findMember(range.ty, "begin", f, &in);
        FunctionDecl* end = findMember(range.ty, "end", f, &in);
        if (!begin || !end)
          unsupported(f, "a range without begin()/end()");
        Value b = callFunction(begin, global.classArgsOf(in), in, range.text, "", {}, f);
        Value e = callFunction(end, global.classArgsOf(in), in, range.text, "", {}, f);
        if (!b.ty || b.ty->kind != TyKind::Pointer)
          unsupported(f, "a range whose iterators are not pointers");
        beginV = b.text;
        endV = e.text;
        elementTy = b.ty->inner;
      }
      else
        unsupported(f, "this kind of range");
      std::string itSlot = "%v" + std::to_string(temp++);
      allocas += "  " + itSlot + " = alloca ptr\n";
      fn += "  store ptr " + beginV + ", ptr " + itSlot + "\n";
      destroyTemps();
      std::string cL = newLabel("range.cond"), bL = newLabel("range.body"), sL = newLabel("range.step"), eL = newLabel("range.end");
      emitBranch(cL);
      startBlock(cL);
      std::string it = newTemp();
      fn += "  " + it + " = load ptr, ptr " + itSlot + "\n";
      std::string done = newTemp();
      fn += "  " + done + " = icmp eq ptr " + it + ", " + endV + "\n";
      fn += "  br i1 " + done + ", label %" + eL + ", label %" + bL + "\n";
      startBlock(bL);
      pushScope();
      // The loop variable: a reference binds to the element, a value copies it.
      auto* v = static_cast<VariableDecl*>(f->var);
      const Ty* vt = declType(v);
      if (!vt || !vt->isKnown() || types.isDependent(vt))
        unsupported(v, "a loop variable of unknown type");
      std::string element = newTemp();
      fn += "  " + element + " = load ptr, ptr " + itSlot + "\n";
      std::string slot = "%v" + std::to_string(temp++);
      if (vt->isReference())
      {
        allocas += "  " + slot + " = alloca ptr\n";
        fn += "  store ptr " + element + ", ptr " + slot + "\n";
        locals[v] = {slot, vt, true};
      }
      else
      {
        allocas += "  " + slot + " = alloca " + llType(vt, v) + "\n";
        if (isClass(vt))
          copyConstruct(slot, element, vt, v);
        else
          store(load(element, elementTy, v), slot, vt, v);
        locals[v] = {slot, vt, false};
        if (needsDestruction(vt, v))
          scopes.back().push_back({slot, vt});
      }
      loops.push_back({eL, sL, scopes.size()});
      statement(f->body);
      loops.pop_back();
      popScope();
      emitBranch(sL);
      startBlock(sL);
      std::string cur = newTemp();
      fn += "  " + cur + " = load ptr, ptr " + itSlot + "\n";
      std::string next = newTemp();
      fn += "  " + next + " = getelementptr inbounds " + llType(elementTy, f) + ", ptr " + cur + ", i64 1\n";
      fn += "  store ptr " + next + ", ptr " + itSlot + "\n";
      emitBranch(cL);
      startBlock(eL);
      popScope();
    }

    Value condition(Node* cond, Node* at)
    {
      if (!cond)
        unsupported(at, "a missing condition");
      if (cond->kind == NodeKind::DeclStmt)
      {
        auto* d = static_cast<DeclStmt*>(cond);
        if (d->decls.size() != 1 || d->decls[0]->kind != NodeKind::VariableDecl)
          unsupported(at, "this condition declaration");
        auto* v = static_cast<VariableDecl*>(d->decls[0]);
        declareLocal(v);
        Local& l = locals[v];
        return convert(load(l.addr, l.ty, at), types.boolType(), at);
      }
      return convert(rvalue(static_cast<Expr*>(cond)), types.boolType(), at);
    }

    void switchStatement(SwitchStmt* sw)
    {
      pushScope();
      if (sw->init)
        statement(sw->init);
      if (!sw->cond || sw->cond->kind == NodeKind::DeclStmt)
        unsupported(sw, "a switch on a declaration");
      Value cond = rvalue(static_cast<Expr*>(sw->cond));
      const Ty* ct = cond.ty->kind == TyKind::Enum ? enumUnderlying(cond.ty->decl) : cond.ty;
      if (ct->kind != TyKind::Int && ct->kind != TyKind::Char && ct->kind != TyKind::Bool)
        unsupported(sw, "a switch on a non-integer");
      cond = convert(cond, ct, sw);
      destroyTemps();
      if (!sw->body || sw->body->kind != NodeKind::BlockStmt)
        unsupported(sw, "a switch whose body is not a block");
      auto* body = static_cast<BlockStmt*>(sw->body);
      std::string endL = newLabel("switch.end");
      std::string defaultL;
      std::vector<std::string> labels;
      std::string cases;
      for (Stmt* st : body->stmts)
      {
        if (st->kind == NodeKind::CaseStmt)
        {
          auto* c = static_cast<CaseStmt*>(st);
          if (c->rangeEnd)
            unsupported(st, "case ranges");
          auto v = evalConst(c->value);
          if (!v)
            unsupported(st, "a case label that is not a constant");
          std::string l = newLabel("case");
          labels.push_back(l);
          cases += "    " + llType(ct, sw) + " " + std::to_string(*v) + ", label %" + l + "\n";
        }
        else if (st->kind == NodeKind::DefaultStmt)
        {
          defaultL = newLabel("default");
          labels.push_back(defaultL);
        }
      }
      std::string invalidL;
      const Ty* condTy = types.stripRef(static_cast<Expr*>(sw->cond)->ty); // before the conversion to the underlying type
      if (condTy && condTy->kind == TyKind::Enum && defaultL.empty())
      {
        Decl* def = global.definitionOf(condTy->decl);
        auto* e = def && def->kind == NodeKind::EnumDecl ? static_cast<EnumDecl*>(def) : nullptr;
        if (e && e->isKov)
        {
          // A Kov enum: every value needs a case, and a value outside them aborts.
          std::set<int64_t> covered;
          for (Stmt* st : body->stmts)
            if (st->kind == NodeKind::CaseStmt)
              if (auto v = evalConst(static_cast<CaseStmt*>(st)->value))
                covered.insert(*v);
          std::string missing;
          for (Decl* d : e->enumerators)
            if (d->kind == NodeKind::EnumeratorDecl && !covered.count(enumeratorValue(static_cast<EnumeratorDecl*>(d))))
              missing += (missing.empty() ? "" : ", ") + sym(static_cast<EnumeratorDecl*>(d)->name);
          if (!missing.empty())
            error(sw, "switch on " + global.qualifiedName(e) + " does not handle " + missing);
          invalidL = newLabel("switch.invalid");
        }
      }
      fn += "  switch " + llType(ct, sw) + " " + cond.text + ", label %" + (invalidL.empty() ? (defaultL.empty() ? endL : defaultL) : invalidL) + " [\n" + cases + "  ]\n";
      blockTerminated = true;
      if (!invalidL.empty())
      {
        startBlock(invalidL);
        auto [line, column] = lineColumnOf(sw->token);
        fn += "  call void @kov_assert_failed(ptr " + cStringConstant(currentFile->path) + ", i32 " + std::to_string(line) + ", i32 " +
              std::to_string(column) + ", ptr " + cStringConstant("invalid value in switch on " + global.qualifiedName(condTy->decl)) + ")\n  unreachable\n";
        usesAssert = true;
        blockTerminated = true;
      }
      loops.push_back({endL, "", scopes.size()});
      pushScope();
      size_t next = 0;
      for (Stmt* st : body->stmts)
      {
        if (st->kind == NodeKind::CaseStmt || st->kind == NodeKind::DefaultStmt)
        {
          const std::string& l = labels[next++];
          emitBranch(l);
          startBlock(l);
          continue;
        }
        if (st->kind == NodeKind::FallthroughStmt)
          continue;
        statement(st);
      }
      popScope();
      loops.pop_back();
      emitBranch(endL);
      startBlock(endL);
      popScope();
    }

    // --- function definitions ---

    void emitFunction(FunctionDecl* f, const TemplateArgs& args, const Ty* classTy)
    {
      if (!conditionHolds(f))
        return; // conditional(false): the function does not exist in this build
      currentFile = f->file;
      if (!args.empty())
        global.retypeFor(f, args, classTy);
      currentArgs = args.empty() ? nullptr : &args;
      currentClassTy = classTy;
      if (!currentClassTy)
        if (Decl* cls = global.owningClass(f))
          currentClassTy = cls->kind == NodeKind::EnumDecl ? types.enumType(cls) : canonicalClass(types.classType(cls, {}), f);
      const Ty* ft = declType(f);
      if (!ft || ft->kind != TyKind::Function)
      {
        error(f, "function type unknown, skipped");
        currentArgs = nullptr;
        return;
      }
      fn.clear();
      allocas.clear();
      locals.clear();
      loops.clear();
      scopes.clear();
      statementTemps.clear();
      temp = 0;
      label = 0;
      blockTerminated = false;
      currentBlock = "entry";
      currentFunction = f;
      currentReturn = ft->inner;
      sretAddr.clear();
      try
      {
        Signature s = signatureOf(f);
        std::string params;
        std::vector<std::string> paramNames;
        for (size_t i = 0; i < s.paramTypes.size(); ++i)
        {
          std::string pn = "%p" + std::to_string(i);
          paramNames.push_back(pn);
          params += (i ? ", " : "") + s.paramTypes[i] + " " + pn;
        }
        if (s.variadic)
          params += params.empty() ? "..." : ", ...";
        std::string name = functionName(f, args, classTy);
        declaredNames[name] = true;
        std::string header = "define " + s.returnType + " " + name + "(" + params + ") " + attributeText(f) + "{\nentry:\n";
        pushScope();
        size_t slot = 0;
        if (s.hasThis)
        {
          std::string thisSlot = "%v" + std::to_string(temp++);
          allocas += "  " + thisSlot + " = alloca ptr\n";
          fn += "  store ptr " + paramNames[slot] + ", ptr " + thisSlot + "\n";
          locals[nullptr] = {thisSlot, nullptr, false};
          ++slot;
        }
        if (s.hasSret)
          sretAddr = paramNames[slot++];
        packLocals.clear();
        for (size_t i = 0; i < f->params.size(); ++i)
        {
          auto* pd = static_cast<ParamDecl*>(f->params[i]);
          size_t count = 1;
          if (pd->isPack)
          {
            const Ty* packTy = declType(pd);
            if (!packTy || packTy->kind != TyKind::Pack)
              unsupported(f, "a parameter pack that was not deduced");
            count = packTy->args.size();
            packLocals[pd] = {};
          }
          for (size_t k = 0; k < count; ++k, ++slot)
          {
            const Ty* pt = s.paramTys[slot];
            Local local;
            if (isAggregate(pt))
            {
              local = {paramNames[slot], pt, false};
              if (needsDestruction(pt, f))
                scopes.back().push_back({paramNames[slot], pt});
            }
            else
            {
              bool isRef = pt->isReference();
              std::string ps = "%v" + std::to_string(temp++);
              allocas += "  " + ps + " = alloca " + (isRef ? std::string("ptr") : llType(pt, f)) + "\n";
              fn += "  store " + (isRef ? std::string("ptr") : llType(pt, f)) + " " + paramNames[slot] + ", ptr " + ps + "\n";
              local = {ps, pt, isRef};
            }
            if (pd->isPack)
              packLocals[pd].push_back(local);
            else
              locals[pd] = local;
          }
        }
        if (f->flags & DF_Constructor)
        {
          initializeMembers(thisPointer(f), currentClassTy, f->ctorInits, f);
          destroyTemps();
        }
        statement(f->body);
        if (!blockTerminated)
        {
          if (f->flags & DF_Destructor)
            destroyMembers(thisPointer(f), currentClassTy, f);
          popScope();
          if (currentReturn->kind == TyKind::Void || isClass(currentReturn))
            fn += "  ret void\n";
          else if (name == "@main")
            fn += "  ret i32 0\n";
          else
            fn += "  unreachable\n";
        }
        else if (f->flags & DF_Destructor)
        {
          error(f, "codegen does not support return statements in destructors yet");
          currentArgs = nullptr;
          return;
        }
        addDefinition(name, header + allocas + fn + "}\n\n");
        ++result.functions;
      }
      catch (Unsupported&)
      {
      }
      currentArgs = nullptr;
      currentClassTy = nullptr;
    }

    void collectFunctions(Span<Decl*> decls, std::vector<FunctionDecl*>& out)
    {
      for (Decl* d : decls)
      {
        switch (d->kind)
        {
          case NodeKind::NamespaceDecl: collectFunctions(static_cast<NamespaceDecl*>(d)->decls, out); break;
          case NodeKind::ClassDecl:
            if (static_cast<ClassDecl*>(d)->templateParams.empty())
              collectFunctions(static_cast<ClassDecl*>(d)->members, out);
            break;
          case NodeKind::EnumDecl: collectFunctions(static_cast<EnumDecl*>(d)->members, out); break;
          case NodeKind::FunctionDecl:
          {
            auto* f = static_cast<FunctionDecl*>(d);
            if (f->body && f->templateParams.empty() && !global.isTemplateContext(f) && !(f->file && f->file->isImported))
              out.push_back(f);
            break;
          }
          default: break;
        }
      }
    }
  };
}

CodegenResult generateLLVM(std::vector<std::unique_ptr<ParsedFile>>& files, GlobalScope& global, const SymbolTable& symbols, bool debug,
                           unsigned moduleCount)
{
  CodegenResult result;
  Emitter e(global, symbols, result);
  e.debug = debug;
  e.moduleCount = std::max(1u, moduleCount);
  e.bodies.resize(e.moduleCount);
  e.moduleDecls.resize(e.moduleCount);
  e.vtablesIn.resize(e.moduleCount);
  for (size_t i = 0; i < files.size(); ++i)
    e.fileModule[files[i].get()] = unsigned(i % e.moduleCount);
  std::vector<FunctionDecl*> functions;
  for (auto& file : files)
    e.collectFunctions(file->decls, functions);
  for (FunctionDecl* f : functions)
  {
    auto it = e.fileModule.find(f->file);
    e.currentModule = it == e.fileModule.end() ? 0 : it->second;
    e.emitFunction(f, {}, nullptr);
  }
  // Instantiations requested while emitting, including ones requested by other instantiations.
  while (!e.worklist.empty())
  {
    Emitter::Instantiation inst = e.worklist.front();
    e.worklist.pop_front();
    e.currentModule = inst.module;
    e.emitFunction(inst.f, inst.args, inst.classTy);
  }
  e.finishAssertRuntime();
  std::string prefix = "; Kov v0: generated by kovc\n"
                       "target datalayout = \"e-m:w-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128\"\n"
                       "target triple = \"x86_64-pc-windows-msvc\"\n\n";
  if (e.moduleCount <= 1)
  {
    result.ir = prefix + e.decls + e.moduleDecls[0] + "\n" + e.shared + e.body;
    return result;
  }
  for (unsigned k = 0; k < e.moduleCount; ++k)
    result.modules.push_back(e.moduleText(k, prefix));
  return result;
}
