#include "codegen.hpp"
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
  };

  struct Emitter
  {
    GlobalScope& global;
    TypeTable& types;
    const SymbolTable& symbols;
    CodegenResult& result;

    std::string decls;   // struct types, globals, string constants, declares
    std::string body;    // function definitions
    std::string fn;      // current function text (after the allocas)
    std::string allocas; // entry-block allocas of the current function
    uint32_t temp = 0;
    uint32_t label = 0;
    uint32_t stringCount = 0;
    bool blockTerminated = false;
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
    std::unordered_map<const Ty*, StructInfo> structs;
    std::unordered_map<Decl*, std::string> globals;
    std::unordered_map<std::string, bool> declaredNames; // IR names with a declare/define line
    std::unordered_map<Decl*, int64_t> enumeratorValues;

    struct Instantiation
    {
      FunctionDecl* f;
      TemplateArgs args;
      const Ty* classTy;
    };
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
        case TyKind::Lambda: case TyKind::MemberPointer:
          return "ptr";
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
        case TyKind::Lambda: case TyKind::MemberPointer:
          return 8;
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
      if (c->keyword == Kw::union_)
        unsupported(c, "unions");
      TemplateArgs args = global.classArgsOf(key);
      if (!c->templateParams.empty() && args.pairs.size() != c->templateParams.size())
        unsupported(at, "the class template " + global.qualifiedName(c) + " used without all its arguments");
      StructInfo& info = structs[key];
      info.name = "%struct." + std::to_string(structs.size());
      for (const ClassDecl::Base& b : c->bases)
      {
        const Ty* bt = global.typeOfAstIn(b.type, c, args);
        if (!bt || bt->kind != TyKind::Class || types.isDependent(bt))
          unsupported(c, "a base class that is not a known class");
        const Ty* baseKey = canonicalClass(bt, c);
        structFor(baseKey, c);
        info.bases.push_back({baseKey->decl, uint32_t(info.fieldTypes.size())});
        info.fieldTypes.push_back(baseKey);
      }
      for (Decl* m : c->members)
      {
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
      return structs[key];
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
        out = gep(info.name, objectAddr, it->second);
        return true;
      }
      for (auto& [baseDef, index] : info.bases)
      {
        std::string baseAddr = gep(info.name, objectAddr, index);
        if (fieldAddress(info.fieldTypes[index], member, baseAddr, out, at))
          return true;
      }
      return false;
    }

    // --- class metadata: constructors, destructors ---

    FunctionDecl* findConstructor(const Ty* classTy, size_t argCount, const Ty* firstArg, Node* at)
    {
      ClassDecl* c = classDef(classTy->decl, at);
      TemplateArgs args = global.classArgsOf(canonicalClass(classTy, at));
      ArgsScope scope(*this, &args);
      FunctionDecl* best = nullptr;
      for (Decl* m : c->members)
      {
        if (m->kind != NodeKind::FunctionDecl || !(m->flags & DF_Constructor) || !m->templateParams.empty())
          continue;
        auto* f = static_cast<FunctionDecl*>(m);
        size_t required = 0;
        for (Decl* p : f->params)
          if (!static_cast<ParamDecl*>(p)->defaultValue)
            ++required;
        if (argCount < required || argCount > f->params.size())
          continue;
        if (argCount == 1 && firstArg)
        {
          const Ty* pt = types.stripRef(declType(f->params[0]));
          const Ty* a = types.stripRef(firstArg);
          if (pt && a && types.unqualified(pt) == types.unqualified(a))
            return f;
          if (pt && a && isClass(pt) != isClass(a))
            continue;
          if (pt && a && isClass(pt) && isClass(a) && types.unqualified(pt) != types.unqualified(a))
            continue;
        }
        if (!best || f->params.size() == argCount)
          best = f;
      }
      return best;
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
    void destroy(const std::string& addr, const Ty* t, Node* at)
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
        FunctionDecl* ctor = findConstructor(t, 0, nullptr, at);
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
      for (Decl* d : info.fieldDecls)
      {
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
      FunctionDecl* ctor = findConstructor(t, args.size(), firstTy, at);
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
      if (needsDestruction(t, at))
        unsupported(at, "copying " + global.qualifiedName(c) + ", which has a destructor but no copy constructor");
      std::string v = newTemp();
      fn += "  " + v + " = load " + llType(t, at) + ", ptr " + src + "\n";
      fn += "  store " + llType(t, at) + " " + v + ", ptr " + dst + "\n";
    }

    // Initialises the object/scalar at addr of type t from an initializer expression,
    // constructing in place where the expression is a temporary.
    void initializeFrom(const std::string& addr, const Ty* t, Expr* init, Node* at)
    {
      t = types.stripRef(t);
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
        q = global.qualifiedName(f);
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
      return "@\"" + q + "(" + params + ")\"";
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
        s.paramTypes.push_back(isClass(pt) || pt->isReference() ? "ptr" : llType(pt, f));
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
      if (def->body && templated)
      {
        if (!requested[name])
        {
          requested[name] = true;
          worklist.push_back({def, args, classTy});
        }
        return name;
      }
      if (def->body)
        return name; // Defined by the main pass.
      if (declaredNames[name])
        return name;
      declaredNames[name] = true;
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
                       const std::string& sret, Span<Expr*> callArgs, Node* at)
    {
      std::string name = useFunction(f, args, classTy);
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
      for (size_t i = 0; i < callArgs.size(); ++i)
      {
        const Ty* pt = slot + i < s.paramTys.size() ? s.paramTys[slot + i] : nullptr;
        std::string text;
        if (pt && pt->isReference())
        {
          if (isClass(types.stripRef(pt)))
          {
            Value o = rvalue(callArgs[i]);
            if (!isClass(o.ty))
              unsupported(at, "binding a non-class to a class reference");
            std::string addr = o.text;
            if (classKey(o.ty) != classKey(pt))
              if (!baseAddress(o.ty, types.stripRef(pt), o.text, addr, at))
                unsupported(at, "binding an unrelated class to a reference");
            text = "ptr " + addr;
          }
          else
            text = "ptr " + lvalue(callArgs[i]);
        }
        else if (pt && isClass(pt))
        {
          Value v = rvalue(callArgs[i]);
          if (!isClass(v.ty))
            unsupported(at, "passing a non-class to a class parameter");
          std::string copy = newObjectSlot(pt, at);
          if (v.isTemporary)
            moveTemporary(copy, v, pt, at);
          else
            copyConstruct(copy, v.text, pt, at);
          text = "ptr " + copy;
        }
        else
        {
          Value a = rvalue(callArgs[i]);
          if (isClass(a.ty))
            unsupported(at, "passing a class where a scalar is expected");
          if (pt)
            a = convert(a, pt, at);
          else if (a.ty->kind == TyKind::Float && a.ty->bits == 32)
            a = convert(a, types.floatType(64), at);
          else if ((a.ty->kind == TyKind::Int || a.ty->kind == TyKind::Char) && a.ty->bits < 32)
            a = convert(a, types.intType(32), at);
          else if (a.ty->kind == TyKind::Bool)
            a = convert(a, types.intType(32), at);
          text = llType(a.ty, at) + " " + a.text;
        }
        argText += std::string(argText.empty() ? "" : ", ") + text;
      }
      std::string sig = s.variadic ? signatureText(s) + " " : s.returnType + " ";
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
      decls += name + " = private unnamed_addr constant [" + std::to_string(bytes.size()) + " x i8] c\"" + escaped + "\"\n";
      return name;
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
      if (isClass(ty))
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
      const Ty* t = global.typeOfDecl(v);
      if (!t || !t->isKnown())
        unsupported(at, "a global of unknown type");
      std::string name = "@\"" + global.qualifiedName(v) + "\"";
      globals[v] = name;
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
      decls += name + " = " + (t->isConst ? "constant " : "global ") + llType(st, at) + " " + init + "\n";
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

    // Finds an operator member (searching bases) by its qualified-name suffix. Returns the
    // class type it was found in through foundIn.
    FunctionDecl* findOperator(const Ty* classTy, const std::string& name, size_t argCount, Node* at, const Ty** foundIn)
    {
      FunctionDecl* found = nullptr;
      std::function<void(const Ty*)> search = [&](const Ty* cls)
      {
        ClassDecl* c = classDef(cls->decl, at);
        for (Decl* m : c->members)
        {
          if (found || m->kind != NodeKind::FunctionDecl)
            continue;
          auto* f = static_cast<FunctionDecl*>(m);
          if (f->params.size() != argCount || !f->templateParams.empty())
            continue;
          if (global.qualifiedName(f).ends_with("::" + name))
          {
            found = f;
            if (foundIn)
              *foundIn = cls;
          }
        }
        if (found)
          return;
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

    // Calls an operator member on an object. The result of a reference-returning operator is
    // the referenced address (ty keeps the reference).
    Value operatorCall(Value object, const std::string& name, Span<Expr*> args, Node* at, bool required)
    {
      const Ty* foundIn = nullptr;
      FunctionDecl* f = findOperator(object.ty, name, args.size(), at, &foundIn);
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
            classTy = currentClassTy;
            if (classDef(owner, c) != classDef(currentClassTy->decl, c))
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
          return callFunction(f, calleeArgs(f, classTy, c, c), classTy, thisArg, sret, c->args, c);
        }
        if (!d || d->kind == NodeKind::ClassDecl || d->kind == NodeKind::EnumDecl || d->kind == NodeKind::UsingAliasDecl)
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
              if (!isClass(o.ty))
                unsupported(c, "a method call on a non-class object");
              thisArg = o.text;
              objectTy = o.ty;
            }
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
          else if (m->base)
          {
            const Ty* bt = types.stripRef(m->base->ty);
            if (bt && bt->kind == TyKind::Pointer)
              bt = bt->inner;
            if (isClass(bt))
              objectTy = canonicalClass(bt, c);
          }
          return callFunction(f, calleeArgs(f, objectTy, c, c), objectTy, thisArg, sret, c->args, c);
        }
      }
      // Call through a function pointer.
      Value fnValue = rvalue(callee);
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
            char buf[64];
            if (t->bits == 32)
              snprintf(buf, sizeof buf, "%.9e", double(float(d)));
            else
              snprintf(buf, sizeof buf, "%.17e", d);
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
        case NodeKind::ThisExpr: return {thisPointer(e), e->ty};
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
          auto v = evalConst(e);
          if (!v)
            unsupported(e, "this sizeof");
          return {std::to_string(*v), types.sizeType()};
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
          ensureMalloc();
          std::string r = newTemp();
          fn += "  " + r + " = call ptr @malloc(i64 " + std::to_string(sizeOf(t, e)) + ")\n";
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
        case NodeKind::DeleteExpr:
        {
          auto* d = static_cast<DeleteExpr*>(e);
          Value p = rvalue(d->operand);
          if (p.ty->kind != TyKind::Pointer)
            unsupported(e, "delete of a non-pointer");
          if (isClass(p.ty->inner))
            destroy(p.text, p.ty->inner, e);
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
        if (findOperator(l.ty, name, 1, b, &foundIn))
          return callRvalue(operatorCall(l, name, one, b, true), b);
        if (op == P::Assign)
        {
          Value r = rvalue(b->rhs);
          if (!isClass(r.ty))
            unsupported(b, "assigning a non-class to a class");
          if (needsDestruction(l.ty, b))
            unsupported(b, "assignment to " + global.qualifiedName(l.ty->decl) + ", which has a destructor but no operator=");
          std::string v = newTemp();
          fn += "  " + v + " = load " + llType(l.ty, b) + ", ptr " + r.text + "\n";
          fn += "  store " + llType(l.ty, b) + " " + v + ", ptr " + l.text + "\n";
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
        case NodeKind::CaseStmt:
        case NodeKind::DefaultStmt:
          unsupported(s, "a case label outside a switch body");
        case NodeKind::FallthroughStmt:
          break;
        default:
          unsupported(s, std::string("the statement kind ") + nodeKindName(s->kind));
      }
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
      fn += "  switch " + llType(ct, sw) + " " + cond.text + ", label %" + (defaultL.empty() ? endL : defaultL) + " [\n" + cases + "  ]\n";
      blockTerminated = true;
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
      currentFile = f->file;
      if (!args.empty())
        global.retypeFor(f, args, classTy);
      currentArgs = args.empty() ? nullptr : &args;
      currentClassTy = classTy;
      if (!currentClassTy)
        if (Decl* cls = global.owningClass(f))
          currentClassTy = canonicalClass(types.classType(cls, {}), f);
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
        std::string header = "define " + s.returnType + " " + name + "(" + params + ") {\nentry:\n";
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
        for (size_t i = 0; i < f->params.size(); ++i, ++slot)
        {
          const Ty* pt = s.paramTys[slot];
          if (isClass(pt))
          {
            locals[f->params[i]] = {paramNames[slot], pt, false};
            if (needsDestruction(pt, f))
              scopes.back().push_back({paramNames[slot], pt});
            continue;
          }
          bool isRef = pt->isReference();
          std::string ps = "%v" + std::to_string(temp++);
          allocas += "  " + ps + " = alloca " + (isRef ? std::string("ptr") : llType(pt, f)) + "\n";
          fn += "  store " + (isRef ? std::string("ptr") : llType(pt, f)) + " " + paramNames[slot] + ", ptr " + ps + "\n";
          locals[f->params[i]] = {ps, pt, isRef};
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
        body += header + allocas + fn + "}\n\n";
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
          case NodeKind::FunctionDecl:
          {
            auto* f = static_cast<FunctionDecl*>(d);
            if (f->body && f->templateParams.empty() && !global.isTemplateContext(f))
              out.push_back(f);
            break;
          }
          default: break;
        }
      }
    }
  };
}

CodegenResult generateLLVM(std::vector<std::unique_ptr<ParsedFile>>& files, GlobalScope& global, const SymbolTable& symbols)
{
  CodegenResult result;
  Emitter e(global, symbols, result);
  std::vector<FunctionDecl*> functions;
  for (auto& file : files)
    e.collectFunctions(file->decls, functions);
  for (FunctionDecl* f : functions)
    e.emitFunction(f, {}, nullptr);
  // Instantiations requested while emitting, including ones requested by other instantiations.
  while (!e.worklist.empty())
  {
    Emitter::Instantiation inst = e.worklist.front();
    e.worklist.pop_front();
    e.emitFunction(inst.f, inst.args, inst.classTy);
  }
  result.ir = "; Kov v0: generated by kovc\n"
              "target datalayout = \"e-m:w-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128\"\n"
              "target triple = \"x86_64-pc-windows-msvc\"\n\n" +
              e.decls + "\n" + e.body;
  return result;
}
