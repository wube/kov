#include "codegen.hpp"
#include <map>
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
    std::string typeInfoName;                 // @"typeinfo.Class": { ptr name, ptr base descriptor }, what slot 0 points to
    const Ty* primaryBase = nullptr;          // the polymorphic first base, whose descriptor the chain continues with
    bool isUnion = false;                     // all members at offset 0, storage sized for the largest
    std::vector<std::pair<Decl*, uint32_t>> anonymous; // anonymous struct/union members -> field index
    std::unordered_map<Decl*, std::pair<uint32_t, uint32_t>> bitfields; // member -> (shift, width) inside its storage field
  };
  struct BitfieldInfo
  {
    uint32_t shift, width;
    const Ty* unit; // the storage unit's (integer) type
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
    std::vector<std::vector<std::string>> moduleCtors;           // per module: startup functions for its globals
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
    Decl* constContext = nullptr; // The declaration whose initializer evalConst is folding (a variable template's): the context of the traits in it.
    struct ConstContextScope // restores constContext when an Unsupported leaves the fold
    {
      Emitter& e;
      Decl* saved;
      ConstContextScope(Emitter& e, Decl* d) : e(e), saved(e.constContext) { e.constContext = d; }
      ~ConstContextScope() { e.constContext = saved; }
    };
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
    std::unordered_map<Decl*, BitfieldInfo> bitfieldOf; // bit-field members of every laid-out class
    bool dyncastEmitted = false; // the @kov.dyncast helper is in `decls`
    std::unordered_map<uint32_t, std::string> labelBlocks; // goto labels of the current function -> their blocks
    Span<uint32_t> pendingDesignators;                        // of the brace list constructInto is about to consume
    std::vector<std::pair<Decl*, int64_t>> constEnv;          // parameters bound while evalConst runs a constexpr call
    std::unordered_map<std::string, std::string> instanceGlobals; // static members of class template instantiations, by name
    std::unordered_map<std::string, bool> declaredNames; // IR names with a declare/define line
    std::unordered_map<Decl*, int64_t> enumeratorValues;

    struct Instantiation
    {
      FunctionDecl* f;
      TemplateArgs args;
      const Ty* classTy;
      unsigned module;         // the module of the function that asked for it
      std::string requestedBy; // the name of that function, for diagnostics
      std::string name;        // the LLVM name to define it under (the declaration's), or empty
    };
    std::string currentFunctionName; // LLVM name of the function being emitted
    unsigned staticLocals = 0;       // numbers the function-local statics' globals
    std::string currentRequestedBy;  // for an instantiation: the function that first used it

    // A lambda's closure: one struct per lambda, fields for the captures, one function.
    struct ClosureInfo
    {
      std::string structName;
      std::vector<Decl*> captured;       // enclosing local per field (null for `this`)
      std::vector<bool> byRef;
      std::vector<Expr*> inits;          // init-captures: the initializer (null for other captures)
      std::vector<const Ty*> fieldTypes; // pointee type for references and `this`
      const Ty* enclosingClass = nullptr;
      std::string functionName;
      bool emitted = false;
      bool laidOut = false; // captures and fields known (read where the lambda is written); before that only the struct's name exists
    };
    std::unordered_map<const Ty*, ClosureInfo> closures; // keyed by the lambda type (one per enclosing instantiation)
    static bool isLambda(const Ty* t) { return t && t->kind == TyKind::Lambda && t->decl; }
    static bool isAggregate(const Ty* t) { return isClass(t) || isLambda(t); }
    static LambdaExpr* lambdaOf(const Ty* t) { return isLambda(t) && t->decl->kind == NodeKind::ClassDecl ? static_cast<ClassDecl*>(t->decl)->lambda : nullptr; }
    static bool isGenericLambda(const Ty* t) { LambdaExpr* l = lambdaOf(t); return l && !l->templateParams.empty(); }
    std::unordered_set<std::string> lambdaInstances; // call operators of generic lambdas emitted so far, by IR name
    std::deque<Instantiation> worklist;
    std::unordered_map<std::string, bool> requested; // instantiated names queued or emitted

    struct Object
    {
      std::string addr;
      const Ty* ty;
      bool exception = false; // addr holds a caught exception record: ending it destroys the object
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

    // Exceptions (docs/design.md section 9) on Windows SEH: `throw` raises through kov_throw, one
    // filter function per try statement matches the thrown type against the handlers, and the
    // destructors of the objects between a call and its handler run in cleanup pads.
    struct EHFrame
    {
      bool isHandler;            // false: an open try body; true: an open catch handler
      std::string dispatchLabel; // try: the block of its catchswitch
      std::string parentFunclet; // the funclet token the try or handler is in ("" at top level)
      std::string funclet;       // handler: its catchpad token
      std::string exceptionSlot; // handler: the alloca holding the caught exception record
      size_t scopeDepth;         // scopes.size() when entered
      bool used = false;         // try: some call in the body unwinds to the dispatch
    };
    std::vector<EHFrame> ehFrames;
    std::string ehBlocks;                                     // cleanup pads of the current function
    std::unordered_map<std::string, std::string> cleanupPads; // destruction state -> cleanup block
    std::string funcletOverride;                              // the pad being emitted, for its calls
    bool usesEH = false;                                      // the current function has invokes or pads
    bool usesExceptions = false;                              // the program needs the exception runtime
    uint32_t filterCount = 0;
    std::unordered_map<std::string, bool> emittedShared;      // typeinfo and destructor thunks emitted
    std::string exceptionTypeinfo;                            // typeinfo of the library's Exception, once seen

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
      std::string where;
      if (!currentFunctionName.empty())
        where = " (in " + currentFunctionName + (currentRequestedBy.empty() ? "" : ", requested by " + currentRequestedBy) + ")";
      error(at, "codegen does not support " + what + " yet" + where);
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
          return "ptr";
        case TyKind::MemberPointer: return t->inner && t->inner->kind == TyKind::Function ? "ptr" : "i64"; // a function, or a data member's offset within its class
        case TyKind::Lambda: return t->decl ? closureStructName(t) : "ptr";
        case TyKind::Array:
          return "[" + std::to_string(t->count) + " x " + llType(t->inner, at) + "]";
        case TyKind::Vector:
          return "<" + std::to_string(t->count) + " x " + llType(t->inner, at) + ">";
        case TyKind::Class: return structFor(t, at).name;
        case TyKind::Enum: return llType(enumUnderlying(t->decl), at);
        default:
          unsupported(at, "the type " + types.toString(t, &symbols) + (t->kind == TyKind::Pack ? " (a parameter pack)" : t->kind == TyKind::Param ? " (a template parameter)" : "") +
                            (at ? std::string(" of ") + nodeKindName(at->kind) : std::string()));
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
        case TyKind::Vector: return t->count * sizeOf(t->inner, at);
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
          align = std::max(align, declaredAlignment(t, at)); // `struct alignas(16) X`
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
        case TyKind::Vector: return t->count * sizeOf(t->inner, at); // SSE vectors are 16-byte aligned
        case TyKind::Class:
        {
          uint64_t align = 1;
          for (const Ty* f : structFor(t, at).fieldTypes)
            align = std::max(align, alignOf(f, at));
          return std::max(align, declaredAlignment(t, at));
        }
        default: return std::max<uint64_t>(1, sizeOf(t, at));
      }
    }

    // The alignment a class declares with alignas(N), or 1.
    uint64_t declaredAlignment(const Ty* t, Node* at)
    {
      ClassDecl* c = classDef(t->decl, at);
      if (!c->alignment)
        return 1;
      ParsedFile* saved = currentFile;
      currentFile = c->file ? c->file : currentFile;
      std::optional<int64_t> v = evalConst(c->alignment);
      currentFile = saved;
      return v && *v > 0 ? uint64_t(*v) : 1;
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
          if (m->kind == NodeKind::VariableDecl && !(m->flags & (DF_Static | DF_Typedef | DF_Friend)))
            ft = global.typeOfDeclIn(m, args);
          else if (m->kind == NodeKind::ClassDecl && static_cast<ClassDecl*>(m)->name.empty() && static_cast<ClassDecl*>(m)->isDefinition)
            ft = types.classType(m, {});
          else
            continue;
          info.fieldIndex[m] = 0;
          info.fieldDecls.push_back(m);
          if (!ft || !ft->isKnown() || types.isDependent(ft))
            continue; // unknown union member: skip its size; known members set a minimum
          if (m->kind == NodeKind::ClassDecl)
            info.anonymous.push_back({m, 0});
          try {
            size = std::max(size, sizeOf(ft, c));
            align = std::max(align, alignOf(ft, c));
          } catch (Unsupported&) {} // keep going with the size from other members
        }
        info.fieldTypes.push_back(types.array(types.intType(uint8_t(align * 8)), uint32_t((size + align - 1) / align)));
        decls += info.name + " = type { " + llType(info.fieldTypes[0], c) + " }\n";
        ++result.structs;
        return structs[key];
      }
      if (!c->templateParams.empty() && args.pairs.size() != c->templateParams.size())
      {
        Decl* lastParam = c->templateParams[c->templateParams.size() - 1];
        bool emptyPack = args.pairs.size() + 1 == c->templateParams.size() && lastParam->kind == NodeKind::TemplateParamDecl &&
                         static_cast<TemplateParamDecl*>(lastParam)->isPack;
        if (!emptyPack)
          unsupported(at, "the class template " + global.qualifiedName(c) + " used without all its arguments");
      }
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
      int bitUnitIndex = -1;    // the storage field the current run of bit fields shares
      const Ty* bitUnitType = nullptr;
      uint32_t bitUsed = 0;
      for (Decl* m : c->members)
      {
        if (m->kind == NodeKind::ClassDecl && static_cast<ClassDecl*>(m)->name.empty() && static_cast<ClassDecl*>(m)->isDefinition)
        {
          // Anonymous struct/union member: a field whose own members are reachable by name.
          const Ty* ft = types.classType(m, {});
          try { structFor(ft, c); } catch (Unsupported&) {} // partial inner struct is OK: later fields still get indexed
          info.anonymous.push_back({m, uint32_t(info.fieldTypes.size())});
          info.fieldIndex[m] = uint32_t(info.fieldTypes.size());
          info.fieldDecls.push_back(m);
          info.fieldTypes.push_back(ft);
          bitUnitIndex = -1;
          continue;
        }
        if (m->kind != NodeKind::VariableDecl || (m->flags & (DF_Static | DF_Typedef | DF_Friend)))
          continue;
        const Ty* ft = global.typeOfDeclIn(m, args);
        if (!ft || !ft->isKnown() || types.isDependent(ft))
        {
          // Unknown member type: insert an 8-byte placeholder so subsequent fields keep
          // correct indices and bitfield runs reset. The layout will be approximate but
          // functions accessing later fields can still be generated.
          ft = types.array(types.intType(8), 8);
          bitUnitIndex = -1;
          info.fieldIndex[m] = uint32_t(info.fieldTypes.size());
          info.fieldDecls.push_back(m);
          info.fieldTypes.push_back(ft);
          continue;
        }
        if (Expr* widthExpr = static_cast<VariableDecl*>(m)->bitfield)
        {
          // `unsigned level : 3;`: consecutive bit fields of one type share a storage unit of that
          // type while their widths fit; each remembers its shift and width.
          ParsedFile* savedFile = currentFile;
          currentFile = c->file ? c->file : currentFile;
          std::optional<int64_t> w = evalConst(widthExpr);
          currentFile = savedFile;
          if (!w || *w <= 0)
            unsupported(m, "a bit field whose width is not a constant");
          const Ty* unit = types.unqualified(ft);
          if (unit->kind == TyKind::Enum)
            unit = enumUnderlying(unit->decl);
          if (unit->kind == TyKind::Bool)
            unit = types.intType(8, true);
          uint32_t unitBits = uint32_t(sizeOf(unit, m) * 8), width = uint32_t(*w);
          if (bitUnitIndex < 0 || bitUnitType != unit || bitUsed + width > unitBits)
          {
            bitUnitIndex = int(info.fieldTypes.size());
            bitUnitType = unit;
            bitUsed = 0;
            info.fieldTypes.push_back(unit);
            info.fieldDecls.push_back(m); // the first field of the unit stands for the unit in memberwise work
          }
          info.fieldIndex[m] = uint32_t(bitUnitIndex);
          info.bitfields[m] = {bitUsed, width};
          bitfieldOf[m] = {bitUsed, width, unit};
          bitUsed += width;
          continue;
        }
        bitUnitIndex = -1;
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
        // Slot 0 is the class's descriptor: its name (typeid) and the primary base's descriptor
        // (dynamic_cast walks that chain); the methods follow (loadSlot skips the descriptor).
        std::string ti = "@\"typeinfo." + types.toString(key, &symbols) + "\"";
        structs[key].typeInfoName = ti;
        structs[key].primaryBase = primaryPolymorphic ? baseKeys[0] : nullptr;
        bool savedShared = emittingShared;
        emittingShared = true; // the vtable text goes into every module that uses it
        std::string nameConstant = cStringConstant(types.toString(key, &symbols));
        emittingShared = savedShared;
        std::string baseDescriptor = primaryPolymorphic ? structs[baseKeys[0]].typeInfoName : std::string("null");
        std::string descriptor = "$" + ti.substr(1) + " = comdat any\n" + ti + " = linkonce_odr constant { ptr, ptr } { ptr " + nameConstant +
                                 ", ptr " + baseDescriptor + " }, comdat\n";
        std::string entries = "ptr " + ti;
        for (size_t i = 0; i < impls2.size(); ++i)
        {
          FunctionDecl* f = impls2[i];
          std::string entry = "ptr null";
          if (f && !(f->flags & DF_Pure))
            entry = "ptr " + useFunction(f, global.classArgsOf(owners2[i]), owners2[i]);
          entries += ", " + entry;
        }
        structs[key].vtableText = descriptor + "$" + vt.substr(1) + " = comdat any\n" + vt + " = linkonce_odr constant [" +
                                  std::to_string(impls2.size() + 1) + " x ptr] [" + entries + "], comdat\n";
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
      fn += "  " + entry + " = getelementptr inbounds ptr, ptr " + vptr + ", i64 " + std::to_string(slot + 1) + "\n"; // after the name
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
        if (m->kind == NodeKind::FunctionDecl && (m->flags & DF_Constructor))
          candidates.push_back(static_cast<FunctionDecl*>(m));
      return global.chooseOverload(candidates, argTypes, args, nullptr);
    }

    std::vector<const Ty*> argTypesOf(Span<Expr*> args)
    {
      std::vector<const Ty*> out;
      for (Expr* a : args)
      {
        if (a && isPackExpansion(a))
        {
          // `args...`: one argument per element of the pack, under the current instantiation.
          Decl* param = packIn(a);
          Decl* tp = param ? templatePackOf(param) : nullptr;
          const Ty* pack = nullptr;
          if (tp && currentArgs)
            for (auto& [d, t] : currentArgs->pairs)
              if (d == tp)
                pack = t;
          if (pack && pack->kind == TyKind::Pack)
          {
            for (const Ty* element : pack->args)
              out.push_back(argumentType(a, element, types));
            continue;
          }
        }
        if (a && a->kind == NodeKind::BraceInitExpr)
        {
          out.push_back(braceListType(a, global.initializerListDecl(), types));
          continue;
        }
        out.push_back(a ? argumentType(a, a->ty, types) : nullptr);
      }
      return out;
    }

    bool isInitializerListType(const Ty* t)
    {
      Decl* d = global.initializerListDecl();
      return d && t && t->kind == TyKind::Class && t->decl == d;
    }

    // `{a, b, c}` for an InitializerList<E> parameter: the elements in a temporary array, the list
    // object (a pointer and a count) pointing at them; both live to the end of the statement.
    std::string initializerListTemporary(const Ty* listTy, BraceInitExpr* b, Node* at)
    {
      if (listTy->args.empty())
        unsupported(at, "an InitializerList without its element type");
      const Ty* element = listTy->args[0];
      uint32_t n = uint32_t(b->args.size());
      const Ty* arrayTy = types.array(element, n);
      std::string array = newObjectSlot(arrayTy, at);
      for (uint32_t i = 0; i < n; ++i)
      {
        std::string slot = newTemp();
        fn += "  " + slot + " = getelementptr inbounds " + llType(arrayTy, at) + ", ptr " + array + ", i64 0, i64 " + std::to_string(i) + "\n";
        initializeFrom(slot, element, b->args[i], at);
      }
      if (needsDestruction(arrayTy, at))
        statementTemps.push_back({array, arrayTy});
      std::string list = newObjectSlot(listTy, at);
      StructInfo info = structFor(listTy, at);
      if (info.fieldDecls.size() < 2)
        unsupported(at, "an InitializerList class without its two fields");
      fn += "  store ptr " + array + ", ptr " + gep(info.name, list, info.fieldIndex[info.fieldDecls[0]]) + "\n";
      fn += "  store i32 " + std::to_string(n) + ", ptr " + gep(info.name, list, info.fieldIndex[info.fieldDecls[1]]) + "\n";
      return list;
    }

    // Default-constructs every element of an array of objects at addr.
    void constructArray(const std::string& addr, const Ty* t, Node* at)
    {
      for (uint32_t i = 0; i < t->count; ++i)
      {
        std::string element = newTemp();
        fn += "  " + element + " = getelementptr inbounds " + llType(t, at) + ", ptr " + addr + ", i64 0, i64 " + std::to_string(i) + "\n";
        if (isClass(t->inner))
          defaultConstruct(element, t->inner, at);
        else if (t->inner->kind == TyKind::Array)
          constructArray(element, t->inner, at);
        else
          fn += "  store " + llType(t->inner, at) + " zeroinitializer, ptr " + element + "\n";
      }
    }

    // A destructor written by the user; `~T() = default` is the compiler's memberwise one.
    FunctionDecl* findDestructor(ClassDecl* c)
    {
      for (Decl* m : c->members)
        if (m->kind == NodeKind::FunctionDecl && (m->flags & DF_Destructor) && !(m->flags & DF_Default))
          return static_cast<FunctionDecl*>(m);
      return nullptr;
    }

    // Constructors written by the user (`= default` ones are the compiler's memberwise ones).
    bool hasUserConstructors(ClassDecl* c)
    {
      for (Decl* m : c->members)
        if (m->kind == NodeKind::FunctionDecl && (m->flags & DF_Constructor) && !(m->flags & DF_Default))
          return true;
      return false;
    }

    // Default construction does something: user constructors, a vtable pointer, default member
    // initializers, or a base/member class that needs constructing (a zeroed global is not enough).
    bool needsConstruction(const Ty* t, Node* at, int depth = 0)
    {
      t = types.stripRef(t);
      if (t->kind == TyKind::Array)
        return needsConstruction(t->inner, at, depth);
      if (!isClass(t) || !t->decl || depth > 16)
        return false;
      if (hasUserConstructors(classDef(t->decl, at)))
        return true;
      StructInfo info = structFor(t, at); // copy: nested calls may add structs
      if (info.polymorphic)
        return true;
      for (Decl* m : info.fieldDecls)
        if (m->kind == NodeKind::VariableDecl && static_cast<VariableDecl*>(m)->init)
          return true;
      for (const Ty* ft : info.fieldTypes)
        if (needsConstruction(ft, at, depth + 1))
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
      if (c->isTypedUnion)
      {
        // A typed union destroys whichever alternative it holds.
        bool any = false;
        for (size_t i = 0; i < c->alternatives.size(); ++i)
          any = any || needsDestruction(alternativeType(c, t, i, at), at);
        structFor(t, at).needsDestruction = any;
        return any;
      }
      bool result = findDestructor(c) != nullptr;
      for (const Ty* ft : std::vector<const Ty*>(info.fieldTypes))
        result = result || (!ft->isReference() && needsDestruction(ft, at)); // a reference member owns nothing
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
            emitCall("", "void " + fp + "(ptr " + addr + ")", false);
            return;
          }
      }
      if (c->isTypedUnion)
      {
        destroyAlternative(c, t, addr, at);
        return;
      }
      if (FunctionDecl* dtor = findDestructor(c))
      {
        TemplateArgs args = global.classArgsOf(canonicalClass(t, at));
        emitCall("", "void " + useFunction(dtor, args, canonicalClass(t, at)) + "(ptr " + addr + ")", false);
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
        if (!ft->isReference() && needsDestruction(ft, at))
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
      if (c->isTypedUnion)
      {
        // `Property p;`: the first alternative, default-constructed.
        fn += "  store " + llType(t, at) + " zeroinitializer, ptr " + addr + "\n";
        if (!c->alternatives.empty())
          constructAlternative(c, t, addr, 0, nullptr, at);
        return;
      }
      if (hasUserConstructors(c))
      {
        FunctionDecl* ctor = findConstructor(t, {}, at);
        if (!ctor && !inheritedConstructorBase(c))
          unsupported(at, "default construction of " + global.qualifiedName(c) + ", which has no default constructor");
        if (ctor && !(ctor->flags & DF_Default))
        {
          // Through callFunction: `Style(const Style* parent = nullptr)` wants its default argument.
          TemplateArgs args = global.classArgsOf(canonicalClass(t, at));
          callFunction(ctor, args, canonicalClass(t, at), addr, "", {}, at);
          return;
        }
        // `T() = default;` next to other constructors: the memberwise default construction below.
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
      if (isClass(t) && t->decl)
      {
        // `Frame(x) : Frame(x, "") {}`: a delegating constructor builds the whole object.
        const QualifiedName& own = classDef(t->decl, at)->name;
        if (const FunctionDecl::CtorInit* self = own.empty() ? nullptr : initFor(own.last().symbol))
        {
          constructInto(addr, t, self->args, at);
          return;
        }
      }
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
        if (const Ty* declared = global.typeOfDecl(v); ft->isReference() || (declared && declared->isReference()))
        {
          // A reference member: the field holds the address of what it refers to.
          Expr* source = init && init->args.size() == 1 ? init->args[0] : init ? nullptr : v->init;
          if (!source)
            unsupported(at, "a reference member without an initializer");
          withSingleElement(source, at, [&](Expr* one)
          {
            std::string target = isClass(types.stripRef(one->ty)) ? rvalue(one).text : lvalue(one);
            fn += "  store ptr " + target + ", ptr " + fieldAddr + "\n";
          });
          continue;
        }
        if (init)
        {
          if (isClass(ft))
            constructInto(fieldAddr, ft, init->args, at);
          else if (init->args.size() == 1)
            withSingleElement(init->args[0], at, [&](Expr* one) { store(rvalue(one), fieldAddr, ft, at); });
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
          constructArray(fieldAddr, ft, at);
        else
          fn += "  store " + llType(ft, at) + " zeroinitializer, ptr " + fieldAddr + "\n";
      }
    }

    // The constructor of class t that takes an InitializerList (the rest defaulted), or null.
    FunctionDecl* initializerListConstructor(ClassDecl* c, const Ty* t, Node* at)
    {
      TemplateArgs classArgs = global.classArgsOf(canonicalClass(t, at));
      ArgsScope scope(*this, &classArgs);
      for (Decl* m : c->members)
      {
        if (m->kind != NodeKind::FunctionDecl || !(m->flags & DF_Constructor) || (m->flags & DF_Delete))
          continue;
        auto* f = static_cast<FunctionDecl*>(m);
        if (f->params.empty() || !f->templateParams.empty())
          continue;
        bool othersDefaulted = true;
        for (size_t i = 1; i < f->params.size(); ++i)
          othersDefaulted = othersDefaulted && static_cast<ParamDecl*>(f->params[i])->defaultValue != nullptr;
        if (!othersDefaulted)
          continue;
        const Ty* pt = declType(f->params[0]);
        if (pt && isInitializerListType(types.stripRef(pt)))
          return f;
      }
      return nullptr;
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
        if (FunctionDecl* ctor = initializerListConstructor(c, t, at))
        {
          // `Vector<int> v = {1, 2, 3}`, `Poly p({1, 2, 3, 4})`: the list as one InitializerList argument.
          TemplateArgs classArgs = global.classArgsOf(canonicalClass(t, at));
          callFunction(ctor, classArgs, canonicalClass(t, at), addr, "", args, at);
          return;
        }
        pendingDesignators = static_cast<BraceInitExpr*>(args[0])->designators;
        constructInto(addr, t, static_cast<BraceInitExpr*>(args[0])->args, at);
        return;
      }
      Span<uint32_t> designators = pendingDesignators;
      pendingDesignators = {};
      // `using Base::Base;`: the base's constructors, when none of the class's own takes the
      // arguments (a copy stays a copy).
      bool copyLike = args.size() == 1 && args[0]->ty && isClass(types.stripRef(args[0]->ty)) && classKey(types.stripRef(args[0]->ty)) == classKey(t);
      if (!copyLike && (!hasUserConstructors(c) || !findConstructor(t, argTypesOf(args), at)))
        if (std::optional<QualifiedName> base = inheritedConstructorBase(c))
        {
          // Inherited constructors: the arguments build that base, everything else is default.
          FunctionDecl::CtorInit init{};
          init.member = *base;
          init.args = args;
          initializeMembers(addr, t, Span<FunctionDecl::CtorInit>{&init, 1}, at);
          return;
        }
      if (args.size() == 1 && isPackExpansion(args[0]))
      {
        // `T(forward<Args>(args)...)`: the elements are the arguments. With one element the
        // constructor choice (copy, move, converting) sees that element's type; several elements
        // of a class without constructors fill its fields in order; otherwise the constructor
        // call below expands the pack itself.
        Expr* pattern = static_cast<UnaryExpr*>(args[0])->operand;
        size_t count = packElementCount(pattern, at);
        if (count == 0)
        {
          defaultConstruct(addr, t, at);
          return;
        }
        if (count == 1)
        {
          forEachPackElement(pattern, at, [&](size_t)
          {
            Expr* one = pattern;
            constructInto(addr, t, Span<Expr*>{&one, 1}, at);
          });
          return;
        }
        if (!hasUserConstructors(c))
        {
          StructInfo info = structFor(t, at);
          fn += "  store " + llType(t, at) + " zeroinitializer, ptr " + addr + "\n";
          size_t i = 0;
          forEachPackElement(pattern, at, [&](size_t)
          {
            if (i >= info.fieldDecls.size())
              unsupported(at, "more constructor arguments than " + global.qualifiedName(c) + " has fields");
            Decl* d = info.fieldDecls[i++];
            initializeFrom(gep(info.name, addr, info.fieldIndex[d]), info.fieldTypes[info.fieldIndex[d]], pattern, at);
          });
          return;
        }
      }
      const Ty* firstTy = args.size() == 1 ? args[0]->ty : nullptr;
      FunctionDecl* ctor = findConstructor(t, argTypesOf(args), at);
      if (ctor && (ctor->flags & DF_Delete))
        unsupported(at, "constructing " + types.toString(t, &symbols) + " with a deleted constructor (copying a move-only object?)");
      if (ctor && firstTy && isClass(types.stripRef(firstTy)) && classKey(firstTy) == classKey(t) && ctor->params.size() == 1)
      {
        // The same class: a copy, unless the chosen constructor is a copy constructor itself.
        TemplateArgs classArgs = global.classArgsOf(canonicalClass(t, at));
        const Ty* pt;
        {
          ArgsScope scope(*this, &classArgs);
          pt = declType(ctor->params[0]);
        }
        if (!pt || !isClass(types.stripRef(pt)) || classKey(pt) != classKey(t))
          ctor = nullptr; // a converting constructor of another class: not for a copy
      }
      if (ctor && firstTy && isClass(types.stripRef(firstTy)) && classKey(firstTy) != classKey(t) && ctor->params.size() == 1)
      {
        TemplateArgs classArgs = global.classArgsOf(canonicalClass(t, at));
        const Ty* pt;
        {
          ArgsScope scope(*this, &classArgs);
          pt = declType(ctor->params[0]);
        }
        if (pt && isClass(types.stripRef(pt)) && classKey(pt) == classKey(t))
        {
          // The copy constructor, chosen for a value of another class: that class converts to t
          // first (a base, or its conversion operator), the constructor is not called with it.
          Value src = convert(rvalue(args[0]), types.unqualified(t), at);
          if (src.isTemporary)
            moveTemporary(addr, src, t, at);
          else if (isXvalue(args[0]))
            moveConstruct(addr, src.text, t, at);
          else
            copyConstruct(addr, src.text, t, at);
          return;
        }
      }
      if (ctor)
      {
        TemplateArgs ctorArgs = global.classArgsOf(canonicalClass(t, at));
        if (!ctor->templateParams.empty())
          global.deduceArgs(ctor, argTypesOf(args), ctorArgs); // `template<typename F> Function(F)`: F from the argument
        callFunction(ctor, ctorArgs, canonicalClass(t, at), addr, "", args, at);
        return;
      }
      if (args.size() == 1 && firstTy && isClass(types.stripRef(firstTy)) && classKey(firstTy) == classKey(t))
      {
        Value src = rvalue(args[0]);
        if (src.isTemporary)
          moveTemporary(addr, src, t, at);
        else if (isXvalue(args[0]))
          moveConstruct(addr, src.text, t, at);
        else
          copyConstruct(addr, src.text, t, at);
        return;
      }
      if (!hasUserConstructors(c))
      {
        // An aggregate: the values fill the fields in order; the fields left over get their
        // default member initializers (or default construction), as in C++.
        StructInfo info = structFor(t, at);
        fn += "  store " + llType(t, at) + " zeroinitializer, ptr " + addr + "\n";
        if (info.polymorphic)
        {
          useVtable(info);
          fn += "  store ptr " + info.vtableName + ", ptr " + addr + "\n";
        }
        if (info.fieldDecls.size() == 1 && info.bases.empty() && designators.empty() && args.size() > 1)
        {
          // Brace elision: `Array<int, 3> a = {1, 2, 3}` fills the one array field element by element.
          const Ty* ft = info.fieldTypes[info.fieldIndex[info.fieldDecls[0]]];
          if (ft->kind == TyKind::Array)
          {
            std::string fieldAddr = gep(info.name, addr, info.fieldIndex[info.fieldDecls[0]]);
            if (args.size() > ft->count)
              unsupported(at, "more values than the array of " + global.qualifiedName(c) + " holds");
            for (size_t i = 0; i < args.size(); ++i)
            {
              std::string element = newTemp();
              fn += "  " + element + " = getelementptr inbounds " + llType(ft, at) + ", ptr " + fieldAddr + ", i64 0, i64 " + std::to_string(i) + "\n";
              initializeFrom(element, ft->inner, args[i], at);
            }
            return;
          }
        }
        // Which value goes to which field: in order, or by name for `.x = 1` designators.
        std::vector<Expr*> valueOf(info.fieldDecls.size(), nullptr);
        size_t next = 0;
        for (size_t k = 0; k < args.size(); ++k)
        {
          uint32_t designator = k < designators.size() ? designators[k] : UINT32_MAX;
          if (designator != UINT32_MAX)
          {
            size_t found = info.fieldDecls.size();
            for (size_t j = 0; j < info.fieldDecls.size(); ++j)
              if (info.fieldDecls[j]->kind == NodeKind::VariableDecl && !static_cast<VariableDecl*>(info.fieldDecls[j])->name.empty() &&
                  static_cast<VariableDecl*>(info.fieldDecls[j])->name.last().symbol == designator)
                found = j;
            if (found == info.fieldDecls.size())
              unsupported(at, "a designator naming no field of " + global.qualifiedName(c) + " (" + sym(designator) + ")");
            valueOf[found] = args[k];
            next = found + 1;
            continue;
          }
          if (next >= valueOf.size())
            unsupported(at, "more values than " + global.qualifiedName(c) + " has fields");
          valueOf[next++] = args[k];
        }
        size_t i = 0;
        for (Decl* d : info.fieldDecls)
        {
          std::string fieldAddr = gep(info.name, addr, info.fieldIndex[d]);
          const Ty* ft = info.fieldTypes[info.fieldIndex[d]];
          if (Expr* value = valueOf[i++])
          {
            initializeFrom(fieldAddr, ft, value, at);
            continue;
          }
          if (d->kind != NodeKind::VariableDecl)
            continue;
          auto* v = static_cast<VariableDecl*>(d);
          if (v->init)
            initializeFrom(fieldAddr, ft, v->init, v);
          else if (isClass(ft))
            defaultConstruct(fieldAddr, ft, at);
          else if (ft->kind == TyKind::Array && needsDestruction(ft, at))
            constructArray(fieldAddr, ft, at);
        }
        return;
      }
      unsupported(at, "a constructor of " + global.qualifiedName(c) + " taking " + std::to_string(args.size()) + " arguments");
    }

    // `auto [a, b] = pair;`: e names a binding of the hidden local v; out gets the address of the
    // matching data member (or array element). False when e is not one of v's bindings.
    bool bindingAddress(VariableDecl* v, Node* e, std::string& out)
    {
      if (v->structuredBindings.empty() || !e || e->kind != NodeKind::NameExpr)
        return false;
      uint32_t symbol = static_cast<NameExpr*>(e)->name.last().symbol;
      size_t index = v->structuredBindings.size();
      for (size_t i = 0; i < v->structuredBindings.size(); ++i)
        if (static_cast<NameExpr*>(v->structuredBindings[i])->name.last().symbol == symbol)
          index = i;
      if (index == v->structuredBindings.size())
        return false;
      auto it = locals.find(v);
      if (it == locals.end())
        unsupported(e, "a structured binding outside its function");
      std::string base = it->second.addr;
      if (it->second.isRef)
      {
        std::string r = newTemp();
        fn += "  " + r + " = load ptr, ptr " + base + "\n";
        base = r;
      }
      const Ty* object = types.unqualified(types.stripRef(it->second.ty));
      if (object->kind == TyKind::Array)
      {
        std::string r = newTemp();
        fn += "  " + r + " = getelementptr inbounds " + llType(object->inner, e) + ", ptr " + base + ", i64 " + std::to_string(index) + "\n";
        out = r;
        return true;
      }
      if (!isClass(object))
        unsupported(e, "a structured binding of something that is neither a class nor an array");
      StructInfo info = structFor(object, e); // copy: fieldAddress may add structs
      if (index >= info.fieldDecls.size() || !fieldAddress(object, info.fieldDecls[index], base, out, e))
        unsupported(e, "a structured binding of a class without that many data members");
      return true;
    }

    // `return result;` naming a local object or a by-value parameter of the returned class: it is
    // moved into the result, not copied (C++'s implicit move on return).
    Decl* returnedLocalObject(Expr* e)
    {
      if (!e || e->kind != NodeKind::NameExpr)
        return nullptr;
      Decl* d = static_cast<NameExpr*>(e)->resolved;
      if (!d || (d->kind != NodeKind::VariableDecl && d->kind != NodeKind::ParamDecl) || (d->flags & DF_Static))
        return nullptr;
      if (d->kind == NodeKind::VariableDecl && !static_cast<VariableDecl*>(d)->structuredBindings.empty())
        return nullptr;
      auto it = locals.find(d);
      if (it == locals.end() || it->second.isRef || !isClass(it->second.ty) || classKey(it->second.ty) != classKey(currentReturn))
        return nullptr;
      return d;
    }

    // A bit field's value: the storage unit shifted and masked (sign-extended for signed types).
    Value loadBitfield(const std::string& addr, const BitfieldInfo& bits, const Ty* memberTy, Node* at)
    {
      std::string unitLl = llType(bits.unit, at);
      uint32_t unitBits = uint32_t(sizeOf(bits.unit, at) * 8);
      std::string unit = newTemp(), r;
      fn += "  " + unit + " = load " + unitLl + ", ptr " + addr + "\n";
      const Ty* valueTy = types.unqualified(memberTy);
      bool isSigned = (valueTy->kind == TyKind::Int || valueTy->kind == TyKind::Char) && !valueTy->isUnsigned;
      if (isSigned)
      {
        std::string left = newTemp();
        fn += "  " + left + " = shl " + unitLl + " " + unit + ", " + std::to_string(unitBits - bits.shift - bits.width) + "\n";
        r = newTemp();
        fn += "  " + r + " = ashr " + unitLl + " " + left + ", " + std::to_string(unitBits - bits.width) + "\n";
      }
      else
      {
        std::string shifted = newTemp();
        fn += "  " + shifted + " = lshr " + unitLl + " " + unit + ", " + std::to_string(bits.shift) + "\n";
        r = newTemp();
        uint64_t mask = bits.width >= 64 ? ~0ull : ((1ull << bits.width) - 1);
        fn += "  " + r + " = and " + unitLl + " " + shifted + ", " + std::to_string(mask) + "\n";
      }
      Value asUnit{r, bits.unit};
      if (valueTy->kind == TyKind::Bool)
      {
        std::string b = newTemp();
        fn += "  " + b + " = icmp ne " + unitLl + " " + r + ", 0\n";
        return {b, valueTy};
      }
      return convert(asUnit, valueTy, at);
    }

    // Writes a bit field: the other bits of the storage unit stay as they are.
    void storeBitfield(Value v, const std::string& addr, const BitfieldInfo& bits, const Ty* memberTy, Node* at)
    {
      std::string unitLl = llType(bits.unit, at);
      Value asUnit = convert(v, bits.unit, at);
      uint64_t mask = bits.width >= 64 ? ~0ull : ((1ull << bits.width) - 1);
      std::string old = newTemp(), cleared = newTemp(), masked = newTemp(), shifted = newTemp(), merged = newTemp();
      fn += "  " + old + " = load " + unitLl + ", ptr " + addr + "\n";
      fn += "  " + cleared + " = and " + unitLl + " " + old + ", " + std::to_string(~(mask << bits.shift)) + "\n";
      fn += "  " + masked + " = and " + unitLl + " " + asUnit.text + ", " + std::to_string(mask) + "\n";
      fn += "  " + shifted + " = shl " + unitLl + " " + masked + ", " + std::to_string(bits.shift) + "\n";
      fn += "  " + merged + " = or " + unitLl + " " + cleared + ", " + shifted + "\n";
      fn += "  store " + unitLl + " " + merged + ", ptr " + addr + "\n";
    }

    // The address of the object a variable or parameter declaration names (what lvalue gives a
    // name expression): a local, a field of this, or a global.
    std::string addressOfDecl(Decl* d, Node* e)
    {
      if (d->kind == NodeKind::VariableDecl)
        if (std::string bound; bindingAddress(static_cast<VariableDecl*>(d), e, bound))
          return bound;
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
        return globalAddress(static_cast<VariableDecl*>(d), e, qualifierClass(e, e));
      }
      unsupported(e, "taking the address of " + global.qualifiedName(d) + " (" + nodeKindName(d->kind) + (packLocals.count(d) ? ", a parameter pack outside its expansion" : "") + ")");
    }

    // Whether an expression is an xvalue: an rvalue reference to an object that may be moved from
    // (`move(x)`); a named rvalue reference is an lvalue.
    static bool isXvalue(Expr* e) { return e && e->ty && e->ty->kind == TyKind::RRef && !isLvalueExpression(e); }

    // Move-constructs the object at src into dst: through the move constructor where the class has
    // one, as a copy otherwise.
    void moveConstruct(const std::string& dst, const std::string& src, const Ty* t, Node* at)
    {
      ClassDecl* c = classDef(t->decl, at);
      TemplateArgs args = global.classArgsOf(canonicalClass(t, at));
      FunctionDecl* move = nullptr;
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
          if (f->flags & DF_Default)
            continue; // `T(T&&) = default`: the memberwise move below
          if (pt && pt->kind == TyKind::RRef && isClass(pt->inner) && classKey(pt->inner) == classKey(t))
            move = f;
        }
      }
      if (move && !(move->flags & DF_Delete))
      {
        emitCall("", "void " + useFunction(move, args, canonicalClass(t, at)) + "(ptr " + dst + ", ptr " + src + ")");
        return;
      }
      copyConstruct(dst, src, t, at, true); // the user copy constructor if any, else memberwise moves
    }

    // Copy-constructs the object at src into dst (user copy constructor or bitwise copy); with
    // `move`, a class without a user copy constructor moves its members instead.
    void copyConstruct(const std::string& dst, const std::string& src, const Ty* t, Node* at, bool move = false)
    {
      ClassDecl* c = classDef(t->decl, at);
      if (c->isTypedUnion)
      {
        copyAlternative(c, t, dst, src, move, at);
        return;
      }
      TemplateArgs args = global.classArgsOf(canonicalClass(t, at));
      FunctionDecl* copy = nullptr;
      bool hasMove = false;
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
          if (!pt || !pt->isReference() || !isClass(pt->inner) || classKey(pt->inner) != classKey(t))
            continue;
          if (f->flags & DF_Default)
            continue; // `T(const T&) = default`: the memberwise copy below
          if (pt->kind == TyKind::LRef)
            copy = f; // `T(const T&)`; the move constructor `T(T&&)` would empty the source
          else
            hasMove = true;
        }
      }
      if (copy && (copy->flags & DF_Delete))
        unsupported(at, "copying " + global.qualifiedName(c) + ", whose copy constructor is deleted (move it instead)");
      if (!copy && hasMove)
        unsupported(at, "copying " + global.qualifiedName(c) + ", which has a move constructor but no copy constructor");
      if (copy)
      {
        emitCall("", "void " + useFunction(copy, args, canonicalClass(t, at)) + "(ptr " + dst + ", ptr " + src + ")");
        return;
      }
      if (findDestructor(c))
        unsupported(at, "copying " + global.qualifiedName(c) + ", which has a destructor but no copy constructor");
      memberwiseConstruct(dst, src, t, at, move);
    }

    // Constructs the object at dst from the one at src base by base and member by member
    // (bitwise when nothing needs destruction): copies, or moves with `move`.
    void memberwiseConstruct(const std::string& dst, const std::string& src, const Ty* t, Node* at, bool move)
    {
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
      {
        if (move)
          moveConstruct(gep(info.name, dst, index), gep(info.name, src, index), info.fieldTypes[index], at);
        else
          copyConstruct(gep(info.name, dst, index), gep(info.name, src, index), info.fieldTypes[index], at);
      }
      for (Decl* d : info.fieldDecls)
      {
        uint32_t index = info.fieldIndex[d];
        const Ty* ft = info.fieldTypes[index];
        std::string to = gep(info.name, dst, index), from = gep(info.name, src, index);
        if (isClass(ft) && move)
          moveConstruct(to, from, ft, at);
        else if (isClass(ft))
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
    void assignObject(const std::string& dst, const std::string& src, const Ty* t, Node* at, bool move = false)
    {
      if (ClassDecl* c = typedUnionOf(t, at))
      {
        // The alternative held goes, the source's alternative and tag come.
        destroyAlternative(c, t, dst, at);
        copyAlternative(c, t, dst, src, move, at);
        return;
      }
      const Ty* foundIn = nullptr;
      // From an lvalue: operator=(const T&). From a temporary or `move(x)`: operator=(T&&) if there is one.
      std::vector<const Ty*> argTypes{move ? types.rref(types.unqualified(t)) : types.lref(types.qualified(t, true, false))};
      FunctionDecl* op = findMemberFunction(t, "operator=", argTypes, t, at, &foundIn);
      if (op && (op->flags & DF_Default))
        op = nullptr; // `operator=(const T&) = default`: the memberwise assignment below
      if (op)
      {
        if (op->flags & DF_Delete)
          unsupported(at, "assignment to " + global.qualifiedName(t->decl) + " through its deleted operator=");
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
          emitCall("", "void " + name + "(ptr " + thisArg + ", ptr " + src + ")");
        else
          emitCall(newTemp() + " = ", s.returnType + " " + name + "(ptr " + thisArg + ", ptr " + src + ")");
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
      memberwiseAssign(dst, src, t, at, move);
    }

    // Assigns the object at src to the one at dst base by base and member by member.
    void memberwiseAssign(const std::string& dst, const std::string& src, const Ty* t, Node* at, bool move)
    {
      StructInfo info = structFor(t, at);
      for (auto& [baseDef, index] : info.bases)
        assignObject(gep(info.name, dst, index), gep(info.name, src, index), info.fieldTypes[index], at, move);
      for (Decl* d : info.fieldDecls)
      {
        uint32_t index = info.fieldIndex[d];
        const Ty* ft = info.fieldTypes[index];
        std::string to = gep(info.name, dst, index), from = gep(info.name, src, index);
        if (isClass(ft))
          assignObject(to, from, ft, at, move);
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
          Expr* one = init; // constructInto sees the whole list: an InitializerList constructor, designators, or the fields
          constructInto(addr, t, Span<Expr*>{&one, 1}, at);
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
        // SSE intrinsic returning a vector → store directly into the wrapper class's first field.
        if (init->kind == NodeKind::CallExpr && (!init->ty || !init->ty->isKnown()))
        {
          Value v = rvalue(init);
          if (v.ty && v.ty->kind == TyKind::Vector)
          {
            StructInfo& info = structFor(t, at);
            std::string field0 = gep(info.name, addr, 0);
            std::string tmp = newTemp();
            fn += "  " + tmp + " = load " + llType(v.ty, at) + ", ptr " + v.text + "\n";
            fn += "  store " + llType(v.ty, at) + " " + tmp + ", ptr " + field0 + "\n";
            return;
          }
          // Not a vector — fall through to the non-class converting constructor path.
        }
        if (init->ty && !isClass(types.stripRef(init->ty)) && isClass(t))
        {
          // A non-class value: through a converting constructor (`String s = "text"`).
          Expr* one = init;
          constructInto(addr, t, Span<Expr*>{&one, 1}, at);
          return;
        }
        if (init->ty && isClass(types.stripRef(init->ty)) && classKey(init->ty) != classKey(t))
        {
          // Another class: through a converting constructor, a base, or its conversion operator
          // (`String text = vec;` with `operator String()`), never by copying it as a t.
          Expr* one = init;
          constructInto(addr, t, Span<Expr*>{&one, 1}, at);
          return;
        }
        Value src = rvalue(init);
        if (!isClass(src.ty))
          unsupported(at, "initialising a class from a non-class value");
        if (src.isTemporary)
          moveTemporary(addr, src, t, at);
        else if (isXvalue(init))
          moveConstruct(addr, src.text, t, at); // `T tmp = move(a);`
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
        if (t->kind == TyKind::Array)
        {
          // Element by element (nested lists for arrays of arrays); the rest stays zero.
          fn += "  store " + llType(t, at) + " zeroinitializer, ptr " + addr + "\n";
          for (size_t i = 0; i < bi->args.size() && i < t->count; ++i)
          {
            std::string element = newTemp();
            fn += "  " + element + " = getelementptr inbounds " + llType(t, at) + ", ptr " + addr + ", i64 0, i64 " + std::to_string(i) + "\n";
            initializeFrom(element, t->inner, bi->args[i], at);
          }
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
    // Runs body on an expression, or, for a pack expansion `args...` of exactly one element, on its
    // pattern with that element bound (a scalar member initialised with `forward<Args>(args)...`).
    template<typename F>
    void withSingleElement(Expr* e, Node* at, F&& body)
    {
      if (!isPackExpansion(e))
      {
        body(e);
        return;
      }
      Expr* pattern = static_cast<UnaryExpr*>(e)->operand;
      if (packElementCount(pattern, at) != 1)
        unsupported(at, "a pack of " + std::to_string(packElementCount(pattern, at)) + " elements where one value is expected");
      forEachPackElement(pattern, at, [&](size_t) { body(pattern); });
    }

    size_t packElementCount(Expr* pattern, Node* at)
    {
      Decl* param = packIn(pattern);
      if (!param)
        unsupported(at, "a pack expansion that does not name a parameter pack");
      return packLocals[param].size();
    }

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
        case NodeKind::MemberExpr: visit(static_cast<MemberExpr*>(n)->base); visit(static_cast<MemberExpr*>(n)->dynamicMember); break;
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
        case NodeKind::ForConstexprStmt: visit(static_cast<ForConstexprStmt*>(n)->body); break;
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
    // The IR name of a lambda's closure struct: known as soon as the type is mentioned (a slot
    // for the variable holding it is allocated before its expression is reached); the struct is
    // defined once the captures are known.
    std::string closureStructName(const Ty* t)
    {
      t = types.unqualified(t); // `const auto f = [..]{..}`: the const variable holds the same closure
      ClosureInfo& info = closures[t];
      if (info.structName.empty())
        info.structName = "%closure." + std::to_string(closures.size());
      return info.structName;
    }

    // The closure of a lambda type with its captures laid out. Only the lambda expression itself
    // (create) may lay it out: the capture list is read from the function the lambda is written in.
    ClosureInfo& closureFor(const Ty* t, Node* at, bool create = false)
    {
      t = types.unqualified(t);
      auto it = closures.find(t);
      if (it != closures.end() && it->second.laidOut)
        return it->second;
      if (!create)
        unsupported(at, "a lambda used before its closure was built (its captures are read where it is written)");
      auto* c = static_cast<ClassDecl*>(t->decl);
      LambdaExpr* l = c->lambda;
      if (!l)
        unsupported(at, "a lambda without its expression");
      std::string structName = closureStructName(t);
      ClosureInfo& info = closures[t];
      info.laidOut = true;
      info.enclosingClass = currentClassTy;
      bool usesThis = false;
      std::vector<Decl*> explicitDecls;
      bool thisCaptured = false;
      for (const LambdaCapture& cap : l->captures)
      {
        if (cap.init)
        {
          // `[x = expr]`: a field of the closure, initialised from expr when the closure is made.
          if (!cap.decl || cap.byRef)
            unsupported(l, "a by-reference init-capture");
          info.captured.push_back(cap.decl);
          info.byRef.push_back(false);
          info.inits.push_back(cap.init);
          continue;
        }
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
        info.inits.push_back(nullptr);
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
            info.inits.push_back(nullptr);
          }
        if (usesThis)
          thisCaptured = true;
      }
      for (size_t i = 0; i < info.captured.size(); ++i)
      {
        if (info.inits[i])
        {
          const Ty* vt = declType(info.captured[i]);
          if (!vt || !vt->isKnown())
            unsupported(l, "an init-capture of unknown type");
          info.fieldTypes.push_back(types.stripRef(vt));
          continue;
        }
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
        info.inits.push_back(nullptr);
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
      info.functionName = "@\"" + types.toString(t, &symbols) + "::operator()(" + params + ")\"";
      return closures[t];
    }

    // Builds the closure object of a lambda expression in a fresh slot and emits its function.
    Value makeClosure(LambdaExpr* l)
    {
      const Ty* t = l->ty;
      if (!isLambda(t))
        unsupported(l, "a lambda whose type is unknown");
      ClosureInfo info = closureFor(t, l, true);
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
        if (info.inits[i])
        {
          initializeFrom(field, info.fieldTypes[i], info.inits[i], l); // `[x = expr]`
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
      if (!closures[t].emitted && !isGenericLambda(t)) // a generic lambda's operator is emitted per call, once its types are known
      {
        closures[t].emitted = true;
        emitLambda(l, t, types.function(t->inner ? t->inner : types.voidType(), t->args, false), info.functionName, nullptr);
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
      std::vector<EHFrame> ehFrames;
      std::string ehBlocks;
      std::unordered_map<std::string, std::string> cleanupPads;
      bool usesEH;
    };

    FunctionState saveState()
    {
      return {fn, allocas, currentBlock, sretAddr, locals, loops, scopes, statementTemps, temp, label, blockTerminated,
              currentFunction, currentReturn, currentClassTy, ehFrames, ehBlocks, cleanupPads, usesEH};
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
      ehFrames = s.ehFrames;
      ehBlocks = s.ehBlocks;
      cleanupPads = s.cleanupPads;
      usesEH = s.usesEH;
    }

    void resetFunctionState()
    {
      fn.clear();
      allocas.clear();
      locals.clear();
      labelBlocks.clear();
      loops.clear();
      scopes.clear();
      statementTemps.clear();
      ehFrames.clear();
      ehBlocks.clear();
      cleanupPads.clear();
      usesEH = false;
      temp = 0;
      label = 0;
      blockTerminated = false;
      currentBlock = "entry";
    }

    // A function's definition text: the personality and the cleanup pads when it uses exceptions.
    std::string functionText(std::string header)
    {
      if (usesEH)
        header.insert(header.rfind("{\nentry:"), "personality ptr @__C_specific_handler ");
      return header + allocas + fn + ehBlocks + "}\n\n";
    }

    // Emits the lambda's function while another function is being emitted: the enclosing
    // function's state is saved and restored around it. callTy: the operator's type (parameters
    // and result); name: its IR name; args: for an instantiation of a generic lambda, the
    // enclosing instantiation's arguments plus its own, which declarations in the body are typed under.
    void emitLambda(LambdaExpr* l, const Ty* t, const Ty* callTy, const std::string& name, const TemplateArgs* args)
    {
      ClosureInfo info = closureFor(t, l);
      FunctionState saved = saveState();
      const TemplateArgs* savedArgs = currentArgs;
      ParsedFile* savedFile = currentFile;
      resetFunctionState();
      if (args)
        currentArgs = args;
      if (l->closure && l->closure->file)
        currentFile = l->closure->file;
      currentReturn = callTy->inner ? callTy->inner : types.voidType();
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
        for (size_t i = 0; i < callTy->args.size(); ++i)
        {
          const Ty* pt = callTy->args[i];
          std::string pn = "%p" + std::to_string(i);
          paramNames.push_back(pn);
          params += ", " + std::string(isAggregate(pt) || pt->isReference() ? "ptr" : llType(pt, l)) + " " + pn;
        }
        std::string retType = hasSret ? "void" : currentReturn->isReference() ? "ptr" : llType(currentReturn, l);
        std::string header = "define " + retType + " " + name + "(" + params + ") {\nentry:\n";
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
        for (size_t i = 0; i < l->params.size() && i < callTy->args.size(); ++i)
        {
          const Ty* pt = callTy->args[i];
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
            fn += "  ret " + (currentReturn->isReference() ? std::string("ptr null") : llType(currentReturn, l) + " zeroinitializer") + "\n";
        }
        addDefinition(name, functionText(header));
        ++result.functions;
      }
      catch (Unsupported&)
      {
        restoreState(saved);
        currentArgs = savedArgs;
        currentFile = savedFile;
        throw;
      }
      restoreState(saved);
      currentArgs = savedArgs;
      currentFile = savedFile;
    }

    // A lambda without captures used as a pointer to function: a function with the pointer's
    // signature that calls the lambda's operator with no closure (nothing is captured), defined
    // once per lambda.
    std::string lambdaThunk(const Ty* t, const Ty* fnTy, Node* at)
    {
      t = types.unqualified(t);
      LambdaExpr* l = lambdaOf(t);
      ClosureInfo info = closureFor(t, at);
      if (!info.captured.empty() || (l && !l->templateParams.empty()))
        unsupported(at, "a lambda with captures (or a generic one) used as a pointer to function");
      const Ty* ret = fnTy->inner;
      const Ty* lambdaRet = t->inner ? t->inner : types.voidType();
      if (types.unqualified(types.stripRef(ret)) != types.unqualified(types.stripRef(lambdaRet)) || fnTy->args.size() != t->args.size())
        unsupported(at, "a lambda used as a pointer to a function of another signature");
      for (size_t i = 0; i < fnTy->args.size(); ++i)
      {
        const Ty* pt = fnTy->args[i];
        if (types.unqualified(types.stripRef(pt)) != types.unqualified(types.stripRef(t->args[i])))
          unsupported(at, "a lambda used as a pointer to a function of another signature");
        if (isClass(pt) && needsDestruction(pt, at))
          unsupported(at, "a lambda used as a pointer to function taking an object with a destructor by value");
      }
      std::string name = "@\"" + types.toString(t, &symbols) + "::thunk\"";
      if (!lambdaInstances.insert(name).second)
        return name;
      bool hasSret = isClass(ret);
      std::string retType = hasSret ? "void" : ret->isReference() ? "ptr" : llType(ret, at);
      std::string params, args = "ptr null";
      if (hasSret)
      {
        params = "ptr %sret";
        args += ", ptr %sret";
      }
      for (size_t i = 0; i < fnTy->args.size(); ++i)
      {
        const Ty* pt = fnTy->args[i];
        std::string ty = isAggregate(pt) || pt->isReference() ? "ptr" : llType(pt, at);
        params += (params.empty() ? "" : ", ") + ty + " %a" + std::to_string(i);
        args += ", " + ty + " %a" + std::to_string(i);
      }
      std::string text = "define " + retType + " " + name + "(" + params + ") {\nentry:\n";
      if (hasSret || ret->kind == TyKind::Void)
        text += "  call void " + info.functionName + "(" + args + ")\n  ret void\n";
      else
        text += "  %r = call " + retType + " " + info.functionName + "(" + args + ")\n  ret " + retType + " %r\n";
      text += "}\n\n";
      addDefinition(name, text);
      ++result.functions;
      return name;
    }

    // Calls a closure value with arguments.
    Value callClosure(Value closure, Span<Expr*> callArgs, const std::string& sret, Node* at)
    {
      const Ty* t = types.unqualified(closure.ty); // a const lambda variable calls the same operator
      ClosureInfo info = closureFor(t, at);
      const Ty* callTy = types.function(t->inner ? t->inner : types.voidType(), t->args, false);
      std::string functionName = info.functionName;
      TemplateArgs instanceArgs;
      LambdaExpr* genericLambda = lambdaOf(t);
      if (genericLambda && genericLambda->templateParams.empty())
        genericLambda = nullptr;
      if (LambdaExpr* l = genericLambda)
      {
        // A generic lambda: the call's deduced arguments (recorded by the type checker, as for a
        // function template) pick the instantiation of its call operator, emitted at first use.
        CallExpr* call = at && at->kind == NodeKind::CallExpr ? static_cast<CallExpr*>(at) : nullptr;
        if (!call || call->templateArgCount != l->templateParams.size())
          unsupported(at, "a call to a generic lambda whose parameter types were not deduced");
        TemplateArgs own;
        for (size_t i = 0; i < l->templateParams.size(); ++i)
          own.pairs.push_back({l->templateParams[i], call->templateArgs[i]});
        callTy = global.retypeLambdaFor(t, own, &instanceArgs);
        if (!callTy)
          unsupported(at, "a generic lambda whose body could not be typed for this call");
        std::string params;
        for (size_t i = 0; i < callTy->args.size(); ++i)
          params += (i ? ", " : "") + types.toString(callTy->args[i], &symbols);
        functionName = "@\"" + types.toString(t, &symbols) + "::operator()(" + params + ")\"";
      }
      const Ty* ret = callTy->inner ? callTy->inner : types.voidType();
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
            const Ty* pt = position < callTy->args.size() ? callTy->args[position] : nullptr;
            if (!pt)
              unsupported(at, "too many arguments to a lambda");
            sig += ", " + std::string(isAggregate(pt) || pt->isReference() ? "ptr" : llType(pt, at));
            argText += ", " + argumentText(pt, pattern, at);
            ++position;
          });
          continue;
        }
        const Ty* pt = position < callTy->args.size() ? callTy->args[position] : nullptr;
        if (!pt)
          unsupported(at, "too many arguments to a lambda");
        sig += ", " + std::string(isAggregate(pt) || pt->isReference() ? "ptr" : llType(pt, at));
        argText += ", " + argumentText(pt, arg, at);
        ++position;
      }
      sig += ")";
      // The instantiation's operator is emitted after the arguments: a lambda argument's closure
      // (its capture list) is built here, in the function that writes it, before the operator's
      // body calls it.
      if (genericLambda && lambdaInstances.insert(functionName).second)
        emitLambda(genericLambda, t, callTy, functionName, &instanceArgs);
      if (hasSret || ret->kind == TyKind::Void)
      {
        emitCall("", sig + " " + functionName + "(" + argText + ")");
        return hasSret ? Value{resultAddr, ret, sret.empty()} : Value{"", types.voidType(), false};
      }
      std::string r = newTemp();
      emitCall(r + " = ", sig + " " + functionName + "(" + argText + ")");
      return {r, ret, false};
    }

    // --- functions ---

    // IR name of a function (for an instantiation: with the class and template arguments).
    std::string functionName(FunctionDecl* f, const TemplateArgs& args, const Ty* classTy)
    {
      if (global.isExternC(f))
        return "@" + sym(f->name.last().symbol);
      if (f->name.parts.size() == 1 && sym(f->name.parts[0].symbol) == "main" && !global.owningClass(f))
        return "@kov.main"; // called by the generated @main, which reports uncaught exceptions
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
        if (f->name.parts.size() > 1 && !global.owningClass(f))
        {
          // `int M::twice(int) {}` outside its namespace: the name the declaration in M has.
          std::string prefix;
          for (size_t i = 0; i + 1 < f->name.parts.size(); ++i)
            if (f->name.parts[i].symbol != UINT32_MAX)
              prefix += sym(f->name.parts[i].symbol) + "::";
          size_t pos = q.rfind("::");
          std::string head = pos == std::string::npos ? "" : q.substr(0, pos + 2), tail = pos == std::string::npos ? q : q.substr(pos + 2);
          if (!prefix.empty() && head.find(prefix) == std::string::npos)
            q = head + prefix + tail;
        }
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
      else if ((f->flags & DF_Specialization) && f->isTemplate)
      {
        // `template<> Vector<String> splitWords(StringView)`: the explicit arguments, or the return
        // type when the parameter appears only there, tell the specialisations of one template apart.
        std::string spelled;
        for (Node* a : f->name.last().templateArgs)
        {
          const Ty* t = uint8_t(a->kind) <= uint8_t(NodeKind::PackType) ? global.typeOfAst(static_cast<Type*>(a), f) : nullptr;
          spelled += (spelled.empty() ? "" : ", ") + (t && t->isKnown() ? types.toString(t, &symbols) : std::string("?"));
        }
        if (spelled.empty())
          if (const Ty* ft = declType(f); ft && ft->kind == TyKind::Function && ft->inner)
            spelled = types.toString(ft->inner, &symbols);
        q += "<" + spelled + ">";
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
      for (size_t i = 0; i < ft->args.size(); ++i)
      {
        const Ty* pt = ft->args[i];
        if (!pt->isKnown() || types.isDependent(pt))
        {
          std::string name = i < f->params.size() && static_cast<ParamDecl*>(f->params[i])->name != UINT32_MAX
                               ? sym(static_cast<ParamDecl*>(f->params[i])->name) : std::to_string(i + 1);
          unsupported(f, "a parameter of unknown type (" + name + ": " + types.toString(pt, &symbols) + ")");
        }
        if (pt->kind == TyKind::Pack)
        {
          // A deduced pack still in one piece (`Args&&...` of a member template): one parameter per element.
          for (const Ty* e : pt->args)
          {
            s.paramTypes.push_back(isAggregate(e) || e->isReference() ? "ptr" : llType(e, f));
            s.paramTys.push_back(e);
          }
          continue;
        }
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
      if (f->builtin == BUILTIN_EnumFromStr)
      {
        // The generated `E::fromStr(text)` of a Kov enum: the library's enumFromStr<E>(text),
        // which has the same signature (an Optional<E> result slot, the text).
        Decl* lib = global.enumFromStrDecl();
        if (!lib || lib->templateParams.size() != 1 || !f->parent || f->parent->kind != NodeKind::EnumDecl)
          unsupported(f, "E::fromStr without the library's enumFromStr<E>");
        TemplateArgs libArgs;
        libArgs.pairs.push_back({lib->templateParams[0], types.enumType(f->parent)});
        return useFunction(static_cast<FunctionDecl*>(lib), libArgs, nullptr);
      }
      std::string name = functionName(f, args, classTy);
      Decl* defDecl = global.definitionOf(f);
      FunctionDecl* def = defDecl && defDecl->kind == NodeKind::FunctionDecl ? static_cast<FunctionDecl*>(defDecl) : f;
      bool templated = !args.empty();
      if (def->file && def->file->isImported)
        def = f, templated = false; // inline C functions are declared, the library provides them
      if (def != f && (def->flags & DF_Default) && !def->body)
        f->flags |= DF_Default; // `Flow::~Flow() = default;` out of line: the declaration is defaulted too
      if ((f->flags & DF_Default) && !f->body)
      {
        // `= default`: the compiler's memberwise function, defined where first needed (a vtable
        // slot or a function pointer wants a real function; calls inline the body).
        if (!requested[name])
        {
          requested[name] = true;
          worklist.push_back({f, args, classTy, currentModule, currentFunctionName, name});
        }
        return name;
      }
      if (def->body && templated)
      {
        if (!requested[name])
        {
          requested[name] = true;
          // A definition separate from the declaration has its own parameter declarations:
          // `template<typename Policy> ScrollBar<Policy>::ScrollBar(...)` names the class's
          // parameters first, then the member's own.
          TemplateArgs defArgs = args;
          if (def != f)
          {
            Decl* owner = global.owningClass(f);
            Span<Decl*> classParams = owner ? owner->templateParams : Span<Decl*>{};
            size_t classCount = def->templateParams.size() > f->templateParams.size() ? def->templateParams.size() - f->templateParams.size() : 0;
            if (classCount == classParams.size() && classCount + f->templateParams.size() <= def->templateParams.size())
            {
              std::vector<std::pair<Decl*, const Ty*>> added;
              for (auto& [d, t] : defArgs.pairs)
              {
                for (size_t i = 0; i < classCount; ++i)
                  if (d == classParams[i])
                    added.push_back({def->templateParams[i], t});
                for (size_t i = 0; i < f->templateParams.size(); ++i)
                  if (d == f->templateParams[i])
                    d = def->templateParams[classCount + i];
              }
              for (auto& p : added)
                defArgs.pairs.push_back(p);
            }
          }
          worklist.push_back({def, defArgs, classTy, currentModule, currentFunctionName, name});
        }
        return name;
      }
      if (def->body && !(def->file && def->file->isImported))
        return name; // Defined by the main pass.
      if (declaredNames[name])
        return name;
      declaredNames[name] = true;
      {
        static bool debugCalls = getenv("KOV_DEBUG_CALLS") != nullptr;
        if (debugCalls)
          fprintf(stderr, "declare %s: def %s, def flags %x, def body %d, f flags %x, f body %d\n", name.c_str(), def == f ? "self" : "other", def->flags,
                  def->body != nullptr, f->flags, f->body != nullptr);
      }
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
    // `kov_atomic_add32(p, v)` and friends (stdlib/core/Thread.kov): atomic instructions, not calls.
    std::optional<Value> atomicBuiltin(FunctionDecl* f, Span<Expr*> callArgs, Node* at)
    {
      std::string name = sym(f->name.last().symbol);
      if (name.rfind("kov_atomic_", 0) != 0)
        return std::nullopt;
      std::string op = name.substr(11);
      std::string width = op.size() >= 2 ? op.substr(op.size() - 2) : "";
      op = op.substr(0, op.size() - 2);
      std::string ty = width == "64" ? "i64" : "i32";
      std::string align = width == "64" ? "8" : "4";
      if (callArgs.empty())
        unsupported(at, "an atomic builtin without its pointer");
      std::string p = convert(rvalue(callArgs[0]), types.pointer(types.voidType()), at).text;
      const Ty* valueTy = width == "64" ? types.intType(64, false) : types.intType(32, false);
      auto arg = [&](size_t i) { return convert(rvalue(callArgs[i]), valueTy, at).text; };
      std::string r = newTemp();
      if (op == "load")
      {
        fn += "  " + r + " = load atomic " + ty + ", ptr " + p + " seq_cst, align " + align + "\n";
        return Value{r, valueTy};
      }
      if (op == "store")
      {
        fn += "  store atomic " + ty + " " + arg(1) + ", ptr " + p + " seq_cst, align " + align + "\n";
        return Value{"", types.voidType()};
      }
      if (op == "add" || op == "exchange")
      {
        fn += "  " + r + " = atomicrmw " + (op == "add" ? "add" : "xchg") + " ptr " + p + ", " + ty + " " + arg(1) + " seq_cst, align " + align + "\n";
        return Value{r, valueTy};
      }
      if (op == "cas")
      {
        std::string exchange = arg(1), comparand = arg(2), pair = newTemp();
        fn += "  " + pair + " = cmpxchg ptr " + p + ", " + ty + " " + comparand + ", " + ty + " " + exchange + " seq_cst seq_cst, align " + align + "\n";
        fn += "  " + r + " = extractvalue { " + ty + ", i1 } " + pair + ", 0\n";
        return Value{r, valueTy};
      }
      unsupported(at, "the atomic builtin " + name);
    }

    Value callFunction(FunctionDecl* f, const TemplateArgs& args, const Ty* classTy, const std::string& thisArg,
                       const std::string& sret, Span<Expr*> callArgs, Node* at, const std::string& indirect = "")
    {
      if (f->flags & DF_Delete)
        unsupported(at, "a call to the deleted function " + global.qualifiedName(f));
      if (global.isExternC(f))
        if (std::optional<Value> v = atomicBuiltin(f, callArgs, at))
          return *v;
      if (f->flags & DF_Default)
        return defaultedCall(f, classTy, thisArg, callArgs, at);
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
      size_t provided = position - slot; // argument values emitted (pack expansions counted element by element)
      if (provided < f->params.size())
      {
        // Parameters the call left out take their default arguments, evaluated at the call site
        // (the expression's literals belong to the callee's file; a `caller` location to this one).
        // A member of a class template has them typed under the instantiation's arguments first
        // (`ScrollBar(const Style* s = &Policy::defaultStyle)` with Policy = VerticalPolicy).
        ParsedFile* callerFile = currentFile;
        bool retyped = false;
        for (size_t i = provided; i < f->params.size(); ++i)
        {
          auto* pd = static_cast<ParamDecl*>(f->params[i]);
          if (pd->isPack)
            break;
          if (!pd->defaultValue)
            unsupported(at, "a call to " + global.qualifiedName(f) + " without an argument for its parameter " +
                              (pd->name != UINT32_MAX ? sym(pd->name) : std::to_string(i + 1)) + " (and the overload resolution let it through)");
          if (!retyped && !args.empty() && f != currentFunction)
          {
            global.retypeFor(f, args, classTy);
            retyped = true;
          }
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
      bool mayThrow = !global.isExternC(f); // C functions do not throw Kov exceptions
      if (s.returnType == "void")
      {
        emitCall("", sig + name + "(" + argText + ")", mayThrow);
        if (s.hasSret)
          return {resultAddr, ret, sret.empty()};
        return {"", types.voidType(), false};
      }
      std::string r = newTemp();
      emitCall(r + " = ", sig + name + "(" + argText + ")", mayThrow);
      return {r, ret, false};
    }

    // A member declared `= default`: the memberwise constructor, destructor, assignment or
    // comparison the compiler writes for it, on the object at thisArg.
    Value defaultedCall(FunctionDecl* f, const Ty* classTy, const std::string& thisArg, Span<Expr*> args, Node* at)
    {
      if (!classTy || thisArg.empty())
        unsupported(at, "a defaulted function without its object");
      const Ty* t = types.unqualified(types.stripRef(classTy));
      std::string src;
      if (!args.empty())
      {
        if (args.size() != 1)
          unsupported(at, "a defaulted function with " + std::to_string(args.size()) + " arguments");
        Value v = rvalue(args[0]);
        if (!isClass(v.ty))
        {
          // A Vector value being assigned/copied into a class that wraps it (e.g. float32x4
          // wrapping __m128): store the vector directly into the wrapper's first field and return.
          if (v.ty && v.ty->kind == TyKind::Vector && isClass(t))
          {
            StructInfo& info = structFor(t, at);
            std::string field0 = gep(info.name, thisArg, 0);
            std::string tmp = newTemp();
            fn += "  " + tmp + " = load " + llType(v.ty, at) + ", ptr " + v.text + "\n";
            fn += "  store " + llType(v.ty, at) + " " + tmp + ", ptr " + field0 + "\n";
            bool isAssign = (f->flags & DF_Constructor) == 0;
            if (isAssign)
              return {thisArg, classTy, false};
            return {"", types.voidType(), false};
          }
          // For defaulted comparisons (operator==, operator<=>), convert the scalar to
          // a temporary of the class type through its converting constructor, e.g. `fixedPoint == 0`.
          if (!(f->flags & (DF_Constructor | DF_Destructor)) && isClass(t))
          {
            std::string tmp = newObjectSlot(t, at);
            Expr* one = args[0];
            constructInto(tmp, t, Span<Expr*>{&one, 1}, at);
            src = tmp;
          }
          else
            unsupported(at, "a defaulted function given a non-class");
        }
        if (src.empty())
        {
          src = v.text;
          if (classKey(v.ty) != classKey(t) && !baseAddress(v.ty, t, v.text, src, at))
            unsupported(at, "a defaulted function given an unrelated class");
        }
      }
      return defaultedOperation(f, t, thisArg, src, at);
    }

    // The memberwise body of a member declared `= default`, on the object at thisArg (and the one
    // at src: the other operand of a copy, move, assignment or comparison).
    Value defaultedOperation(FunctionDecl* f, const Ty* t, const std::string& thisArg, const std::string& src, Node* at)
    {
      bool move = false;
      if (!f->params.empty())
      {
        TemplateArgs cargs = global.classArgsOf(canonicalClass(t, at));
        ArgsScope scope(*this, &cargs);
        const Ty* pt = declType(f->params[0]);
        move = pt && pt->kind == TyKind::RRef;
      }
      if (f->flags & DF_Constructor)
      {
        if (src.empty())
        {
          fn += "  store " + llType(t, at) + " zeroinitializer, ptr " + thisArg + "\n";
          initializeMembers(thisArg, t, {}, at);
          return {"", types.voidType(), false};
        }
        memberwiseConstruct(thisArg, src, t, at, move);
        return {"", types.voidType(), false};
      }
      if (f->flags & DF_Destructor)
      {
        destroyMembers(thisArg, t, at);
        return {"", types.voidType(), false};
      }
      if (!f->name.empty() && f->name.last().special == 2 && !src.empty())
      {
        P op = P(f->name.last().opToken);
        if (op == P::Assign)
        {
          memberwiseAssign(thisArg, src, t, at, move);
          return {thisArg, types.lref(t), false};
        }
        if (op == P::EqEq || op == P::NotEq)
        {
          Value eq = memberwiseEqual(thisArg, src, t, at);
          if (op == P::EqEq)
            return eq;
          std::string r = newTemp();
          fn += "  " + r + " = xor i1 " + eq.text + ", true\n";
          return {r, types.boolType(), false};
        }
        if (op == P::Spaceship)
        {
          // `operator<=>(const T&) const = default`: the order of the bases and members as the
          // value of `a <=> b` (inlined at a call; emitFunction moves it to the result slot).
          return orderingObject(memberwiseOrder(thisArg, src, t, at), at);
        }
      }
      unsupported(at, "the defaulted function " + global.qualifiedName(f));
    }

    // a == b for the objects at those addresses, base by base and member by member.
    Value memberwiseEqual(const std::string& a, const std::string& b, const Ty* t, Node* at)
    {
      StructInfo info = structFor(t, at);
      std::string result = "true";
      auto combine = [&](const std::string& v)
      {
        if (result == "true")
        {
          result = v;
          return;
        }
        std::string r = newTemp();
        fn += "  " + r + " = and i1 " + result + ", " + v + "\n";
        result = r;
      };
      for (auto& [baseDef, index] : info.bases)
        combine(objectsEqual(gep(info.name, a, index), gep(info.name, b, index), info.fieldTypes[index], at).text);
      for (Decl* d : info.fieldDecls)
      {
        uint32_t index = info.fieldIndex[d];
        const Ty* ft = info.fieldTypes[index];
        std::string fa = gep(info.name, a, index), fb = gep(info.name, b, index);
        if (isClass(ft))
          combine(objectsEqual(fa, fb, ft, at).text);
        else if (ft->kind == TyKind::Array)
          unsupported(at, "a defaulted comparison of an array member");
        else
          combine(compare(P::EqEq, load(fa, ft, at), load(fb, ft, at), at).text);
      }
      return {result, types.boolType(), false};
    }

    // a == b for class objects: the class's own operator== unless it is defaulted, else memberwise.
    Value objectsEqual(const std::string& a, const std::string& b, const Ty* t, Node* at)
    {
      const Ty* foundIn = nullptr;
      std::vector<const Ty*> argTypes{types.lref(types.qualified(t, true, false))};
      FunctionDecl* op = findMemberFunction(t, "operator==", argTypes, t, at, &foundIn);
      if (op && !(op->flags & DF_Default))
      {
        TemplateArgs args = global.classArgsOf(foundIn);
        Signature s;
        {
          ArgsScope scope(*this, &args);
          s = signatureOf(op);
        }
        std::string name = useFunction(op, args, foundIn);
        std::string thisArg = a;
        if (canonicalClass(t, at) != foundIn)
          baseAddress(t, foundIn, a, thisArg, at);
        std::string r = newTemp();
        emitCall(r + " = ", s.returnType + " " + name + "(ptr " + thisArg + ", ptr " + b + ")");
        if (s.returnType == "i1")
          return {r, types.boolType(), false};
        return convert(Value{r, types.intType(32), false}, types.boolType(), at);
      }
      return memberwiseEqual(a, b, t, at);
    }

    // One call argument as "<type> <operand>", converted to the parameter type (null: variadic).
    std::string argumentText(const Ty* pt, Expr* arg, Node* at)
    {
      if (pt && pt->isReference())
      {
        if (isAggregate(types.stripRef(pt)))
        {
          const Ty* cls = types.stripRef(pt);
          if (arg->kind == NodeKind::BraceInitExpr && isInitializerListType(cls))
            return "ptr " + initializerListTemporary(cls, static_cast<BraceInitExpr*>(arg), at); // `max({a, b, c})`
          if (arg->kind == NodeKind::BraceInitExpr && isClass(cls))
            return "ptr " + convertedTemporary(cls, arg, at); // `f({a, b, c})` with `f(const Rect&)`
          const Ty* argTy = arg->ty && arg->ty->isKnown() && types.stripRef(arg->ty)->isKnown() && !types.isDependent(arg->ty) ? types.stripRef(arg->ty) : nullptr;
          if (argTy && isClass(cls) && !isClass(argTy))
            return "ptr " + convertedTemporary(cls, arg, at); // `f("text")` with `f(const String&)`, a lambda with `f(const Function<void()>&)`
          if (argTy && isClass(cls) && isClass(argTy) && classKey(argTy) != classKey(cls) && !baseInstantiation(argTy, classDef(cls->decl, at), at))
          {
            if (conversionOperator(argTy, types.unqualified(cls), at))
              return "ptr " + convert(rvalue(arg), types.unqualified(cls), at).text; // `f(lowercase)` through `operator const String&()`
            if (findConstructor(cls, {arg->ty}, at))
              return "ptr " + convertedTemporary(cls, arg, at); // `f(view)` with `f(const String&)`
          }
          Value o = rvalue(arg);
          if (!isAggregate(o.ty))
            unsupported(at, "binding a non-class to a class reference");
          std::string addr = o.text;
          if (isClass(o.ty) && classKey(o.ty) != classKey(pt))
            if (!baseAddress(o.ty, types.stripRef(pt), o.text, addr, at))
              unsupported(at, "binding an unrelated class to a reference");
          return "ptr " + addr;
        }
        const Ty* referenced = types.stripRef(pt);
        if (referenced->kind == TyKind::Pointer && referenced->inner && referenced->inner->kind == TyKind::Function && arg->ty &&
            isLambda(types.stripRef(arg->ty)))
        {
          // `Pair(key, lambda)` with `Pair(const K&, const Fn*&)`: the function pointer, in a slot
          // the reference can bind to.
          Value fp = convert(rvalue(arg), referenced, at);
          std::string slot = "%v" + std::to_string(temp++);
          allocas += "  " + slot + " = alloca ptr\n";
          fn += "  store ptr " + fp.text + ", ptr " + slot + "\n";
          return "ptr " + slot;
        }
        return "ptr " + lvalue(arg);
      }
      if (pt && isAggregate(pt))
      {
        if (arg->kind == NodeKind::BraceInitExpr && isInitializerListType(pt))
          return "ptr " + initializerListTemporary(pt, static_cast<BraceInitExpr*>(arg), at); // `max({a, b, c})`
        if (arg->kind == NodeKind::BraceInitExpr && isClass(pt))
        {
          std::string copy = newObjectSlot(pt, at);
          constructInto(copy, pt, static_cast<BraceInitExpr*>(arg)->args, at); // `f({a, b, c})`; the callee destroys it
          return "ptr " + copy;
        }
        const Ty* argTy = arg->ty && arg->ty->isKnown() && !types.isDependent(arg->ty) ? types.stripRef(arg->ty) : nullptr;
        if (argTy && isClass(pt) && (!isClass(argTy) || (classKey(argTy) != classKey(pt) &&
                                                         !baseInstantiation(argTy, classDef(pt->decl, at), at) &&
                                                         !conversionOperator(argTy, types.unqualified(pt), at))))
        {
          // Through a converting constructor; the callee destroys the parameter.
          std::string copy = newObjectSlot(pt, at);
          Expr* one = arg;
          constructInto(copy, pt, Span<Expr*>{&one, 1}, at);
          return "ptr " + copy;
        }
        Value v = rvalue(arg);
        if (!isAggregate(v.ty))
        {
          // A Vector value being passed to a class parameter that wraps it (e.g. float32x4
          // wrapping __m128): construct the wrapper and store the vector into its first field.
          if (v.ty && v.ty->kind == TyKind::Vector && isClass(pt))
          {
            std::string copy = newObjectSlot(pt, at);
            fn += "  store " + llType(pt, at) + " zeroinitializer, ptr " + copy + "\n";
            StructInfo& info = structFor(pt, at);
            std::string field0 = gep(info.name, copy, 0);
            std::string tmp = newTemp();
            fn += "  " + tmp + " = load " + llType(v.ty, at) + ", ptr " + v.text + "\n";
            fn += "  store " + llType(v.ty, at) + " " + tmp + ", ptr " + field0 + "\n";
            return "ptr " + copy;
          }
          unsupported(at, "passing a non-class to a class parameter");
        }
        if (isClass(v.ty) && isClass(pt) && classKey(v.ty) != classKey(pt))
          v = convert(v, types.unqualified(pt), at); // a base, or a conversion operator
        std::string copy = newObjectSlot(pt, at);
        if (isLambda(pt))
        {
          std::string tmp = newTemp();
          fn += "  " + tmp + " = load " + llType(pt, at) + ", ptr " + v.text + "\n";
          fn += "  store " + llType(pt, at) + " " + tmp + ", ptr " + copy + "\n";
        }
        else if (v.isTemporary)
          moveTemporary(copy, v, pt, at);
        else if (isXvalue(arg))
          moveConstruct(copy, v.text, pt, at); // `f(move(ptr))` with `f(UniquePtr<T> ptr)`
        else
          copyConstruct(copy, v.text, pt, at);
        return "ptr " + copy;
      }
      Value a = rvalue(arg);
      if (isClass(a.ty) && pt && !isAggregate(pt) && conversionOperator(a.ty, types.unqualified(pt), at))
        a = convert(a, types.unqualified(pt), at); // `draw(image)` with `operator const Image*()`
      if (isLambda(a.ty) && pt && pt->kind == TyKind::Pointer && pt->inner && pt->inner->kind == TyKind::Function)
        a = convert(a, pt, at); // a lambda without captures as a pointer to function
      if (isAggregate(a.ty))
        unsupported(at, "passing a class (" + types.toString(a.ty, &symbols) + ") where a scalar is expected");
      if (pt && a.ty->kind == TyKind::Pointer && (pt->kind == TyKind::Int || pt->kind == TyKind::Char || pt->kind == TyKind::Enum))
        unsupported(at, "passing a pointer (" + types.toString(a.ty, &symbols) + ") where " + types.toString(pt, &symbols) + " is expected");
      if (pt && pt->kind == TyKind::Pointer && (a.ty->kind == TyKind::Int || a.ty->kind == TyKind::Char || a.ty->kind == TyKind::Enum))
        unsupported(at, "passing " + types.toString(a.ty, &symbols) + " where a pointer is expected");
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

    // A temporary of class type cls built from a non-class value through a converting
    // constructor, alive until the end of the full expression: what a reference parameter binds to.
    std::string convertedTemporary(const Ty* cls, Expr* arg, Node* at)
    {
      const Ty* source = arg->ty ? types.stripRef(arg->ty) : nullptr;
      if (arg->kind != NodeKind::BraceInitExpr && (!source || !source->isKnown()))
        unsupported(at, "converting a value of unknown type to " + types.toString(cls, &symbols)); // (not the copy constructor, again and again)
      Expr* one = arg;
      // The resolver already enforces explicit-ness during overload resolution; codegen trusts it.
      std::string tmp = newObjectSlot(cls, at);
      constructInto(tmp, cls, Span<Expr*>{&one, 1}, at);
      if (needsDestruction(cls, at))
        statementTemps.push_back({tmp, cls});
      return tmp;
    }

    // --- constants ---

    std::string_view spelling(uint32_t token) const
    {
      const std::vector<Token>& toks = *currentFile->tokens;
      const Token& t = toks[std::min<size_t>(token, toks.size() - 1)];
      return currentFile->text.substr(t.offset, t.length);
    }

    // `(Name)` where Name is a type: a C-style cast the parser could not tell from a parenthesis.
    bool isParenthesizedTypeName(Expr* e)
    {
      if (!e || e->kind != NodeKind::ParenExpr)
        return false;
      Expr* inner = static_cast<ParenExpr*>(e)->inner;
      if (!inner || inner->kind != NodeKind::NameExpr)
        return false;
      Decl* d = static_cast<NameExpr*>(inner)->resolved;
      return d && (d->kind == NodeKind::ClassDecl || d->kind == NodeKind::EnumDecl || d->kind == NodeKind::UsingAliasDecl ||
                   (d->kind == NodeKind::VariableDecl && (d->flags & DF_Typedef)));
    }

    // The arguments of a variable template's use (`canBuild<String, const char*>`): the written
    // ones, a trailing pack taking the rest.
    TemplateArgs variableTemplateArgs(NameExpr* n, VariableDecl* v, Node* at)
    {
      TemplateArgs args, none;
      const NamePart& last = n->name.last();
      std::vector<const Ty*> packElements;
      size_t count = v->templateParams.size();
      auto* lastParam = count ? static_cast<TemplateParamDecl*>(v->templateParams[count - 1]) : nullptr;
      for (size_t i = 0; i < last.templateArgs.size(); ++i)
      {
        Node* arg = last.templateArgs[i];
        const Ty* t = nullptr;
        if (uint8_t(arg->kind) <= uint8_t(NodeKind::PackType))
          t = global.typeOfAstIn(static_cast<Type*>(arg), currentFunction, currentArgs ? *currentArgs : none);
        else if (std::optional<int64_t> c = evalConst(static_cast<Expr*>(arg)))
          t = types.valueType(types.intType(), *c);
        if (!t || !t->isKnown())
          unsupported(at, "an argument of " + global.qualifiedName(v) + " that is not a type or a constant");
        if (lastParam && lastParam->isPack && i + 1 >= count)
          packElements.push_back(t);
        else if (i < count)
          args.pairs.push_back({v->templateParams[i], t});
        else
          unsupported(at, "more arguments than " + global.qualifiedName(v) + " has parameters");
      }
      if (lastParam && lastParam->isPack)
        args.pairs.push_back({lastParam, types.pack(Span<const Ty*>{packElements.data(), uint32_t(packElements.size())})});
      // Fill in default arguments for parameters not covered by the written arguments.
      for (size_t i = args.pairs.size(); i < count; ++i)
      {
        auto* param = static_cast<TemplateParamDecl*>(v->templateParams[i]);
        if (param->defaultArg)
        {
          const Ty* t = nullptr;
          if (param->isType && uint8_t(param->defaultArg->kind) <= uint8_t(NodeKind::PackType))
            t = global.typeOfAstIn(static_cast<Type*>(param->defaultArg), currentFunction, currentArgs ? *currentArgs : none);
          else if (std::optional<int64_t> c = evalConst(static_cast<Expr*>(param->defaultArg)))
            t = types.valueType(types.intType(), *c);
          if (t && t->isKnown())
          {
            args.pairs.push_back({param, t});
            continue;
          }
        }
        unsupported(at, "the variable template " + global.qualifiedName(v) + " used without all its arguments");
      }
      return args;
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
            for (const char* s : {"i64", "i32", "i16", "i8"}) // MSVC suffixes: 1i64, 1ui64
              if (digits.size() > strlen(s) && digits.compare(digits.size() - strlen(s), strlen(s), s) == 0)
                digits.resize(digits.size() - strlen(s));
            while (!digits.empty() && strchr("uUlLzZ", digits.back()))
              digits.pop_back();
            try
            {
              if (digits.size() > 2 && digits[0] == '0' && (digits[1] == 'b' || digits[1] == 'B'))
                return int64_t(std::stoull(digits.substr(2), nullptr, 2)); // 0b1010
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
        case NodeKind::RequiresExpr: return static_cast<RequiresExpr*>(e)->satisfied ? 1 : 0; // typed for this instantiation
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
          // Short-circuit: `true || <unknown>` is 1, `false && <unknown>` is 0.
          if (l && P(b->op) == P::OrOr && *l)
            return 1;
          if (l && P(b->op) == P::AndAnd && !*l)
            return 0;
          auto r = evalConst(b->rhs, depth + 1);
          if (l && r && P(b->op) == P::OrOr)
            return (*l || *r) ? 1 : 0;
          if (l && r && P(b->op) == P::AndAnd)
            return (*l && *r) ? 1 : 0;
          if (!l && r && !b->isKeyword && (P(b->op) == P::Minus || P(b->op) == P::Plus) && isParenthesizedTypeName(b->lhs))
            return P(b->op) == P::Minus ? -*r : *r; // `(DWORD)-11` from a C header, parsed as a subtraction
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
        case NodeKind::CallExpr:
        {
          // `cells()`: a function whose body is one `return expression;`, evaluated with the
          // arguments bound to its parameters.
          auto* c = static_cast<CallExpr*>(e);
          if (!c->callee || c->callee->kind != NodeKind::NameExpr)
            return std::nullopt;
          Decl* d = static_cast<NameExpr*>(c->callee)->resolved;
          if (!d)
            return std::nullopt;
          if (d->kind == NodeKind::ClassDecl && c->args.empty())
          {
            // `is_optional<T>()`: a zero-arg constructor of a type-trait struct used as bool.
            // Walk its hierarchy for `static constexpr bool value`.
            // Walk the class and its bases (through AST base list) for `static constexpr bool value`.
            auto findValue = [this](ClassDecl* cls, auto&& self, int d) -> VariableDecl* {
              if (!cls || d > 8) return nullptr;
              for (Decl* m : cls->members)
                if (m->kind == NodeKind::VariableDecl && (m->flags & DF_Static) && (m->flags & DF_Constexpr) &&
                    static_cast<VariableDecl*>(m)->init && !static_cast<VariableDecl*>(m)->name.empty())
                {
                  auto& last = static_cast<VariableDecl*>(m)->name.last();
                  if (std::string_view sv = symbols.name(last.symbol); sv == "value")
                    return static_cast<VariableDecl*>(m);
                }
              // Walk base classes via the AST base list (Scope is incomplete in codegen)
              for (auto& base : cls->bases)
                if (base.type && base.type->kind == NodeKind::NamedType)
                {
                  Decl* bd = static_cast<NamedType*>(base.type)->resolved;
                  if (bd && bd->kind == NodeKind::ClassDecl)
                    if (auto* v = self(static_cast<ClassDecl*>(bd), self, d + 1))
                      return v;
                }
              return nullptr;
            };
            if (VariableDecl* v = findValue(static_cast<ClassDecl*>(d), findValue, 0))
              return evalConst(v->init, depth + 1);
            return std::nullopt;
          }
          if (d->kind != NodeKind::FunctionDecl)
            return std::nullopt;
          auto* f = static_cast<FunctionDecl*>(d);
          if (!f->body || f->body->kind != NodeKind::BlockStmt || f->params.size() != c->args.size())
            return std::nullopt;
          auto* block = static_cast<BlockStmt*>(f->body);
          if (block->stmts.size() != 1 || !block->stmts[0] || block->stmts[0]->kind != NodeKind::ReturnStmt)
            return std::nullopt;
          std::vector<std::pair<Decl*, int64_t>> bound;
          for (size_t i = 0; i < c->args.size(); ++i)
          {
            auto v = evalConst(c->args[i], depth + 1);
            if (!v)
              return std::nullopt;
            bound.push_back({f->params[i], *v});
          }
          size_t mark = constEnv.size();
          constEnv.insert(constEnv.end(), bound.begin(), bound.end());
          ParsedFile* saved = currentFile;
          currentFile = f->file ? f->file : currentFile;
          auto r = evalConst(static_cast<ReturnStmt*>(block->stmts[0])->value, depth + 1);
          currentFile = saved;
          constEnv.resize(mark);
          return r;
        }
        case NodeKind::NameExpr:
        {
          auto* n = static_cast<NameExpr*>(e);
          Decl* d = n->resolved;
          if (d && d->kind == NodeKind::ParamDecl)
          {
            for (size_t i = constEnv.size(); i-- > 0;)
              if (constEnv[i].first == d)
                return constEnv[i].second;
            return std::nullopt;
          }
          if (!d)
          {
            // A builtin type trait, evaluated under the current instantiation.
            TemplateArgs none;
            if (std::optional<bool> trait = global.typeTraitValue(n, constContext ? constContext : currentFunction, currentArgs ? *currentArgs : none))
              return *trait ? 1 : 0;
            return std::nullopt;
          }
          if (d->kind == NodeKind::EnumeratorDecl)
            return enumeratorValue(static_cast<EnumeratorDecl*>(d));
          if (d->kind == NodeKind::TemplateParamDecl)
          {
            // A non-type template parameter: its value in the instantiation being emitted.
            if (currentArgs)
              for (auto& [p, t] : currentArgs->pairs)
                if (p == d && t && t->kind == TyKind::Value)
                  return t->value;
            return std::nullopt;
          }
          if (d->kind == NodeKind::VariableDecl)
          {
            auto* v = static_cast<VariableDecl*>(d);
            if (v->builtin == BUILTIN_EnumCount && v->parent)
              return int64_t(enumeratorCount(v->parent));
            const Ty* t = declType(v);
            VariableDecl* def = locals.count(d) ? v : variableDefinition(v);
            if (t && (t->isConst || (v->flags & DF_Constexpr)) && def->init && !locals.count(d))
            {
              ParsedFile* saved = currentFile;
              ConstContextScope context(*this, def);
              currentFile = def->file;
              std::optional<int64_t> r;
              if (!v->templateParams.empty())
              {
                // `bytes<double>`, `canBuild<String, const char*>`: the variable template's
                // initializer for these arguments.
                TemplateArgs args = variableTemplateArgs(n, v, e);
                global.retypeInitializerFor(def, args);
                ArgsScope scope(*this, &args);
                r = evalConst(def->init, depth + 1);
              }
              else
                r = evalConst(def->init, depth + 1);
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
            t = currentArgs ? global.typeOfAstIn(s->type, constContext ? constContext : currentFunction, *currentArgs)
                              : global.typeOfAst(s->type, constContext ? constContext : currentFunction); // `sizeof(T)` in a variable template's initializer
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
      std::string_view first = spelling(l->token);
      bool wide16 = first.starts_with("L") || (first.starts_with("u") && !first.starts_with("u8"));
      bool wide32 = first.starts_with("U");
      if (wide16 || wide32)
        return wideStringConstant(bytes, wide16 ? 16 : 32);
      return cStringConstant(bytes);
    }

    // `L"text"` / `U"text"`: the UTF-8 bytes decoded and re-encoded as UTF-16 or UTF-32 code units.
    std::string wideStringConstant(const std::string& utf8, int bits)
    {
      std::vector<uint32_t> points;
      for (size_t i = 0; i < utf8.size();)
      {
        unsigned char c = utf8[i];
        uint32_t cp;
        size_t n;
        if (c < 0x80) { cp = c; n = 1; }
        else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; n = 2; }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; n = 3; }
        else { cp = c & 0x07; n = 4; }
        for (size_t k = 1; k < n && i + k < utf8.size(); ++k)
          cp = (cp << 6) | (utf8[i + k] & 0x3F);
        points.push_back(cp);
        i += n;
      }
      std::vector<uint32_t> units;
      for (uint32_t cp : points)
      {
        if (bits == 16 && cp >= 0x10000)
        {
          cp -= 0x10000;
          units.push_back(0xD800 + (cp >> 10));
          units.push_back(0xDC00 + (cp & 0x3FF));
        }
        else
          units.push_back(cp);
      }
      units.push_back(0);
      std::string& out = emittingShared ? decls : moduleDecls[currentModule];
      std::string name = "@.wstr" + std::to_string(stringCount++);
      std::string unit = bits == 16 ? "i16" : "i32", elements;
      for (size_t i = 0; i < units.size(); ++i)
        elements += std::string(i ? ", " : "") + unit + " " + std::to_string(units[i]);
      out += name + " = private unnamed_addr constant [" + std::to_string(units.size()) + " x " + unit + "] [" + elements + "]\n";
      return name;
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
      if (info.primaryBase)
        useVtable(structFor(info.primaryBase, nullptr)); // the descriptor chain refers to the base's descriptor
    }

    // The name a class descriptor (slot 0 of a vtable) starts with.
    std::string descriptorName(const std::string& descriptor)
    {
      std::string n = newTemp();
      fn += "  " + n + " = load ptr, ptr " + descriptor + "\n";
      return n;
    }

    // dynamic_cast<T*>(p) / dynamic_cast<T&>(x) between polymorphic classes: the object's
    // descriptor chain is searched for T's; null (for a pointer) when T is not among them.
    // Upcasts and non-polymorphic operands fall back to the static conversion (nullopt).
    std::optional<Value> dynamicCast(NamedCastExpr* c, Expr* e)
    {
      const Ty* target = types.stripRef(e->ty);
      if (!target || !target->isKnown())
        return std::nullopt;
      bool pointer = target->kind == TyKind::Pointer;
      const Ty* tcls = pointer ? target->inner : target;
      if (!isClass(tcls))
        return std::nullopt;
      Value o = rvalue(c->operand);
      const Ty* scls = o.ty && o.ty->kind == TyKind::Pointer ? o.ty->inner : types.stripRef(o.ty);
      if (!isClass(scls) || !structFor(scls, e).polymorphic || !structFor(tcls, e).polymorphic)
        return std::nullopt;
      std::vector<std::pair<std::string, uint32_t>> path;
      if (classKey(scls) == classKey(tcls) || basePath(scls, tcls, path, e))
        return std::nullopt; // the same class or a base of it: the static conversion
      StructInfo& ti = structFor(tcls, e);
      useVtable(ti);
      if (!dyncastEmitted)
      {
        dyncastEmitted = true;
        decls += "$kov.dyncast = comdat any\n"
                 "define linkonce_odr ptr @kov.dyncast(ptr %obj, ptr %target) comdat {\n"
                 "entry:\n  %isnull = icmp eq ptr %obj, null\n  br i1 %isnull, label %fail, label %start\n"
                 "start:\n  %vt = load ptr, ptr %obj\n  %d0 = load ptr, ptr %vt\n  br label %loop\n"
                 "loop:\n  %d = phi ptr [ %d0, %start ], [ %next, %step ]\n  %hit = icmp eq ptr %d, %target\n  br i1 %hit, label %ok, label %step\n"
                 "step:\n  %bp = getelementptr inbounds { ptr, ptr }, ptr %d, i32 0, i32 1\n  %next = load ptr, ptr %bp\n"
                 "  %end = icmp eq ptr %next, null\n  br i1 %end, label %fail, label %loop\n"
                 "ok:\n  ret ptr %obj\n"
                 "fail:\n  ret ptr null\n}\n";
      }
      std::string r = newTemp();
      fn += "  " + r + " = call ptr @kov.dyncast(ptr " + o.text + ", ptr " + ti.typeInfoName + ")\n";
      return Value{r, e->ty, false};
    }

    // A floating-point constant in IR: the exact bits, rounded through float for 32-bit values.
    static std::string floatConstant(double value, uint8_t bits)
    {
      double exact = bits == 32 ? double(float(value)) : value;
      uint64_t raw;
      memcpy(&raw, &exact, sizeof raw);
      char buf[32];
      snprintf(buf, sizeof buf, "0x%016llX", (unsigned long long)raw);
      return buf;
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
    // The shared declarations without those of functions this module defines: a `declare` next to
    // the `define` is a redefinition to LLVM (a call site whose definition was not found declares
    // the function as external).
    std::string declsFor(unsigned k)
    {
      std::string out;
      size_t pos = 0;
      while (pos < decls.size())
      {
        size_t end = decls.find('\n', pos);
        if (end == std::string::npos)
          end = decls.size();
        std::string_view line(decls.data() + pos, end - pos);
        bool skip = false;
        if (line.rfind("declare ", 0) == 0)
        {
          size_t at = line.find('@');
          if (at != std::string_view::npos)
          {
            size_t nameEnd = line[at + 1] == '"' ? line.find('"', at + 2) + 1 : line.find('(', at);
            if (nameEnd != std::string_view::npos)
            {
              auto it = definedIn.find(std::string(line.substr(at, nameEnd - at)));
              skip = it != definedIn.end() && it->second == k;
            }
          }
        }
        if (!skip)
        {
          out.append(line);
          out += '\n';
        }
        pos = end + 1;
      }
      return out;
    }

    std::string moduleText(unsigned k, const std::string& prefix)
    {
      std::string text = moduleDecls[k] + bodies[k] + shared;
      std::string decls = declsFor(k);
      std::string already = decls + moduleDecls[k]; // declarations every module (or this one) carries
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
        if (it != definedIn.end() && it->second != k && already.find(name + "(") == std::string::npos)
          declares += declareLines[name] + "\n"; // not when a declaration is in the module's text already
      }
      return prefix + decls + moduleDecls[k] + ctorsText(k) + "\n" + shared + declares + "\n" + bodies[k];
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

    // The class's `operator T()` converting to `to` (exactly, or any scalar one when none is exact);
    // explicit ones only for bool, where conditions allow them.
    FunctionDecl* conversionOperator(const Ty* from, const Ty* to, Node* at)
    {
      ClassDecl* c = classDef(from->decl, at);
      TemplateArgs args = global.classArgsOf(canonicalClass(from, at));
      ArgsScope scope(*this, &args);
      FunctionDecl* fallback = nullptr;
      for (Decl* m : c->members)
      {
        if (m->kind != NodeKind::FunctionDecl || !(m->flags & DF_Conversion))
          continue;
        if (global.isExplicit(static_cast<FunctionDecl*>(m), args) && to->kind != TyKind::Bool)
          continue;
        const Ty* ft = declType(m);
        if (!ft || ft->kind != TyKind::Function || !ft->inner || !ft->inner->isKnown())
          continue;
        const Ty* ret = types.unqualified(types.stripRef(ft->inner));
        if (ret == types.unqualified(to))
          return static_cast<FunctionDecl*>(m);
        if (!fallback && !isClass(ret))
          fallback = static_cast<FunctionDecl*>(m);
      }
      return fallback;
    }

    // A class's (non-explicit) conversion operator to an enum or an integer, or null: what a
    // switch on the object switches on.
    FunctionDecl* integralConversionOperator(const Ty* from, Node* at)
    {
      ClassDecl* c = classDef(from->decl, at);
      TemplateArgs args = global.classArgsOf(canonicalClass(from, at));
      ArgsScope scope(*this, &args);
      for (Decl* m : c->members)
      {
        if (m->kind != NodeKind::FunctionDecl || !(m->flags & DF_Conversion) || global.isExplicit(static_cast<FunctionDecl*>(m), args))
          continue;
        const Ty* ft = declType(m);
        if (!ft || ft->kind != TyKind::Function || !ft->inner || !ft->inner->isKnown())
          continue;
        const Ty* ret = types.unqualified(types.stripRef(ft->inner));
        if (ret->kind == TyKind::Enum || ret->kind == TyKind::Int || ret->kind == TyKind::Char || ret->kind == TyKind::Bool)
          return static_cast<FunctionDecl*>(m);
      }
      return nullptr;
    }

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
      if (isLambda(from) && tk == TyKind::Pointer && to->inner && to->inner->kind == TyKind::Function)
        return {lambdaThunk(from, to->inner, at), to}; // a lambda without captures as a pointer to function
      if (fk == TyKind::Class || tk == TyKind::Class)
      {
        if (fk == TyKind::Class && tk == TyKind::Class)
        {
          std::string addr;
          if (baseAddress(from, to, v.text, addr, at))
            return {addr, to, v.isTemporary && addr == v.text};
        }
        if (fk == TyKind::Class)
          if (FunctionDecl* op = conversionOperator(from, to, at))
          {
            TemplateArgs args = global.classArgsOf(canonicalClass(from, at));
            return convert(callRvalue(callFunction(op, args, canonicalClass(from, at), v.text, "", {}, at), at), to, at);
          }
        unsupported(at, "a conversion from " + types.toString(from, &symbols) + " to " + types.toString(to, &symbols));
      }
      std::string r = newTemp();
      std::string fromT = llType(from, at), toT = llType(to, at);
      if (fromT == toT && !(fk == TyKind::Pointer && tk == TyKind::Pointer))
        return {v.text, to, v.isTemporary}; // the same bits: bool to `enum class E : bool`, uint8 to an int8 enum
      // (two class pointers are both `ptr`, yet Derived* -> Base* may move to the base subobject)
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
            return {keepNull(v.text, addr), to};
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
      // SSE vector to scalar: extract lane 0.
      // __m128 -> float, __m128d -> double, __m128i -> int32/int64.
      if (fk == TyKind::Vector && (tk == TyKind::Float || tk == TyKind::Int || tk == TyKind::Char))
      {
        // If element types differ, bitcast the vector first.
        const Ty* elemTy = from->inner;
        if (elemTy && elemTy->kind != tk)
        {
          // e.g. <4 x i32> -> double: bitcast <4 x i32> to <2 x double> then extract.
          // or <4 x float> -> int32: bitcast <4 x float> to <4 x i32> then extract.
          uint32_t totalBits = from->count * (elemTy->bits ? elemTy->bits : 32);
          uint32_t newCount = totalBits / to->bits;
          const Ty* newVec = types.vectorType(to, newCount);
          std::string t1 = newTemp();
          fn += "  " + t1 + " = bitcast " + fromT + " " + v.text + " to " + llType(newVec, at) + "\n";
          fn += "  " + r + " = extractelement " + llType(newVec, at) + " " + t1 + ", i32 0\n";
        }
        else
        {
          fn += "  " + r + " = extractelement " + fromT + " " + v.text + ", i32 0\n";
        }
        return {r, to};
      }
      // Scalar to SSE vector: splat (broadcast to all lanes).
      if (tk == TyKind::Vector && (fk == TyKind::Float || fk == TyKind::Int || fk == TyKind::Char || fk == TyKind::Bool))
      {
        const Ty* elemTy = to->inner;
        // Convert the scalar to the element type first if needed.
        Value scalar = (from != elemTy && types.unqualified(from) != types.unqualified(elemTy))
                        ? convert(v, elemTy, at) : v;
        std::string t1 = newTemp(), t2 = newTemp();
        fn += "  " + t1 + " = insertelement " + toT + " undef, " + llType(elemTy, at) + " " + scalar.text + ", i32 0\n";
        // Shuffle to splat lane 0 to all lanes.
        std::string mask = "<";
        for (uint32_t i = 0; i < to->count; ++i)
          mask += (i ? ", " : "") + std::string("i32 0");
        mask += ">";
        fn += "  " + t2 + " = shufflevector " + toT + " " + t1 + ", " + toT + " undef, <" + std::to_string(to->count) + " x i32> " + mask + "\n";
        return {t2, to};
      }
      // Vector to vector bitcast (same total size, different element types).
      if (fk == TyKind::Vector && tk == TyKind::Vector)
      {
        fn += "  " + r + " = bitcast " + fromT + " " + v.text + " to " + toT + "\n";
        return {r, to};
      }
      unsupported(at, "a conversion from " + types.toString(from, &symbols) + " to " + types.toString(to, &symbols));
    }

    // Address of the `base` subobject inside the `derived` object at addr (false if not a base).
    // An explicit cast: what convert does, plus the downcast to a derived class (pointer or
    // reference) that an implicit conversion never makes.
    Value explicitConvert(Value v, const Ty* to, Node* at)
    {
      const Ty* target = types.stripRef(to);
      const Ty* from = v.ty;
      bool pointers = from && target && from->kind == TyKind::Pointer && target->kind == TyKind::Pointer && isClass(from->inner) && isClass(target->inner);
      bool objects = isClass(from) && isClass(target);
      if (pointers || objects)
      {
        const Ty* fromCls = pointers ? from->inner : from;
        const Ty* toCls = pointers ? target->inner : target;
        std::vector<std::pair<std::string, uint32_t>> path;
        if (classKey(fromCls) != classKey(toCls) && basePath(toCls, fromCls, path, at))
        {
          std::string addr = derivedAddress(path, v.text);
          return {pointers ? keepNull(v.text, addr) : addr, pointers ? target : types.unqualified(toCls), false}; // `static_cast<VerticalFlow&>(layout)`
        }
      }
      return convert(v, to, at);
    }

    // A pointer moved to a base or derived subobject: null stays null (C++'s rule for pointer conversions).
    std::string keepNull(const std::string& original, const std::string& adjusted)
    {
      if (adjusted == original)
        return adjusted;
      std::string isNull = newTemp(), r = newTemp();
      fn += "  " + isNull + " = icmp eq ptr " + original + ", null\n";
      fn += "  " + r + " = select i1 " + isNull + ", ptr null, ptr " + adjusted + "\n";
      return r;
    }

    // The chain of base-class fields from derived down to base: (struct name, field index) pairs.
    bool basePath(const Ty* derived, const Ty* base, std::vector<std::pair<std::string, uint32_t>>& path, Node* at)
    {
      if (canonicalClass(derived, at) == canonicalClass(base, at))
        return true;
      StructInfo& info = structFor(derived, at);
      std::string name = info.name;
      std::vector<std::pair<Decl*, uint32_t>> bases = info.bases;
      std::vector<const Ty*> fieldTypes = info.fieldTypes;
      for (auto& [baseDef, index] : bases)
      {
        path.push_back({name, index});
        if (basePath(fieldTypes[index], base, path, at))
          return true;
        path.pop_back();
      }
      return false;
    }

    // The derived object's address from its base subobject's, back along the path of base fields.
    std::string derivedAddress(const std::vector<std::pair<std::string, uint32_t>>& path, const std::string& baseAddr)
    {
      if (path.empty())
        return baseAddr;
      std::string offset = "ptr null";
      for (auto& [name, index] : path)
        offset = "ptr getelementptr (" + name + ", " + offset + ", i32 0, i32 " + std::to_string(index) + ")";
      std::string off = newTemp(), neg = newTemp(), r = newTemp();
      fn += "  " + off + " = ptrtoint " + offset + " to i64\n";
      fn += "  " + neg + " = sub i64 0, " + off + "\n";
      fn += "  " + r + " = getelementptr i8, ptr " + baseAddr + ", i64 " + neg + "\n";
      return r;
    }

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
      if (ty->kind == TyKind::Array)
      {
        // A scalar stored into an array: zero-init the array, then store the scalar
        // converted to the element type in element 0 (C's `T a[N] = {value}` semantics).
        fn += "  store " + llType(ty, at) + " zeroinitializer, ptr " + addr + "\n";
        auto isNumeric = [](TyKind k) { return k == TyKind::Int || k == TyKind::Bool || k == TyKind::Char || k == TyKind::Float; };
        if (v.ty && ty->inner && isNumeric(v.ty->kind) && isNumeric(ty->inner->kind))
        {
          Value elem = convert(v, ty->inner, at);
          std::string elemAddr = newTemp();
          fn += "  " + elemAddr + " = getelementptr inbounds " + llType(ty, at) + ", ptr " + addr + ", i64 0, i64 0\n";
          fn += "  store " + llType(ty->inner, at) + " " + elem.text + ", ptr " + elemAddr + "\n";
        }
        return;
      }
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
          if (d->kind == NodeKind::VariableDecl)
            if (std::string bound; bindingAddress(static_cast<VariableDecl*>(d), e, bound))
              return bound;
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
            return globalAddress(static_cast<VariableDecl*>(d), e, qualifierClass(e, e));
          }
          if (d->kind == NodeKind::EnumeratorDecl)
            break; // an enumerator bound to a reference: its value goes to a temporary (below)
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
            Value p = arrowOperand(rvalue(m->base), e);
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
            return globalAddress(static_cast<VariableDecl*>(d), e, baseTy); // `object.staticMember`: the object's instantiation
          std::string addr;
          if (!fieldAddress(baseTy, d, object, addr, e))
            unsupported(e, "a member that is not a field of the object's class");
          if (const Ty* ft = global.typeOfDecl(d); ft && ft->isReference())
          {
            // A reference member: the field holds the address of what it refers to.
            std::string referenced = newTemp();
            fn += "  " + referenced + " = load ptr, ptr " + addr + "\n";
            return referenced;
          }
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
          if (ClassDecl* c = typedUnionOf(base.ty, e))
            return alternativeLvalue(c, base, i, e, true); // p[Integer]: the alternative, if held
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
        case NodeKind::CastExpr:
        {
          // `(T&&)value` in forward, `(Base&)derived`: a cast to a reference names the operand
          // itself; a copy in a temporary would be a dangling reference once returned.
          auto* c = static_cast<CastExpr*>(e);
          if (e->ty && e->ty->isReference())
            return lvalue(c->operand);
          break;
        }
        case NodeKind::NamedCastExpr:
        {
          auto* c = static_cast<NamedCastExpr*>(e);
          if (e->ty && e->ty->isReference() && c->castKind != Kw::dynamic_cast_)
            return lvalue(c->operand);
          break;
        }
        case NodeKind::BinaryExpr:
        {
          auto* b = static_cast<BinaryExpr*>(e);
          if (!b->isKeyword && (P(b->op) == P::DotStar || P(b->op) == P::ArrowStar))
            return memberPointerAddress(b);
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
        case NodeKind::ConditionalExpr:
        {
          auto* c = static_cast<ConditionalExpr*>(e);
          auto namesObject = [&](Expr* x) { return x && (isLvalueExpression(x) || (x->ty && x->ty->isReference())); };
          if (c->then && namesObject(c->then) && namesObject(c->else_))
          {
            // `first ? a : b` with two lvalues is an lvalue: the address of the chosen one.
            Value cond = convert(rvalue(c->cond), types.boolType(), e);
            std::string tL = newLabel("cond.then"), eL = newLabel("cond.else"), jL = newLabel("cond.end");
            fn += "  br i1 " + cond.text + ", label %" + tL + ", label %" + eL + "\n";
            size_t temps = statementTemps.size();
            startBlock(tL);
            std::string a = lvalue(c->then);
            destroyTempsFrom(temps);
            std::string aEnd = currentBlock;
            emitBranch(jL);
            startBlock(eL);
            std::string b = lvalue(c->else_);
            destroyTempsFrom(temps);
            std::string bEnd = currentBlock;
            emitBranch(jL);
            startBlock(jL);
            std::string r = newTemp();
            fn += "  " + r + " = phi ptr [ " + a + ", %" + aEnd + " ], [ " + b + ", %" + bEnd + " ]\n";
            return r;
          }
          break;
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

    // A global built at startup (a class with a constructor, or an initializer that is not a
    // constant): a function constructing it, registered in llvm.global_ctors of the module that
    // first uses the global. Order within a module follows first use; destructors at exit do not
    // run yet.
    void emitGlobalConstructor(VariableDecl* v, const std::string& name, const Ty* t, Node* at)
    {
      FunctionState saved = saveState();
      std::string savedOverride = funcletOverride;
      FunctionDecl* savedFunction = currentFunction;
      ParsedFile* savedFile = currentFile;
      bool savedShared = emittingShared;
      funcletOverride.clear();
      resetFunctionState();
      currentFunction = nullptr;
      VariableDecl* def = variableDefinition(v);
      currentFile = def->file;
      emittingShared = false;
      std::string fnName = "@\"kov.init." + name.substr(2, name.size() - 3) + "\""; // per instantiation for class template members
      try
      {
        pushScope();
        if (t->isReference() && def->init)
        {
          // The address of what the reference is bound to.
          std::string target = isClass(types.stripRef(def->init->ty)) ? rvalue(def->init).text : lvalue(def->init);
          fn += "  store ptr " + target + ", ptr " + name + "\n";
        }
        else if (def->init)
          initializeFrom(name, t, def->init, at);
        else if (isClass(t))
          defaultConstruct(name, t, at);
        else if (t->kind == TyKind::Array)
          constructArray(name, t, at); // `static Style styles[3];`
        destroyTemps();
        popScope();
        fn += "  ret void\n";
        addDefinition(fnName, functionText("define void " + fnName + "() {\nentry:\n"));
        moduleCtors[moduleCount <= 1 ? 0 : currentModule].push_back(fnName);
      }
      catch (Unsupported&)
      {
        restoreState(saved);
        funcletOverride = savedOverride;
        currentFunction = savedFunction;
        currentFile = savedFile;
        emittingShared = savedShared;
        throw;
      }
      restoreState(saved);
      funcletOverride = savedOverride;
      currentFunction = savedFunction;
      currentFile = savedFile;
      emittingShared = savedShared;
    }

    // The llvm.global_ctors array of module k.
    std::string ctorsText(unsigned k)
    {
      if (k >= moduleCtors.size() || moduleCtors[k].empty())
        return "";
      std::string s = "@llvm.global_ctors = appending global [" + std::to_string(moduleCtors[k].size()) + " x { i32, ptr, ptr }] [";
      for (size_t i = 0; i < moduleCtors[k].size(); ++i)
        s += std::string(i ? ", " : "") + "{ i32, ptr, ptr } { i32 65535, ptr " + moduleCtors[k][i] + ", ptr null }";
      return s + "]\n";
    }

    // The declaration carrying a global's initializer: the out-of-line definition of a static data
    // member (`double Gui::scale = 1.0;`), or the declaration itself.
    VariableDecl* variableDefinition(VariableDecl* v)
    {
      Decl* d = global.definitionOf(v);
      return d && d->kind == NodeKind::VariableDecl ? static_cast<VariableDecl*>(d) : v;
    }

    // `Counted<int>::instances`: the instantiation a qualified static member belongs to (null for
    // an unqualified name or a class that is not a template).
    const Ty* qualifierClass(Node* e, Node* at)
    {
      if (!e || e->kind != NodeKind::NameExpr)
        return nullptr;
      auto* n = static_cast<NameExpr*>(e);
      if (n->name.parts.size() <= 1)
        return nullptr;
      QualifiedName qualifier = n->name;
      qualifier.parts.count -= 1;
      NamedType tmp{};
      tmp.kind = NodeKind::NamedType;
      tmp.token = n->token;
      tmp.name = qualifier;
      TemplateArgs none;
      const Ty* q = global.typeOfAstIn(&tmp, currentFunction, currentArgs ? *currentArgs : none);
      return q && isClass(q) && !q->args.empty() ? canonicalClass(q, at) : nullptr;
    }

    // The instantiation a static member of a class template is used with: the qualifier written,
    // or the class of the method being emitted.
    const Ty* staticMemberClass(VariableDecl* v, Node* e, Node* at, const Ty* classTy)
    {
      Decl* owner = global.owningClass(v);
      if (!owner || owner->templateParams.empty())
        return nullptr;
      if (classTy)
        return canonicalClass(classTy, at);
      if (const Ty* q = qualifierClass(e, at))
        return q;
      if (currentClassTy && classDef(canonicalClass(currentClassTy, at)->decl, at) == classDef(owner, at))
        return canonicalClass(currentClassTy, at);
      return nullptr; // no instantiation in sight (an alias used as the qualifier): one shared object, as for a plain class
    }

    // `using EmptyWidget::EmptyWidget;` in a class without constructors of its own: the base whose
    // constructors are inherited (its name, as a constructor initializer would spell it); null if none.
    std::optional<QualifiedName> inheritedConstructorBase(ClassDecl* c)
    {
      for (Decl* m : c->members)
      {
        if (m->kind != NodeKind::UsingNameDecl)
          continue;
        const QualifiedName& q = static_cast<UsingNameDecl*>(m)->name;
        if (q.parts.size() < 2 || q.parts[q.parts.size() - 1].symbol != q.parts[q.parts.size() - 2].symbol)
          continue;
        QualifiedName base = q;
        base.parts.count -= 1;
        return base;
      }
      return std::nullopt;
    }

    // The address of the object a global names: the global itself, or, for a reference global,
    // what it was bound to. classTy: the instantiation, for a static member of a class template.
    std::string globalAddress(VariableDecl* v, Node* at, const Ty* classTy = nullptr)
    {
      classTy = staticMemberClass(v, at, at, classTy);
      std::string g = globalFor(v, at, classTy);
      const Ty* t = classTy ? global.typeOfDeclIn(v, global.classArgsOf(classTy)) : global.typeOfDecl(v);
      if (!t || !t->isReference())
        return g;
      std::string p = newTemp();
      fn += "  " + p + " = load ptr, ptr " + g + "\n";
      return p;
    }

    std::string globalFor(VariableDecl* v, Node* at, const Ty* classTy = nullptr)
    {
      TemplateArgs classArgs;
      std::string instanceName;
      if (classTy)
      {
        // A static member of a class template: one global per instantiation, its initializer
        // evaluated under the instantiation's arguments.
        classArgs = global.classArgsOf(classTy);
        instanceName = "@\"" + types.toString(classTy, &symbols) + "::" + sym(v->name.last().symbol) + "\"";
        auto found = instanceGlobals.find(instanceName);
        if (found != instanceGlobals.end())
          return found->second;
      }
      else
      {
        auto it = globals.find(v);
        if (it != globals.end())
          return it->second;
      }
      ArgsScope argsScope(*this, classTy ? &classArgs : currentArgs);
      struct SharedScope
      {
        bool& flag;
        bool saved;
        SharedScope(bool& f) : flag(f), saved(f) { flag = true; }
        ~SharedScope() { flag = saved; }
      } sharedScope(emittingShared); // a global's initializer belongs to every module
      const Ty* t = classTy ? global.typeOfDeclIn(v, classArgs) : global.typeOfDecl(v);
      if (!t || !t->isKnown())
        unsupported(at, "a global of unknown type");
      std::string name = classTy ? instanceName : "@\"" + global.qualifiedName(v) + "\"";
      if (classTy)
        instanceGlobals[instanceName] = name;
      else
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
      bool constructed = false;
      const Ty* st = types.stripRef(t);
      VariableDecl* def = variableDefinition(v); // `static double scale;` in the class, `double Gui::scale = 1.0;` outside
      if (classTy && def->init)
        global.retypeInitializerFor(def, classArgs); // `static constexpr T ValuesPerOne = T(1) << Bits`: typed for this instantiation
      ParsedFile* saved = currentFile;
      currentFile = def->file;
      if (t->isReference())
      {
        // `static const Style& defaultWidgetStyle = getStyleStatic();`: a pointer, bound at startup.
        currentFile = saved;
        decls += "$" + name.substr(1) + " = comdat any\n" + name + " = linkonce_odr global ptr null, comdat\n";
        if (def->init)
          emitGlobalConstructor(v, name, t, at);
        return name;
      }
      if (def->init)
      {
        std::optional<int64_t> c;
        {
          ConstContextScope context(*this, def); // traits in the initializer resolve their names in the declaration's context
          c = evalConst(def->init);
        }
        if (c && (st->kind == TyKind::Int || st->kind == TyKind::Char || st->kind == TyKind::Enum))
          init = std::to_string(*c);
        else if (c && st->kind == TyKind::Float)
          init = floatConstant(double(*c), st->bits);
        else if (st->kind == TyKind::Float && def->init->kind == NodeKind::LiteralExpr &&
                 static_cast<LiteralExpr*>(def->init)->literalKind == TokenKind::Number)
        {
          std::string digits;
          for (char ch : spelling(def->init->token))
            if (ch != '\'' && ch != 'f' && ch != 'F' && ch != 'l' && ch != 'L')
              digits += ch;
          init = floatConstant(strtod(digits.c_str(), nullptr), st->bits);
        }
        else if (c && st->kind == TyKind::Bool)
          init = *c ? "true" : "false";
        else if (st->kind == TyKind::Pointer && def->init->kind == NodeKind::LiteralExpr && static_cast<LiteralExpr*>(def->init)->keyword == Kw::nullptr_)
          init = "null";
        else if (def->init->kind == NodeKind::LiteralExpr && static_cast<LiteralExpr*>(def->init)->literalKind == TokenKind::String &&
                 st->kind == TyKind::Pointer)
          init = stringConstant(static_cast<LiteralExpr*>(def->init));
        else
          constructed = true; // built at startup, see emitGlobalConstructor
      }
      else if (needsConstruction(st, at))
        constructed = true;
      currentFile = saved;
      // Defined in every module, merged by the linker (one instance per program).
      decls += "$" + name.substr(1) + " = comdat any\n" + name + " = linkonce_odr " + (t->isConst && !constructed ? "constant " : "global ") +
               llType(st, at) + " " + init + ", comdat\n";
      if (constructed)
        emitGlobalConstructor(v, name, st, at);
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
                                     const Ty* objectTy, Node* at, const Ty** foundIn, bool templates = false)
    {
      FunctionDecl* found = nullptr;
      std::function<void(const Ty*)> search = [&](const Ty* cls)
      {
        ClassDecl* c = classDef(cls->decl, at);
        std::vector<FunctionDecl*> candidates;
        for (Decl* m : c->members)
        {
          if (m->kind != NodeKind::FunctionDecl || (!templates && !m->templateParams.empty()))
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
      FunctionDecl* f = findMemberFunction(object.ty, name, argTypesOf(args), object.ty, at, &foundIn, true);
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
      TemplateArgs fnArgs = global.classArgsOf(foundIn);
      if (!f->templateParams.empty())
        global.deduceArgs(f, argTypesOf(args), fnArgs); // `template<typename U> bool operator==(const U*) const`
      std::string indirect;
      if ((f->flags & DF_Virtual) && isClass(object.ty))
        if (int slot = slotOf(object.ty, f, at); slot >= 0)
          indirect = loadSlot(object.text, slot); // `*a == *b` on images: the dynamic type's operator==
      return callFunction(f, fnArgs, foundIn, thisArg, "", args, at, indirect);
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
          if (!classTy && (f->flags & DF_Static) && n->name.parts.size() > 1 && global.owningClass(f))
          {
            // `Box<T>::get()`: the instantiation comes from the qualifier.
            QualifiedName qualifier = n->name;
            qualifier.parts.count -= 1;
            NamedType tmp{};
            tmp.kind = NodeKind::NamedType;
            tmp.token = n->token;
            tmp.name = qualifier;
            TemplateArgs none;
            const Ty* q = global.typeOfAstIn(&tmp, currentFunction, currentArgs ? *currentArgs : none);
            if (q && isClass(q) && !q->args.empty())
              classTy = canonicalClass(q, c);
          }
          // move(x) and forward(x): identity casts (lref -> rref in the type system,
          // but both are just a pointer in LLVM IR).
          if (!classTy && f->params.size() == 1 && c->args.size() == 1 &&
              !f->templateParams.empty() && c->templateArgCount != f->templateParams.size())
          {
            std::string_view fname = f->name.empty() ? "" : symbols.name(f->name.last().symbol);
            if (fname == "move" || fname == "forward")
            {
              std::string addr = lvalue(c->args[0]);
              const Ty* argTy = c->args[0]->ty;
              if (argTy) argTy = types.stripRef(argTy);
              return {addr, argTy ? argTy : types.unknown(), false};
            }
          }
          return callFunction(f, calleeArgs(f, classTy, c, c), classTy, thisArg, sret, c->args, c, indirect);
        }
        if (!d)
          if (std::optional<Value> v = vaBuiltin(n, c))
            return *v;
        if (!d)
          if (std::optional<Value> v = sseBuiltin(n, c))
            return *v;
        if (!d || d->kind == NodeKind::ClassDecl || d->kind == NodeKind::EnumDecl || d->kind == NodeKind::UsingAliasDecl ||
            d->kind == NodeKind::TemplateParamDecl || (d->kind == NodeKind::VariableDecl && (d->flags & DF_Typedef)))
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
          if (c->args.size() == 1 && isPackExpansion(c->args[0]))
          {
            // `T(forward<Args>(args)...)` with a scalar T: the pack's one element, re-typed for it.
            Expr* pattern = static_cast<UnaryExpr*>(c->args[0])->operand;
            size_t count = packElementCount(pattern, c);
            if (count == 0)
              return {t->kind == TyKind::Float ? "0.0" : t->kind == TyKind::Pointer ? "null" : "0", t};
            if (count != 1)
              unsupported(c, "a scalar built from a pack of several elements");
            Value v;
            forEachPackElement(pattern, c, [&](size_t) { v = convert(rvalue(pattern), t, c); });
            return v;
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
            Value p = arrowOperand(rvalue(m->base), c);
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
              Value p = arrowOperand(rvalue(m->base), c);
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
      // `(obj.*pmf)(args)` / `(p->*pmf)(args)`: a call through a pointer to a member function, the
      // object first.
      {
        Expr* inner = callee;
        while (inner && inner->kind == NodeKind::ParenExpr)
          inner = static_cast<ParenExpr*>(inner)->inner;
        if (inner && inner->kind == NodeKind::BinaryExpr && !static_cast<BinaryExpr*>(inner)->isKeyword &&
            (P(static_cast<BinaryExpr*>(inner)->op) == P::DotStar || P(static_cast<BinaryExpr*>(inner)->op) == P::ArrowStar))
        {
          auto* b = static_cast<BinaryExpr*>(inner);
          const Ty* mp = b->rhs && b->rhs->ty ? types.stripRef(b->rhs->ty) : nullptr;
          if (mp && mp->kind == TyKind::MemberPointer && mp->inner && mp->inner->kind == TyKind::Function)
          {
            std::string object = P(b->op) == P::ArrowStar ? arrowOperand(rvalue(b->lhs), c).text : rvalue(b->lhs).text;
            Value fp = rvalue(b->rhs);
            return callIndirect(fp.text, mp->inner, object, c->args, sret, c);
          }
        }
      }
      // Call through a closure or a function pointer.
      Value fnValue = rvalue(callee);
      if (isLambda(fnValue.ty))
        return callClosure(fnValue, c->args, sret, c);
      if (isClass(fnValue.ty))
        return operatorCall(fnValue, "operator()", c->args, c, true); // a functor: object(args)
      const Ty* ft = fnValue.ty->kind == TyKind::Pointer ? fnValue.ty->inner : fnValue.ty;
      if (!ft || ft->kind != TyKind::Function)
        unsupported(c, "a call that did not resolve to a function (the callee has type " + types.toString(fnValue.ty, &symbols) + ")");
      return callIndirect(fnValue.text, ft, "", c->args, sret, c);
    }

    // A call through a function pointer, with the same calling convention as a declared Kov
    // function: the object first for a member function (thisArg), the caller's result slot for a
    // class result, classes by pointer (a copy the callee owns), references as addresses.
    Value callIndirect(const std::string& fnText, const Ty* ft, const std::string& thisArg, Span<Expr*> callArgs, const std::string& sret, Node* c)
    {
      Signature s;
      const Ty* ret = ft->inner;
      if (isClass(ret))
      {
        s.hasSret = true;
        s.returnType = "void";
      }
      else
        s.returnType = ret->isReference() ? "ptr" : llType(ret, c);
      std::string args, resultAddr;
      if (!thisArg.empty())
      {
        args = "ptr " + thisArg;
        s.paramTypes.push_back("ptr");
      }
      if (s.hasSret)
      {
        resultAddr = sret.empty() ? newObjectSlot(ret, c) : sret;
        args += (args.empty() ? "" : ", ") + std::string("ptr ") + resultAddr;
        s.paramTypes.push_back("ptr");
      }
      size_t position = 0;
      auto addArgument = [&](Expr* arg)
      {
        const Ty* pt = position < ft->args.size() ? ft->args[position] : nullptr;
        std::string text = argumentText(pt, arg, c);
        args += (args.empty() ? "" : ", ") + text;
        ++position;
      };
      for (Expr* arg : callArgs)
      {
        if (isPackExpansion(arg))
        {
          Expr* pattern = static_cast<UnaryExpr*>(arg)->operand;
          forEachPackElement(pattern, c, [&](size_t) { addArgument(pattern); });
        }
        else
          addArgument(arg);
      }
      for (const Ty* pt : ft->args)
        s.paramTypes.push_back(isAggregate(pt) || pt->isReference() ? "ptr" : llType(pt, c));
      s.variadic = ft->variadic;
      std::string sig = signatureText(s);
      if (s.returnType == "void")
      {
        emitCall("", sig + " " + fnText + "(" + args + ")");
        if (s.hasSret)
          return {resultAddr, ret, sret.empty()};
        return {"", types.voidType()};
      }
      std::string r = newTemp();
      emitCall(r + " = ", sig + " " + fnText + "(" + args + ")");
      return {r, ret, false};
    }

    // va_start, va_end and va_copy on a C va_list (a char* on x64 Windows): the LLVM intrinsics.
    // They are not declared anywhere; a function defined with `...` uses them like C does.
    std::optional<Value> vaBuiltin(NameExpr* n, CallExpr* c)
    {
      if (n->name.parts.size() != 1)
        return std::nullopt;
      std::string name = sym(n->name.parts[0].symbol);
      if (name != "va_start" && name != "va_end" && name != "va_copy")
        return std::nullopt;
      if (c->args.size() < (name == "va_start" ? 2u : name == "va_copy" ? 2u : 1u))
        unsupported(c, name + " with too few arguments");
      std::string list = lvalue(c->args[0]);
      std::string intrinsic = name == "va_start" ? "@llvm.va_start.p0" : name == "va_end" ? "@llvm.va_end.p0" : "@llvm.va_copy.p0";
      if (!declaredNames[intrinsic])
      {
        declaredNames[intrinsic] = true;
        decls += "declare void " + intrinsic + (name == "va_copy" ? "(ptr, ptr)\n" : "(ptr)\n");
      }
      if (name == "va_copy")
        fn += "  call void " + intrinsic + "(ptr " + list + ", ptr " + lvalue(c->args[1]) + ")\n";
      else
        fn += "  call void " + intrinsic + "(ptr " + list + ")\n";
      return Value{"", types.voidType(), false};
    }

    // SSE intrinsics: _mm_* functions and _MM_SHUFFLE macros.
    // They are not declared; the codegen emits LLVM IR directly for each one.

    // The LLVM vector type for an SSE suffix: ps = <4 x float>, pd = <2 x double>,
    // si128/epi32/epu32/epi8 = <4 x i32> (or <2 x i64>, <16 x i8> as needed).
    const Ty* sseTypeByName(const std::string& name)
    {
      if (name.ends_with("_ps")) return types.vectorType(types.floatType(32), 4);
      if (name.ends_with("_pd")) return types.vectorType(types.floatType(64), 2);
      if (name.ends_with("_si128")) return types.vectorType(types.intType(32), 4);
      if (name.ends_with("_epi32") || name.ends_with("_epu32")) return types.vectorType(types.intType(32), 4);
      if (name.ends_with("_epi64")) return types.vectorType(types.intType(64), 2);
      if (name.ends_with("_epi8")) return types.vectorType(types.intType(8), 16);
      return nullptr;
    }

    // Load a SIMD vector from an argument expression. If the argument is an SSE wrapper class
    // (float32x4, float64x2, int32x4), extract the first field; otherwise load directly.
    std::string sseArg(Expr* arg, const Ty* vecTy, Node* at)
    {
      Value v = rvalue(arg);
      std::string src = v.text;
      const Ty* vt = v.ty ? types.unqualified(types.stripRef(v.ty)) : nullptr;
      if (vt && vt->kind == TyKind::Class)
      {
        StructInfo& info = structFor(vt, at);
        if (!info.fieldDecls.empty())
          src = gep(info.name, v.text, 0); // field 0: the __m128/__m128d/__m128i member
      }
      std::string r = newTemp();
      fn += "  " + r + " = load " + llType(vecTy, at) + ", ptr " + src + "\n";
      return r;
    }

    // Evaluate a scalar argument (float, double, int32_t) as a register value.
    std::string sseScalar(Expr* arg, const Ty* scalarTy, Node* at)
    {
      Value v = rvalue(arg);
      return convert(v, scalarTy, at).text;
    }

    // Declare an LLVM intrinsic if not already declared, and return its name.
    std::string sseIntrinsic(const std::string& name, const std::string& retTy, const std::string& paramTys)
    {
      std::string full = "@" + name;
      if (!declaredNames[full])
      {
        declaredNames[full] = true;
        decls += "declare " + retTy + " " + full + "(" + paramTys + ")\n";
      }
      return full;
    }

    // Emit a binary SSE operation: fadd, fsub, fmul, fdiv, add, sub for vectors.
    std::string sseBinOp(const std::string& op, const std::string& tyStr, const std::string& a, const std::string& b)
    {
      std::string r = newTemp();
      fn += "  " + r + " = " + op + " " + tyStr + " " + a + ", " + b + "\n";
      return r;
    }

    // Emit an LLVM call.
    std::string sseCall(const std::string& intrinsic, const std::string& retTy, const std::string& args)
    {
      std::string r = newTemp();
      fn += "  " + r + " = call " + retTy + " " + intrinsic + "(" + args + ")\n";
      return r;
    }

    // Store a vector result into a temporary alloca and return its address with the vector type.
    Value sseResult(const std::string& reg, const Ty* vecTy, Node* at)
    {
      std::string slot = newTemp();
      allocas += "  " + slot + " = alloca " + llType(vecTy, at) + "\n";
      fn += "  store " + llType(vecTy, at) + " " + reg + ", ptr " + slot + "\n";
      return {slot, vecTy, false};
    }

    std::optional<Value> sseBuiltin(NameExpr* n, CallExpr* c)
    {
      if (n->name.parts.size() != 1)
        return std::nullopt;
      std::string name = sym(n->name.parts[0].symbol);

      // _MM_SHUFFLE(z, y, x, w) = ((z<<6)|(y<<4)|(x<<2)|w)
      if (name == "_MM_SHUFFLE" && c->args.size() == 4)
      {
        auto e = [&](int i) { return evalConst(c->args[i], 0); };
        auto z = e(0), y = e(1), x = e(2), w = e(3);
        if (z && y && x && w)
          return Value{std::to_string((*z << 6) | (*y << 4) | (*x << 2) | *w), types.intType(32)};
        return std::nullopt;
      }
      if (name == "_MM_SHUFFLE2" && c->args.size() == 2)
      {
        auto a = evalConst(c->args[0], 0), b = evalConst(c->args[1], 0);
        if (a && b)
          return Value{std::to_string((*a << 1) | *b), types.intType(32)};
        return std::nullopt;
      }

      if (!name.starts_with("_mm_"))
        return std::nullopt;

      // Determine the vector type from the name suffix.
      const Ty* v4f = types.vectorType(types.floatType(32), 4);
      const Ty* v2d = types.vectorType(types.floatType(64), 2);
      const Ty* v4i = types.vectorType(types.intType(32), 4);
      const Ty* v2i64 = types.vectorType(types.intType(64), 2);
      const Ty* v16i8 = types.vectorType(types.intType(8), 16);

      auto ll = [&](const Ty* t) { return llType(t, c); };

      // --- undefined (zero args) ---
      if (name == "_mm_undefined_ps")  return sseResult("undef", v4f, c);
      if (name == "_mm_undefined_pd")  return sseResult("undef", v2d, c);
      if (name == "_mm_undefined_si128") return sseResult("undef", v4i, c);
      if (name == "_mm_setzero_si128") return sseResult("zeroinitializer", v4i, c);

      // --- arithmetic: binary float ---
      auto floatBinOp = [&](const std::string& op, const Ty* vt) -> std::optional<Value>
      {
        if (c->args.size() < 2) return std::nullopt;
        std::string a = sseArg(c->args[0], vt, c);
        std::string b = sseArg(c->args[1], vt, c);
        return sseResult(sseBinOp(op, ll(vt), a, b), vt, c);
      };

      if (name == "_mm_add_ps")  return floatBinOp("fadd", v4f);
      if (name == "_mm_sub_ps")  return floatBinOp("fsub", v4f);
      if (name == "_mm_mul_ps")  return floatBinOp("fmul", v4f);
      if (name == "_mm_div_ps")  return floatBinOp("fdiv", v4f);
      if (name == "_mm_add_pd")  return floatBinOp("fadd", v2d);
      if (name == "_mm_sub_pd")  return floatBinOp("fsub", v2d);
      if (name == "_mm_mul_pd")  return floatBinOp("fmul", v2d);
      if (name == "_mm_div_pd")  return floatBinOp("fdiv", v2d);

      // --- arithmetic: binary integer ---
      auto intBinOp = [&](const std::string& op, const Ty* vt) -> std::optional<Value>
      {
        if (c->args.size() < 2) return std::nullopt;
        std::string a = sseArg(c->args[0], vt, c);
        std::string b = sseArg(c->args[1], vt, c);
        return sseResult(sseBinOp(op, ll(vt), a, b), vt, c);
      };

      if (name == "_mm_add_epi32") return intBinOp("add", v4i);
      if (name == "_mm_sub_epi32") return intBinOp("sub", v4i);

      // --- bitwise: implemented as integer and/or/xor then bitcast back ---
      auto bitwiseOp = [&](const std::string& op, const Ty* vt) -> std::optional<Value>
      {
        if (c->args.size() < 2) return std::nullopt;
        // Load as the native type, bitcast to i128 for the operation, bitcast back.
        std::string a = sseArg(c->args[0], vt, c);
        std::string b = sseArg(c->args[1], vt, c);
        // For float types, bitcast to <4 x i32> for the integer operation.
        bool needsCast = (vt == v4f || vt == v2d);
        const Ty* intVt = (vt == v2d) ? v2i64 : v4i;
        std::string ai = a, bi = b;
        if (needsCast)
        {
          ai = newTemp(); fn += "  " + ai + " = bitcast " + ll(vt) + " " + a + " to " + ll(intVt) + "\n";
          bi = newTemp(); fn += "  " + bi + " = bitcast " + ll(vt) + " " + b + " to " + ll(intVt) + "\n";
        }
        std::string ri = sseBinOp(op, ll(intVt), ai, bi);
        std::string res = ri;
        if (needsCast)
        {
          res = newTemp(); fn += "  " + res + " = bitcast " + ll(intVt) + " " + ri + " to " + ll(vt) + "\n";
        }
        return sseResult(res, vt, c);
      };

      if (name == "_mm_and_ps")    return bitwiseOp("and", v4f);
      if (name == "_mm_or_ps")     return bitwiseOp("or", v4f);
      if (name == "_mm_xor_ps")    return bitwiseOp("xor", v4f);
      if (name == "_mm_and_pd")    return bitwiseOp("and", v2d);
      if (name == "_mm_or_pd")     return bitwiseOp("or", v2d);
      if (name == "_mm_xor_pd")    return bitwiseOp("xor", v2d);
      if (name == "_mm_and_si128") return bitwiseOp("and", v4i);
      if (name == "_mm_or_si128")  return bitwiseOp("or", v4i);
      if (name == "_mm_xor_si128") return bitwiseOp("xor", v4i);

      // andnot: ~a & b
      auto andnotOp = [&](const Ty* vt) -> std::optional<Value>
      {
        if (c->args.size() < 2) return std::nullopt;
        std::string a = sseArg(c->args[0], vt, c);
        std::string b = sseArg(c->args[1], vt, c);
        bool needsCast = (vt == v4f || vt == v2d);
        const Ty* intVt = (vt == v2d) ? v2i64 : v4i;
        std::string ai = a, bi = b;
        if (needsCast)
        {
          ai = newTemp(); fn += "  " + ai + " = bitcast " + ll(vt) + " " + a + " to " + ll(intVt) + "\n";
          bi = newTemp(); fn += "  " + bi + " = bitcast " + ll(vt) + " " + b + " to " + ll(intVt) + "\n";
        }
        // NOT a
        std::string notA = newTemp();
        fn += "  " + notA + " = xor " + ll(intVt) + " " + ai + ", <";
        for (uint32_t i = 0; i < intVt->count; ++i) fn += (i ? ", " : "") + std::string(intVt->inner->bits == 64 ? "i64 -1" : "i32 -1");
        fn += ">\n";
        std::string ri = sseBinOp("and", ll(intVt), notA, bi);
        std::string res = ri;
        if (needsCast)
        {
          res = newTemp(); fn += "  " + res + " = bitcast " + ll(intVt) + " " + ri + " to " + ll(vt) + "\n";
        }
        return sseResult(res, vt, c);
      };

      if (name == "_mm_andnot_ps")    return andnotOp(v4f);
      if (name == "_mm_andnot_pd")    return andnotOp(v2d);
      if (name == "_mm_andnot_si128") return andnotOp(v4i);

      // --- float comparisons: fcmp, then select to produce all-ones or all-zeros mask ---
      auto cmpOp = [&](const std::string& pred, const Ty* vt) -> std::optional<Value>
      {
        if (c->args.size() < 2) return std::nullopt;
        std::string a = sseArg(c->args[0], vt, c);
        std::string b = sseArg(c->args[1], vt, c);
        std::string cmp = newTemp();
        std::string boolVt = "<" + std::to_string(vt->count) + " x i1>";
        fn += "  " + cmp + " = fcmp " + pred + " " + ll(vt) + " " + a + ", " + b + "\n";
        // Select: true → all-ones, false → all-zeros (as float bit pattern)
        const Ty* intVt = (vt == v2d) ? v2i64 : v4i;
        std::string allOnes = "<";
        for (uint32_t i = 0; i < intVt->count; ++i) allOnes += (i ? ", " : "") + std::string(intVt->inner->bits == 64 ? "i64 -1" : "i32 -1");
        allOnes += ">";
        std::string sel = newTemp();
        fn += "  " + sel + " = select " + boolVt + " " + cmp + ", " + ll(intVt) + " " + allOnes + ", " + ll(intVt) + " zeroinitializer\n";
        std::string res = newTemp();
        fn += "  " + res + " = bitcast " + ll(intVt) + " " + sel + " to " + ll(vt) + "\n";
        return sseResult(res, vt, c);
      };

      if (name == "_mm_cmpeq_ps")  return cmpOp("oeq", v4f);
      if (name == "_mm_cmpge_ps")  return cmpOp("oge", v4f);
      if (name == "_mm_cmpgt_ps")  return cmpOp("ogt", v4f);
      if (name == "_mm_cmple_ps")  return cmpOp("ole", v4f);
      if (name == "_mm_cmplt_ps")  return cmpOp("olt", v4f);
      if (name == "_mm_cmpneq_ps") return cmpOp("une", v4f);
      if (name == "_mm_cmpeq_pd")  return cmpOp("oeq", v2d);
      if (name == "_mm_cmpge_pd")  return cmpOp("oge", v2d);
      if (name == "_mm_cmpgt_pd")  return cmpOp("ogt", v2d);
      if (name == "_mm_cmple_pd")  return cmpOp("ole", v2d);
      if (name == "_mm_cmplt_pd")  return cmpOp("olt", v2d);
      if (name == "_mm_cmpneq_pd") return cmpOp("une", v2d);

      // --- integer comparisons ---
      auto icmpOp = [&](const std::string& pred, const Ty* vt) -> std::optional<Value>
      {
        if (c->args.size() < 2) return std::nullopt;
        std::string a = sseArg(c->args[0], vt, c);
        std::string b = sseArg(c->args[1], vt, c);
        std::string cmp = newTemp();
        std::string boolVt = "<" + std::to_string(vt->count) + " x i1>";
        fn += "  " + cmp + " = icmp " + pred + " " + ll(vt) + " " + a + ", " + b + "\n";
        std::string sel = newTemp();
        std::string allOnes = "<";
        for (uint32_t i = 0; i < vt->count; ++i) allOnes += (i ? ", " : "") + std::string(vt->inner->bits == 64 ? "i64 -1" : "i32 -1");
        allOnes += ">";
        fn += "  " + sel + " = select " + boolVt + " " + cmp + ", " + ll(vt) + " " + allOnes + ", " + ll(vt) + " zeroinitializer\n";
        return sseResult(sel, vt, c);
      };

      if (name == "_mm_cmpeq_epi32") return icmpOp("eq", v4i);
      if (name == "_mm_cmpgt_epi32") return icmpOp("sgt", v4i);
      if (name == "_mm_cmplt_epi32") return icmpOp("slt", v4i);

      // --- set (splat / build) ---
      if (name == "_mm_set1_ps" && c->args.size() >= 1)
      {
        std::string s = sseScalar(c->args[0], types.floatType(32), c);
        std::string v = newTemp();
        fn += "  " + v + " = insertelement " + ll(v4f) + " undef, float " + s + ", i32 0\n";
        std::string splat = newTemp();
        fn += "  " + splat + " = shufflevector " + ll(v4f) + " " + v + ", " + ll(v4f) + " undef, <4 x i32> zeroinitializer\n";
        return sseResult(splat, v4f, c);
      }
      if (name == "_mm_set1_pd" && c->args.size() >= 1)
      {
        std::string s = sseScalar(c->args[0], types.floatType(64), c);
        std::string v = newTemp();
        fn += "  " + v + " = insertelement " + ll(v2d) + " undef, double " + s + ", i32 0\n";
        std::string splat = newTemp();
        fn += "  " + splat + " = shufflevector " + ll(v2d) + " " + v + ", " + ll(v2d) + " undef, <2 x i32> zeroinitializer\n";
        return sseResult(splat, v2d, c);
      }
      if (name == "_mm_set1_epi32" && c->args.size() >= 1)
      {
        std::string s = sseScalar(c->args[0], types.intType(32), c);
        std::string v = newTemp();
        fn += "  " + v + " = insertelement " + ll(v4i) + " undef, i32 " + s + ", i32 0\n";
        std::string splat = newTemp();
        fn += "  " + splat + " = shufflevector " + ll(v4i) + " " + v + ", " + ll(v4i) + " undef, <4 x i32> zeroinitializer\n";
        return sseResult(splat, v4i, c);
      }
      // _mm_set_ps(d, c, b, a) builds <a, b, c, d> — note reversed order
      if (name == "_mm_set_ps" && c->args.size() >= 4)
      {
        std::string vec = "undef";
        for (int i = 3; i >= 0; --i)
        {
          std::string s = sseScalar(c->args[i], types.floatType(32), c);
          std::string next = newTemp();
          fn += "  " + next + " = insertelement " + ll(v4f) + " " + vec + ", float " + s + ", i32 " + std::to_string(3 - i) + "\n";
          vec = next;
        }
        return sseResult(vec, v4f, c);
      }
      if (name == "_mm_set_pd" && c->args.size() >= 2)
      {
        std::string s0 = sseScalar(c->args[1], types.floatType(64), c);
        std::string s1 = sseScalar(c->args[0], types.floatType(64), c);
        std::string v = newTemp();
        fn += "  " + v + " = insertelement " + ll(v2d) + " undef, double " + s0 + ", i32 0\n";
        std::string v2 = newTemp();
        fn += "  " + v2 + " = insertelement " + ll(v2d) + " " + v + ", double " + s1 + ", i32 1\n";
        return sseResult(v2, v2d, c);
      }
      if (name == "_mm_set_epi32" && c->args.size() >= 4)
      {
        std::string vec = "undef";
        for (int i = 3; i >= 0; --i)
        {
          std::string s = sseScalar(c->args[i], types.intType(32), c);
          std::string next = newTemp();
          fn += "  " + next + " = insertelement " + ll(v4i) + " " + vec + ", i32 " + s + ", i32 " + std::to_string(3 - i) + "\n";
          vec = next;
        }
        return sseResult(vec, v4i, c);
      }

      // --- load (unaligned) ---
      if (name == "_mm_loadu_ps" && c->args.size() >= 1)
      {
        std::string p = rvalue(c->args[0]).text;
        std::string r = newTemp();
        fn += "  " + r + " = load " + ll(v4f) + ", ptr " + p + ", align 1\n";
        return sseResult(r, v4f, c);
      }
      if (name == "_mm_loadu_pd" && c->args.size() >= 1)
      {
        std::string p = rvalue(c->args[0]).text;
        std::string r = newTemp();
        fn += "  " + r + " = load " + ll(v2d) + ", ptr " + p + ", align 1\n";
        return sseResult(r, v2d, c);
      }
      if (name == "_mm_loadu_si128" && c->args.size() >= 1)
      {
        std::string p = rvalue(c->args[0]).text;
        std::string r = newTemp();
        fn += "  " + r + " = load " + ll(v4i) + ", ptr " + p + ", align 1\n";
        return sseResult(r, v4i, c);
      }

      // --- store (unaligned) — returns void ---
      if (name == "_mm_storeu_ps" && c->args.size() >= 2)
      {
        std::string p = rvalue(c->args[0]).text;
        std::string v = sseArg(c->args[1], v4f, c);
        fn += "  store " + ll(v4f) + " " + v + ", ptr " + p + ", align 1\n";
        return Value{"", types.voidType(), false};
      }
      if (name == "_mm_storeu_pd" && c->args.size() >= 2)
      {
        std::string p = rvalue(c->args[0]).text;
        std::string v = sseArg(c->args[1], v2d, c);
        fn += "  store " + ll(v2d) + " " + v + ", ptr " + p + ", align 1\n";
        return Value{"", types.voidType(), false};
      }

      // --- cast (bitcast between vector types, no conversion) ---
      auto castOp = [&](const Ty* from, const Ty* to) -> std::optional<Value>
      {
        if (c->args.size() < 1) return std::nullopt;
        std::string a = sseArg(c->args[0], from, c);
        std::string r = newTemp();
        fn += "  " + r + " = bitcast " + ll(from) + " " + a + " to " + ll(to) + "\n";
        return sseResult(r, to, c);
      };

      if (name == "_mm_castps_si128") return castOp(v4f, v4i);
      if (name == "_mm_castsi128_ps") return castOp(v4i, v4f);
      if (name == "_mm_castps_pd")    return castOp(v4f, v2d);
      if (name == "_mm_castpd_ps")    return castOp(v2d, v4f);
      if (name == "_mm_castsi128_pd") return castOp(v4i, v2d);
      if (name == "_mm_castpd_si128") return castOp(v2d, v4i);

      // --- convert (type conversion) ---
      if (name == "_mm_cvtepi32_ps" && c->args.size() >= 1)
      {
        std::string a = sseArg(c->args[0], v4i, c);
        std::string r = newTemp();
        fn += "  " + r + " = sitofp " + ll(v4i) + " " + a + " to " + ll(v4f) + "\n";
        return sseResult(r, v4f, c);
      }
      if (name == "_mm_cvtps_epi32" && c->args.size() >= 1)
      {
        std::string a = sseArg(c->args[0], v4f, c);
        std::string intr = sseIntrinsic("llvm.x86.sse2.cvtps2dq", ll(v4i), ll(v4f));
        return sseResult(sseCall(intr, ll(v4i), ll(v4f) + " " + a), v4i, c);
      }
      if (name == "_mm_cvtps_pd" && c->args.size() >= 1)
      {
        // Low 2 floats → 2 doubles
        std::string a = sseArg(c->args[0], v4f, c);
        const Ty* v2f = types.vectorType(types.floatType(32), 2);
        std::string lo = newTemp();
        fn += "  " + lo + " = shufflevector " + ll(v4f) + " " + a + ", " + ll(v4f) + " undef, <2 x i32> <i32 0, i32 1>\n";
        std::string r = newTemp();
        fn += "  " + r + " = fpext " + ll(v2f) + " " + lo + " to " + ll(v2d) + "\n";
        return sseResult(r, v2d, c);
      }
      if (name == "_mm_cvtpd_ps" && c->args.size() >= 1)
      {
        std::string a = sseArg(c->args[0], v2d, c);
        const Ty* v2f = types.vectorType(types.floatType(32), 2);
        std::string lo = newTemp();
        fn += "  " + lo + " = fptrunc " + ll(v2d) + " " + a + " to " + ll(v2f) + "\n";
        std::string r = newTemp();
        fn += "  " + r + " = shufflevector " + ll(v2f) + " " + lo + ", " + ll(v2f) + " zeroinitializer, <4 x i32> <i32 0, i32 1, i32 2, i32 3>\n";
        return sseResult(r, v4f, c);
      }

      // --- min / max / sqrt / rcp / rsqrt ---
      auto unaryIntr = [&](const std::string& intr, const Ty* vt) -> std::optional<Value>
      {
        if (c->args.size() < 1) return std::nullopt;
        std::string a = sseArg(c->args[0], vt, c);
        std::string i = sseIntrinsic(intr, ll(vt), ll(vt));
        return sseResult(sseCall(i, ll(vt), ll(vt) + " " + a), vt, c);
      };
      auto binaryIntr = [&](const std::string& intr, const Ty* vt) -> std::optional<Value>
      {
        if (c->args.size() < 2) return std::nullopt;
        std::string a = sseArg(c->args[0], vt, c);
        std::string b = sseArg(c->args[1], vt, c);
        std::string i = sseIntrinsic(intr, ll(vt), ll(vt) + ", " + ll(vt));
        return sseResult(sseCall(i, ll(vt), ll(vt) + " " + a + ", " + ll(vt) + " " + b), vt, c);
      };

      if (name == "_mm_min_ps")  return binaryIntr("llvm.x86.sse.min.ps", v4f);
      if (name == "_mm_max_ps")  return binaryIntr("llvm.x86.sse.max.ps", v4f);
      if (name == "_mm_min_pd")  return binaryIntr("llvm.x86.sse2.min.pd", v2d);
      if (name == "_mm_max_pd")  return binaryIntr("llvm.x86.sse2.max.pd", v2d);
      if (name == "_mm_sqrt_ps") return unaryIntr("llvm.sqrt.v4f32", v4f);
      if (name == "_mm_sqrt_pd") return unaryIntr("llvm.sqrt.v2f64", v2d);
      if (name == "_mm_rcp_ps")  return unaryIntr("llvm.x86.sse.rcp.ps", v4f);
      if (name == "_mm_rsqrt_ps") return unaryIntr("llvm.x86.sse.rsqrt.ps", v4f);

      // --- movemask (returns i32) ---
      if (name == "_mm_movemask_ps" && c->args.size() >= 1)
      {
        std::string a = sseArg(c->args[0], v4f, c);
        std::string i = sseIntrinsic("llvm.x86.sse.movmsk.ps", "i32", ll(v4f));
        return Value{sseCall(i, "i32", ll(v4f) + " " + a), types.intType(32)};
      }
      if (name == "_mm_movemask_pd" && c->args.size() >= 1)
      {
        std::string a = sseArg(c->args[0], v2d, c);
        std::string i = sseIntrinsic("llvm.x86.sse2.movmsk.pd", "i32", ll(v2d));
        return Value{sseCall(i, "i32", ll(v2d) + " " + a), types.intType(32)};
      }
      if (name == "_mm_movemask_epi8" && c->args.size() >= 1)
      {
        std::string a = sseArg(c->args[0], v4i, c);
        std::string ab = newTemp();
        fn += "  " + ab + " = bitcast " + ll(v4i) + " " + a + " to " + ll(v16i8) + "\n";
        std::string i = sseIntrinsic("llvm.x86.sse2.pmovmskb.128", "i32", ll(v16i8));
        return Value{sseCall(i, "i32", ll(v16i8) + " " + ab), types.intType(32)};
      }

      // --- shuffle ---
      if (name == "_mm_shuffle_ps" && c->args.size() >= 3)
      {
        std::string a = sseArg(c->args[0], v4f, c);
        std::string b = sseArg(c->args[1], v4f, c);
        auto imm = evalConst(c->args[2], 0);
        if (!imm) return std::nullopt;
        int m = int(*imm);
        int i0 = m & 3, i1 = (m >> 2) & 3, i2 = ((m >> 4) & 3) + 4, i3 = ((m >> 6) & 3) + 4;
        std::string r = newTemp();
        fn += "  " + r + " = shufflevector " + ll(v4f) + " " + a + ", " + ll(v4f) + " " + b +
              ", <4 x i32> <i32 " + std::to_string(i0) + ", i32 " + std::to_string(i1) + ", i32 " + std::to_string(i2) + ", i32 " + std::to_string(i3) + ">\n";
        return sseResult(r, v4f, c);
      }
      if (name == "_mm_shuffle_pd" && c->args.size() >= 3)
      {
        std::string a = sseArg(c->args[0], v2d, c);
        std::string b = sseArg(c->args[1], v2d, c);
        auto imm = evalConst(c->args[2], 0);
        if (!imm) return std::nullopt;
        int m = int(*imm);
        int i0 = m & 1, i1 = ((m >> 1) & 1) + 2;
        std::string r = newTemp();
        fn += "  " + r + " = shufflevector " + ll(v2d) + " " + a + ", " + ll(v2d) + " " + b +
              ", <2 x i32> <i32 " + std::to_string(i0) + ", i32 " + std::to_string(i1) + ">\n";
        return sseResult(r, v2d, c);
      }
      if (name == "_mm_shuffle_epi32" && c->args.size() >= 2)
      {
        std::string a = sseArg(c->args[0], v4i, c);
        auto imm = evalConst(c->args[1], 0);
        if (!imm) return std::nullopt;
        int m = int(*imm);
        int i0 = m & 3, i1 = (m >> 2) & 3, i2 = (m >> 4) & 3, i3 = (m >> 6) & 3;
        std::string r = newTemp();
        fn += "  " + r + " = shufflevector " + ll(v4i) + " " + a + ", " + ll(v4i) + " undef, <4 x i32> <i32 " +
              std::to_string(i0) + ", i32 " + std::to_string(i1) + ", i32 " + std::to_string(i2) + ", i32 " + std::to_string(i3) + ">\n";
        return sseResult(r, v4i, c);
      }

      // --- unpack (interleave) ---
      auto unpackOp = [&](const Ty* vt, bool high) -> std::optional<Value>
      {
        if (c->args.size() < 2) return std::nullopt;
        std::string a = sseArg(c->args[0], vt, c);
        std::string b = sseArg(c->args[1], vt, c);
        uint32_t half = vt->count / 2;
        uint32_t start = high ? half : 0;
        std::string mask = "<" + std::to_string(vt->count) + " x i32> <";
        for (uint32_t i = 0; i < half; ++i)
        {
          if (i) mask += ", ";
          mask += "i32 " + std::to_string(start + i) + ", i32 " + std::to_string(vt->count + start + i);
        }
        mask += ">";
        std::string r = newTemp();
        fn += "  " + r + " = shufflevector " + ll(vt) + " " + a + ", " + ll(vt) + " " + b + ", " + mask + "\n";
        return sseResult(r, vt, c);
      };

      if (name == "_mm_unpacklo_ps")    return unpackOp(v4f, false);
      if (name == "_mm_unpackhi_ps")    return unpackOp(v4f, true);
      if (name == "_mm_unpacklo_pd")    return unpackOp(v2d, false);
      if (name == "_mm_unpackhi_pd")    return unpackOp(v2d, true);
      if (name == "_mm_unpacklo_epi32") return unpackOp(v4i, false);
      if (name == "_mm_unpackhi_epi32") return unpackOp(v4i, true);
      if (name == "_mm_unpacklo_epi64") return unpackOp(v2i64, false);
      if (name == "_mm_unpackhi_epi64") return unpackOp(v2i64, true);

      // --- shifts ---
      // Immediate shifts: slli, srli, srai
      auto shiftImmOp = [&](const std::string& op, const Ty* vt) -> std::optional<Value>
      {
        if (c->args.size() < 2) return std::nullopt;
        std::string a = sseArg(c->args[0], vt, c);
        std::string imm = sseScalar(c->args[1], types.intType(32), c);
        // Splat the immediate into a vector
        std::string splat = newTemp();
        fn += "  " + splat + " = insertelement " + ll(vt) + " zeroinitializer, i32 " + imm + ", i32 0\n";
        std::string splat2 = newTemp();
        fn += "  " + splat2 + " = shufflevector " + ll(vt) + " " + splat + ", " + ll(vt) + " undef, <" + std::to_string(vt->count) + " x i32> zeroinitializer\n";
        return sseResult(sseBinOp(op, ll(vt), a, splat2), vt, c);
      };

      if (name == "_mm_slli_epi32") return shiftImmOp("shl", v4i);
      if (name == "_mm_srli_epi32") return shiftImmOp("lshr", v4i);
      if (name == "_mm_srai_epi32") return shiftImmOp("ashr", v4i);

      // Variable shifts (by __m128i count, low 64 bits):
      auto shiftVarOp = [&](const std::string& intr, const Ty* vt) -> std::optional<Value>
      {
        if (c->args.size() < 2) return std::nullopt;
        std::string a = sseArg(c->args[0], vt, c);
        std::string b = sseArg(c->args[1], v4i, c);
        std::string i = sseIntrinsic(intr, ll(vt), ll(vt) + ", " + ll(v4i));
        return sseResult(sseCall(i, ll(vt), ll(vt) + " " + a + ", " + ll(v4i) + " " + b), vt, c);
      };

      if (name == "_mm_sll_epi32") return shiftVarOp("llvm.x86.sse2.psll.d", v4i);
      if (name == "_mm_srl_epi32") return shiftVarOp("llvm.x86.sse2.psrl.d", v4i);
      if (name == "_mm_sra_epi32") return shiftVarOp("llvm.x86.sse2.psra.d", v4i);

      // _mm_srli_si128: byte shift right (psrldq)
      if (name == "_mm_srli_si128" && c->args.size() >= 2)
      {
        std::string a = sseArg(c->args[0], v16i8, c);
        auto imm = evalConst(c->args[1], 0);
        if (!imm || *imm < 0 || *imm > 16) return std::nullopt;
        int shift = int(*imm);
        // Build shuffle mask: shift bytes right, fill with zeros
        std::string mask = "<16 x i32> <";
        for (int i = 0; i < 16; ++i)
        {
          if (i) mask += ", ";
          int src = i + shift;
          mask += "i32 " + std::to_string(src < 16 ? src : 16); // 16+ maps to the zero vector
        }
        mask += ">";
        std::string r = newTemp();
        fn += "  " + r + " = shufflevector " + ll(v16i8) + " " + a + ", " + ll(v16i8) + " zeroinitializer, " + mask + "\n";
        // Bitcast back to v4i
        std::string res = newTemp();
        fn += "  " + res + " = bitcast " + ll(v16i8) + " " + r + " to " + ll(v4i) + "\n";
        return sseResult(res, v4i, c);
      }

      // --- widening multiply ---
      if (name == "_mm_mul_epu32" && c->args.size() >= 2)
      {
        std::string a = sseArg(c->args[0], v4i, c);
        std::string b = sseArg(c->args[1], v4i, c);
        std::string i = sseIntrinsic("llvm.x86.sse2.pmulu.dq", ll(v2i64), ll(v4i) + ", " + ll(v4i));
        return sseResult(sseCall(i, ll(v2i64), ll(v4i) + " " + a + ", " + ll(v4i) + " " + b), v2i64, c);
      }

      return std::nullopt; // not an SSE intrinsic we handle
    }

    // The pointer an `->` applies to: the value itself, or what the class's operator-> returns
    // (repeatedly, for wrappers of wrappers).
    Value arrowOperand(Value p, Node* at)
    {
      for (int depth = 0; depth < 8 && isClass(p.ty); ++depth)
      {
        const Ty* foundIn = nullptr;
        if (!findMemberFunction(p.ty, "operator->", {}, p.ty, at, &foundIn))
          break;
        p = callRvalue(operatorCall(p, "operator->", {}, at, true), at);
      }
      return p;
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
          if (!d)
            if (std::optional<int64_t> c = evalConst(e)) // a builtin trait used as a value
              return {e->ty && e->ty->kind == TyKind::Bool ? std::string(*c ? "true" : "false") : std::to_string(*c), e->ty};
          if (d && d->kind == NodeKind::EnumeratorDecl)
            return {std::to_string(enumeratorValue(static_cast<EnumeratorDecl*>(d))), e->ty};
          if (d && d->kind == NodeKind::TemplateParamDecl)
          {
            // A non-type template parameter is a constant of the instantiation.
            std::optional<int64_t> v = evalConst(e);
            const Ty* t = e->ty ? types.stripRef(e->ty) : nullptr;
            if (t && t->kind == TyKind::Value && t->inner)
              t = t->inner;
            if (!v || !t || !t->isKnown())
              unsupported(e, "a template parameter whose value is not known here");
            return {t->kind == TyKind::Bool ? std::string(*v ? "true" : "false") : std::to_string(*v), t};
          }
          if (d && d->kind == NodeKind::FunctionDecl)
          {
            auto* f = static_cast<FunctionDecl*>(d);
            if (f->templateParams.empty() && !global.isTemplateContext(f))
              return {useFunction(f, {}, nullptr), types.pointer(declType(d))};
            // `&FunctionImpl<R, Args...>::call<F>`: a member of an instantiation, with explicit arguments.
            TemplateArgs fargs;
            const Ty* classTy = nullptr;
            if (global.owningClass(f))
            {
              if (n->name.parts.size() > 1)
              {
                NamedType qualifier;
                qualifier.kind = NodeKind::NamedType;
                qualifier.token = n->token;
                qualifier.name = n->name;
                qualifier.name.parts.count -= 1;
                TemplateArgs none;
                const Ty* q = global.typeOfAstIn(&qualifier, currentFunction, currentArgs ? *currentArgs : none);
                if (q && isClass(q))
                  classTy = canonicalClass(q, e);
              }
              else if (currentClassTy)
                classTy = canonicalClass(currentClassTy, e);
              if (!classTy)
                unsupported(e, "taking the address of a member of an unknown instantiation");
              fargs = global.classArgsOf(classTy);
            }
            const NamePart& last = n->name.last();
            size_t given = 0;
            for (size_t i = 0; i < last.templateArgs.size() && i < f->templateParams.size(); ++i)
            {
              Node* arg = last.templateArgs[i];
              if (uint8_t(arg->kind) > uint8_t(NodeKind::PackType))
                unsupported(e, "a non-type argument in the address of a function template");
              TemplateArgs none;
              fargs.pairs.push_back({f->templateParams[i], global.typeOfAstIn(static_cast<Type*>(arg), currentFunction, currentArgs ? *currentArgs : none)});
              ++given;
            }
            if (given < f->templateParams.size())
              unsupported(e, "taking the address of a function template without all its arguments");
            std::string name = useFunction(f, fargs, classTy);
            ArgsScope scope(*this, &fargs);
            return {name, types.pointer(declType(f))};
          }
          if (d && d->kind == NodeKind::VariableDecl && !locals.count(d))
          {
            const Ty* t = types.stripRef(declType(d));
            if (!d->templateParams.empty())
            {
              // A variable template: its value for these arguments, a constant.
              if (auto c = evalConst(e))
                return {t && t->kind == TyKind::Bool ? (*c ? "true" : "false") : std::to_string(*c), t ? types.unqualified(t) : types.intType()};
              unsupported(e, "a variable template whose value is not a constant");
            }
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
          if (const Ty* bt = m->base->ty ? types.stripRef(m->base->ty) : nullptr; bt && bt->kind == TyKind::Member && !m->dynamicMember)
          {
            // A reflection descriptor: m.name is the member's name, m.offset its offset in the class.
            auto* field = static_cast<VariableDecl*>(bt->decl);
            std::string which = m->member.empty() ? std::string() : sym(m->member.last().symbol);
            if (which == "name")
              return {cStringConstant(field->name.empty() ? std::string() : sym(field->name.last().symbol)), e->ty};
            if (which == "offset")
            {
              const Ty* cls = bt->args.empty() ? nullptr : bt->args[0];
              std::string addr;
              if (!cls || !fieldAddress(cls, field, "null", addr, e))
                unsupported(e, "the offset of a member that is not a field");
              std::string r = newTemp();
              fn += "  " + r + " = ptrtoint ptr " + addr + " to i64\n";
              return {r, e->ty};
            }
            unsupported(e, "a member descriptor has name and offset only");
          }
          std::string addr = lvalue(e);
          if (m->resolved && bitfieldOf.count(m->resolved))
            return loadBitfield(addr, bitfieldOf[m->resolved], types.stripRef(e->ty), e);
          return load(addr, e->ty, e);
        }
        case NodeKind::IndexExpr:
        {
          auto* i = static_cast<IndexExpr*>(e);
          Value base = rvalue(i->base);
          if (ClassDecl* c = typedUnionOf(base.ty, e))
          {
            std::string addr = alternativeLvalue(c, base, i, e, true);
            const Ty* altTy = types.stripRef(e->ty);
            return isClass(altTy) ? Value{addr, altTy, false} : load(addr, altTy, e);
          }
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
        case NodeKind::TypeidExpr:
        {
          auto* t = static_cast<TypeidExpr*>(e);
          const Ty* infoTy = e->ty ? types.stripRef(e->ty) : nullptr;
          if (!isClass(infoTy))
            unsupported(e, "typeid without the library's TypeInfo class");
          std::string namePtr;
          const Ty* staticTy = nullptr;
          if (t->operandDecl)
          {
            // typeid(q): the parser read the variable as a type name; the resolver found it.
            const Ty* xt = t->operandType ? types.stripRef(t->operandType) : nullptr;
            if (isClass(xt) && structFor(xt, e).polymorphic)
              namePtr = descriptorName(loadSlot(addressOfDecl(t->operandDecl, e), -1));
            else
              staticTy = xt;
          }
          else if (t->operand && uint8_t(t->operand->kind) <= uint8_t(NodeKind::PackType))
            staticTy = t->operandType ? types.stripRef(t->operandType) : nullptr; // typeid(Widget): typed by the resolver
          else if (t->operand)
          {
            auto* x = static_cast<Expr*>(t->operand);
            Decl* named = x->kind == NodeKind::NameExpr ? static_cast<NameExpr*>(x)->resolved : nullptr;
            const Ty* xt = x->ty ? types.stripRef(x->ty) : nullptr;
            if (named && named->kind == NodeKind::ClassDecl)
              staticTy = canonicalClass(types.classType(named, {}), e); // typeid(Base): the parser read the type as a name
            else if (named && named->kind == NodeKind::EnumDecl)
              staticTy = types.enumType(named);
            else if (isClass(xt) && structFor(xt, e).polymorphic)
              namePtr = descriptorName(loadSlot(lvalue(x), -1)); // the dynamic type: slot 0 of the object's vtable
            else
              staticTy = xt;
          }
          if (namePtr.empty())
          {
            if (!staticTy || !staticTy->isKnown() || types.isDependent(staticTy))
              unsupported(e, "typeid of an unknown type (operand: " + std::string(t->operand ? nodeKindName(t->operand->kind) : "none") + ", " +
                                 (staticTy ? types.toString(staticTy, &symbols) : std::string("no type")) + ")");
            namePtr = cStringConstant(types.toString(types.unqualified(staticTy), &symbols));
          }
          std::string slot = newObjectSlot(infoTy, e);
          StructInfo info = structFor(infoTy, e);
          if (info.fieldDecls.empty())
            unsupported(e, "a TypeInfo class without its name field");
          fn += "  store ptr " + namePtr + ", ptr " + gep(info.name, slot, info.fieldIndex[info.fieldDecls[0]]) + "\n";
          return {slot, infoTy, true};
        }
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
          if (e->ty && e->ty->kind == TyKind::Void)
          {
            rvalue(static_cast<CastExpr*>(e)->operand); // `(void)x`: evaluated, result dropped
            return {"", types.voidType()};
          }
          return explicitConvert(rvalue(static_cast<CastExpr*>(e)->operand), e->ty, e);
        case NodeKind::NamedCastExpr:
        {
          auto* c = static_cast<NamedCastExpr*>(e);
          if (c->castKind == Kw::dynamic_cast_)
            if (std::optional<Value> v = dynamicCast(c, e))
              return *v;
          return explicitConvert(rvalue(c->operand), e->ty, e);
        }
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
          if (c->args.size() == 1 && isPackExpansion(c->args[0]))
          {
            // `T(forward<Args>(args)...)` with a scalar T: the pack's one element, re-typed for it
            // (so that `forward<Args>` means forward<that element>).
            Expr* pattern = static_cast<UnaryExpr*>(c->args[0])->operand;
            size_t count = packElementCount(pattern, e);
            if (count == 0)
              return {t->kind == TyKind::Float ? "0.0" : t->kind == TyKind::Pointer ? "null" : "0", t};
            if (count != 1)
              unsupported(e, "a scalar built from a pack of several elements");
            Value v;
            forEachPackElement(pattern, e, [&](size_t) { v = convert(rvalue(pattern), t, e); });
            return v;
          }
          if (c->args.size() == 1)
          {
            Expr* a = c->args[0];
            if (a->kind == NodeKind::BraceInitExpr && static_cast<BraceInitExpr*>(a)->args.empty())
              return {t->kind == TyKind::Float ? "0.0" : t->kind == TyKind::Pointer ? "null" : "0", t}; // `mapped_type{}`: zero
            if (a->kind == NodeKind::BraceInitExpr && static_cast<BraceInitExpr*>(a)->args.size() == 1)
              a = static_cast<BraceInitExpr*>(a)->args[0];
            return convert(rvalue(a), t, e);
          }
          if (c->args.empty())
            return {t->kind == TyKind::Float ? "0.0" : t->kind == TyKind::Pointer ? "null" : "0", t};
          unsupported(e, "this functional cast");
        }
        case NodeKind::RequiresExpr:
          return {static_cast<RequiresExpr*>(e)->satisfied ? "true" : "false", types.boolType()};
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
            if (param && param->kind == NodeKind::TemplateParamDecl && currentArgs)
              for (auto& [p, t] : currentArgs->pairs)
                if (p == param && t && t->kind == TyKind::Pack)
                  return {std::to_string(t->args.size()), types.sizeType()}; // `sizeof...(Args)` of a template pack
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
          Value cond = convert(rvalue(c->cond), types.boolType(), e);
          std::string tL = newLabel("cond.then"), eL = newLabel("cond.else"), jL = newLabel("cond.end");
          fn += "  br i1 " + cond.text + ", label %" + tL + ", label %" + eL + "\n";
          auto namesObject = [&](Expr* x) { return x && (isLvalueExpression(x) || (x->ty && x->ty->isReference())); };
          if (isClass(t) && namesObject(c->then) && namesObject(c->else_))
          {
            // Both branches name objects (`cond ? a : b` with lvalues or references): the result
            // is one of them, not a copy.
            size_t temps = statementTemps.size();
            startBlock(tL);
            std::string a = lvalue(c->then);
            destroyTempsFrom(temps);
            std::string aEnd = currentBlock;
            emitBranch(jL);
            startBlock(eL);
            std::string b = lvalue(c->else_);
            destroyTempsFrom(temps);
            std::string bEnd = currentBlock;
            emitBranch(jL);
            startBlock(jL);
            std::string r = newTemp();
            fn += "  " + r + " = phi ptr [ " + a + ", %" + aEnd + " ], [ " + b + ", %" + bEnd + " ]\n";
            return {r, t, false};
          }
          if (isClass(t))
          {
            // Both branches build the one temporary (a copy of an lvalue, or the value itself).
            std::string slot = newObjectSlot(t, e);
            size_t temps = statementTemps.size();
            startBlock(tL);
            initializeFrom(slot, t, c->then, e);
            destroyTempsFrom(temps);
            emitBranch(jL);
            startBlock(eL);
            initializeFrom(slot, t, c->else_, e);
            destroyTempsFrom(temps);
            emitBranch(jL);
            startBlock(jL);
            if (needsDestruction(t, e))
              statementTemps.push_back({slot, t});
            return {slot, t, true};
          }
          size_t temps = statementTemps.size();
          startBlock(tL);
          Value a = t->kind == TyKind::Void ? rvalue(c->then) : convert(rvalue(c->then), t, e);
          destroyTempsFrom(temps); // `number(isNumber ? strtodouble(String(data, length)) : 0)`: the String exists only here
          std::string aEnd = currentBlock;
          emitBranch(jL);
          startBlock(eL);
          Value b = t->kind == TyKind::Void ? rvalue(c->else_) : convert(rvalue(c->else_), t, e);
          destroyTempsFrom(temps);
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
            emitCall(r + " = ", "ptr @malloc(i64 " + std::to_string(sizeOf(t, e)) + ")", false);
          }
          if (isClass(t))
            constructInto(r, t, n->args, e);
          else if (isLambda(t) && n->args.size() == 1)
            initializeFrom(r, t, n->args[0], e); // `new F(callable)` with F a lambda type: the closure, copied
          else if (n->args.size() == 1 && isPackExpansion(n->args[0]))
          {
            // `new (p) T(forward<Args>(args)...)` with a scalar T: the pack's one element.
            Expr* pattern = static_cast<UnaryExpr*>(n->args[0])->operand;
            size_t count = packElementCount(pattern, e);
            if (count == 0)
              fn += "  store " + llType(t, e) + " zeroinitializer, ptr " + r + "\n";
            else if (count != 1)
              unsupported(e, "a scalar built from a pack of several elements");
            else
              forEachPackElement(pattern, e, [&](size_t) { store(rvalue(pattern), r, t, e); });
          }
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
        case NodeKind::ThrowExpr:
          emitThrow(static_cast<ThrowExpr*>(e));
          return {"", types.voidType()};
        case NodeKind::DeleteExpr:
        {
          auto* d = static_cast<DeleteExpr*>(e);
          Value p = rvalue(d->operand);
          if (p.ty->kind != TyKind::Pointer)
            unsupported(e, "delete of a non-pointer");
          if (isClass(p.ty->inner))
            destroy(p.text, p.ty->inner, e, true);
          ensureMalloc();
          emitCall("", "void @free(ptr " + p.text + ")", false);
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
      if (op == P::Amp && u->ty && types.stripRef(u->ty)->kind == TyKind::MemberPointer)
      {
        // `&Style::field`: the field's offset within the class.
        auto* n = static_cast<NameExpr*>(u->operand);
        Decl* d = n->resolved;
        const Ty* mp = types.stripRef(u->ty);
        const Ty* cls = mp->args.empty() ? nullptr : mp->args[0];
        if (d && d->kind == NodeKind::FunctionDecl && cls && isClass(cls))
        {
          // `&Calc::mul`: the function itself; a call through it passes the object first.
          auto* f = static_cast<FunctionDecl*>(d);
          if (f->flags & DF_Virtual)
            unsupported(u, "a pointer to a virtual member function");
          const Ty* key = canonicalClass(cls, u);
          return {useFunction(f, global.classArgsOf(key), key), mp};
        }
        if (!d || d->kind != NodeKind::VariableDecl || !cls || !isClass(cls))
          unsupported(u, "a pointer to a member that is neither a field nor a function");
        std::string addr;
        if (!fieldAddress(cls, d, "null", addr, u))
          unsupported(u, "a pointer to a member that is not a field");
        std::string r = newTemp();
        fn += "  " + r + " = ptrtoint ptr " + addr + " to i64\n";
        return {r, mp};
      }
      if (op == P::Amp && u->operand->kind == NodeKind::NameExpr && static_cast<NameExpr*>(u->operand)->resolved &&
          static_cast<NameExpr*>(u->operand)->resolved->kind == NodeKind::FunctionDecl)
        return {rvalue(u->operand).text, u->ty}; // `&function`: the function itself is the pointer
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
      {
        const Ty* in = nullptr;
        if (op == P::Bang && !findMemberFunction(v.ty, "operator!", {}, v.ty, u, &in))
        {
          // No operator!: the class's conversion to bool, negated.
          Value b = convert(v, types.boolType(), u);
          std::string r = newTemp();
          fn += "  " + r + " = xor i1 " + b.text + ", true\n";
          return {r, types.boolType()};
        }
        return callRvalue(operatorCall(v, operatorName(op), {}, u, true), u);
      }
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
      if (op == P::DotStar || op == P::ArrowStar)
      {
        std::string addr = memberPointerAddress(b);
        const Ty* mt = b->ty ? types.stripRef(b->ty) : nullptr;
        if (!mt || !mt->isKnown())
          unsupported(b, "a member pointer of unknown type");
        return load(addr, mt, b);
      }
      const Ty* lt = types.stripRef(b->lhs->ty);
      const Ty* rt = b->rhs && b->rhs->ty ? types.stripRef(b->rhs->ty) : nullptr;
      if (op == P::Assign && b->lhs->kind == NodeKind::IndexExpr && static_cast<IndexExpr*>(b->lhs)->base->ty &&
          typedUnionOf(types.stripRef(static_cast<IndexExpr*>(b->lhs)->base->ty), b))
        return assignAlternative(static_cast<IndexExpr*>(b->lhs), b->rhs, b); // `p[Integer] = 5`: the tag and the value
      if ((op == P::EqEq || op == P::NotEq) &&
          ((typedUnionOf(lt, b) && rt && rt->kind == TyKind::Enum) || (typedUnionOf(rt, b) && lt && lt->kind == TyKind::Enum)))
        return compareTag(b, op == P::NotEq);
      if (isClass(lt) || isClass(rt))
      {
        std::string name = operatorName(op);
        Expr* rhsExpr = b->rhs;
        Span<Expr*> one{&rhsExpr, 1};
        const Ty* foundIn = nullptr;
        if (isClass(lt) && findMemberFunction(types.unqualified(lt), name, argTypesOf(one), types.unqualified(lt), b, &foundIn, true))
          return callRvalue(operatorCall(rvalue(b->lhs), name, one, b, true), b);
        // A free `operator+(const char*, const String&)` and the like.
        if (b->lhs->ty && b->rhs && b->rhs->ty && b->lhs->ty->isKnown() && b->rhs->ty->isKnown())
          if (FunctionDecl* f = global.findFreeOperator(uint32_t(op), b->lhs->ty, b->rhs->ty, currentFunction); f && f->templateParams.empty())
          {
            Expr* args[2] = {b->lhs, b->rhs};
            return callRvalue(callFunction(f, {}, nullptr, "", "", Span<Expr*>{args, 2}, b), b);
          }
        if (std::optional<Value> rewritten = rewrittenComparison(b, op, lt, rt))
          return *rewritten;
        if (!isClass(lt))
          unsupported(b, "an operator with a class on the right that no function defines");
        Value l = rvalue(b->lhs);
        if (op == P::Assign)
        {
          const Ty* lt0 = types.unqualified(types.stripRef(l.ty));
          const Ty* rt0 = b->rhs->ty && b->rhs->ty->isKnown() ? types.stripRef(b->rhs->ty) : nullptr;
          if (rt0 && !isClass(rt0) && findConstructor(lt0, {argumentType(b->rhs, b->rhs->ty, types)}, b))
          {
            // `flag = InvertColors::True`: the converting constructor makes a temporary to assign from.
            std::string tmp = convertedTemporary(lt0, b->rhs, b);
            assignObject(l.text, tmp, l.ty, b, true);
            return l;
          }
          if (rt0 && isClass(rt0) && classKey(rt0) != classKey(lt0) && !baseInstantiation(rt0, classDef(lt0->decl, b), b) &&
              (conversionOperator(rt0, lt0, b) || findConstructor(lt0, {argumentType(b->rhs, b->rhs->ty, types)}, b)))
          {
            // `view = string`: StringView's converting constructor makes the temporary to assign from.
            std::string tmp = convertedTemporary(lt0, b->rhs, b);
            assignObject(l.text, tmp, l.ty, b, true);
            return l;
          }
          if (b->rhs->kind == NodeKind::BraceInitExpr)
          {
            // `*this = {}` or `this->data = {a, b}`: construct a temporary of the LHS type from the brace args.
            auto* bi = static_cast<BraceInitExpr*>(b->rhs);
            std::string tmp = newObjectSlot(lt0, b);
            constructInto(tmp, lt0, bi->args, b);
            assignObject(l.text, tmp, lt0, b, true);
            return l;
          }
          Value r = rvalue(b->rhs);
          if (!isClass(r.ty))
            unsupported(b, "assigning a non-class to a class");
          std::string src = r.text;
          if (classKey(r.ty) != classKey(l.ty))
            if (!baseAddress(r.ty, l.ty, r.text, src, b))
              unsupported(b, "assigning an unrelated class");
          assignObject(l.text, src, l.ty, b, r.isTemporary || isXvalue(b->rhs));
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
        const BitfieldInfo* bits = nullptr;
        if (b->lhs->kind == NodeKind::MemberExpr)
          if (Decl* d = static_cast<MemberExpr*>(b->lhs)->resolved; d && bitfieldOf.count(d))
            bits = &bitfieldOf[d];
        if (op != P::Assign)
        {
          Value old = bits ? loadBitfield(addr, *bits, t, b) : load(addr, t, b);
          rhs = arithmetic(baseOp(op), old, rhs, b);
        }
        if (bits)
          storeBitfield(convert(rhs, t, b), addr, *bits, t, b);
        else
          store(rhs, addr, t, b);
        return convert(rhs, t, b);
      }
      Value l = rvalue(b->lhs);
      Value r = rvalue(b->rhs);
      switch (op)
      {
        case P::EqEq: case P::NotEq: case P::Less: case P::LessEq: case P::Greater: case P::GreaterEq:
          return compare(op, l, r, b);
        case P::Spaceship:
          return orderingObject(orderOfScalars(l, r, b), b);
        default:
          return arithmetic(op, l, r, b);
      }
    }

    // --- three-way comparison ---

    // The library's strong_ordering (what `a <=> b` yields), or null without the library.
    const Ty* orderingType()
    {
      Decl* d = global.strongOrderingDecl();
      return d ? types.classType(d, {}) : nullptr;
    }

    // The order of two scalars as an i8: -1, 0 or 1.
    Value orderOfScalars(Value l, Value r, Node* at)
    {
      Value less = compare(P::Less, l, r, at);
      Value greater = compare(P::Greater, l, r, at);
      std::string g = newTemp(), res = newTemp();
      fn += "  " + g + " = select i1 " + greater.text + ", i8 1, i8 0\n";
      fn += "  " + res + " = select i1 " + less.text + ", i8 -1, i8 " + g + "\n";
      return {res, types.intType(8)};
    }

    // An order (i8) as the value of `a <=> b`: a strong_ordering object, or an int without the library.
    Value orderingObject(Value order, Node* at)
    {
      const Ty* ot = orderingType();
      if (!ot)
        return convert(order, types.intType(), at);
      std::string slot = newObjectSlot(ot, at);
      fn += "  store i8 " + order.text + ", ptr " + gep(structFor(ot, at).name, slot, 0) + "\n";
      return {slot, ot, true};
    }

    // The i8 inside the value of `a <=> b`.
    Value orderOf(Value ordering, Node* at)
    {
      const Ty* t = types.stripRef(ordering.ty);
      if (isClass(t))
        return load(gep(structFor(t, at).name, ordering.text, 0), types.intType(8), at);
      return convert(ordering, types.intType(8), at);
    }

    // `a <=> b` of two objects at those addresses: the class's operator<=> (memberwise when
    // defaulted); the order as an i8.
    Value orderOfObjects(const std::string& a, const std::string& b, const Ty* t, Node* at)
    {
      const Ty* foundIn = nullptr;
      std::vector<const Ty*> argTypes{types.lref(types.qualified(t, true, false))};
      FunctionDecl* op = findMemberFunction(t, "operator<=>", argTypes, t, at, &foundIn);
      if (!op)
        unsupported(at, global.qualifiedName(t->decl) + " has no operator<=> (a defaulted one orders by the members)");
      if (op->flags & DF_Default)
        return memberwiseOrder(a, b, t, at);
      TemplateArgs args = global.classArgsOf(foundIn);
      Signature s;
      {
        ArgsScope scope(*this, &args);
        s = signatureOf(op);
      }
      std::string name = useFunction(op, args, foundIn);
      std::string thisArg = a;
      if (canonicalClass(t, at) != foundIn)
        baseAddress(t, foundIn, a, thisArg, at);
      if (s.returnType == "void")
      {
        const Ty* ot = orderingType();
        if (!ot)
          unsupported(at, "operator<=> returning an object without the library's strong_ordering");
        std::string tmp = newObjectSlot(ot, at); // the result object: this, sret, other
        emitCall("", "void " + name + "(ptr " + thisArg + ", ptr " + tmp + ", ptr " + b + ")");
        return orderOf(Value{tmp, ot, true}, at);
      }
      std::string r = newTemp();
      emitCall(r + " = ", s.returnType + " " + name + "(ptr " + thisArg + ", ptr " + b + ")");
      return convert(Value{r, types.intType(32), false}, types.intType(8), at);
    }

    // The order of two objects base by base and member by member: the first that differs decides.
    Value memberwiseOrder(const std::string& a, const std::string& b, const Ty* t, Node* at)
    {
      StructInfo info = structFor(t, at);
      std::string slot = "%order" + std::to_string(temp++);
      allocas += "  " + slot + " = alloca i8\n";
      fn += "  store i8 0, ptr " + slot + "\n";
      std::string done = newLabel("order.done");
      auto part = [&](Value order)
      {
        fn += "  store i8 " + order.text + ", ptr " + slot + "\n";
        std::string ne = newTemp(), next = newLabel("order.next");
        fn += "  " + ne + " = icmp ne i8 " + order.text + ", 0\n";
        fn += "  br i1 " + ne + ", label %" + done + ", label %" + next + "\n";
        blockTerminated = true;
        startBlock(next);
      };
      for (auto& [baseDef, index] : info.bases)
        part(orderOfObjects(gep(info.name, a, index), gep(info.name, b, index), info.fieldTypes[index], at));
      for (Decl* d : info.fieldDecls)
      {
        uint32_t index = info.fieldIndex[d];
        const Ty* ft = info.fieldTypes[index];
        std::string fa = gep(info.name, a, index), fb = gep(info.name, b, index);
        if (isClass(ft))
          part(orderOfObjects(fa, fb, ft, at));
        else if (ft->kind == TyKind::Array)
          unsupported(at, "a defaulted comparison of an array member");
        else
          part(orderOfScalars(load(fa, ft, at), load(fb, ft, at), at));
      }
      emitBranch(done);
      startBlock(done);
      return load(slot, types.intType(8), at);
    }

    // The comparisons C++ rewrites when the class defines no operator for them: `a != b` as
    // `!(a == b)`, `a < b` and the other orderings as `(a <=> b) < 0`, and `==` of a class whose
    // operator<=> is defaulted as `(a <=> b) == 0`. Null when the class has none of those either.
    std::optional<Value> rewrittenComparison(BinaryExpr* b, P op, const Ty* lt, const Ty* rt)
    {
      Expr* rhsExpr = b->rhs;
      Span<Expr*> one{&rhsExpr, 1};
      auto reversed = [&]() -> std::optional<Value>
      {
        // `3 == id` through Id::operator==(int): the operands the other way round.
        if (!rt || !isClass(rt))
          return std::nullopt;
        Expr* lhsExpr = b->lhs;
        Span<Expr*> left{&lhsExpr, 1};
        const Ty* foundIn = nullptr;
        if (findMemberFunction(types.unqualified(rt), "operator==", argTypesOf(left), types.unqualified(rt), b, &foundIn, true))
          return callRvalue(operatorCall(rvalue(b->rhs), "operator==", left, b, true), b);
        return std::nullopt;
      };
      auto call = [&](P which) -> std::optional<Value>
      {
        std::string name = operatorName(which);
        const Ty* foundIn = nullptr;
        if (isClass(lt) && findMemberFunction(types.unqualified(lt), name, argTypesOf(one), types.unqualified(lt), b, &foundIn, true))
          return callRvalue(operatorCall(rvalue(b->lhs), name, one, b, true), b);
        if (b->lhs->ty && b->rhs && b->rhs->ty && b->lhs->ty->isKnown() && b->rhs->ty->isKnown())
          if (FunctionDecl* f = global.findFreeOperator(uint32_t(which), b->lhs->ty, b->rhs->ty, currentFunction); f && f->templateParams.empty())
          {
            Expr* args[2] = {b->lhs, b->rhs};
            return callRvalue(callFunction(f, {}, nullptr, "", "", Span<Expr*>{args, 2}, b), b);
          }
        return std::nullopt;
      };
      auto threeWayDefaulted = [&]()
      {
        const Ty* foundIn = nullptr;
        FunctionDecl* f = isClass(lt) ? findMemberFunction(types.unqualified(lt), "operator<=>", argTypesOf(one), types.unqualified(lt), b, &foundIn, true) : nullptr;
        return f && (f->flags & DF_Default);
      };
      auto orderCompare = [&](P cmp) -> std::optional<Value>
      {
        std::optional<Value> ordering = call(P::Spaceship);
        if (!ordering)
          return std::nullopt;
        return compare(cmp, orderOf(*ordering, b), Value{"0", types.intType(8)}, b);
      };
      switch (op)
      {
        case P::NotEq:
        {
          std::optional<Value> eq = call(P::EqEq);
          if (!eq && threeWayDefaulted())
            return orderCompare(P::NotEq);
          if (!eq)
            eq = reversed();
          if (!eq)
            return std::nullopt;
          std::string r = newTemp();
          fn += "  " + r + " = xor i1 " + convert(*eq, types.boolType(), b).text + ", true\n";
          return Value{r, types.boolType()};
        }
        case P::EqEq:
          if (threeWayDefaulted())
            return orderCompare(P::EqEq);
          return reversed();
        case P::Less: case P::Greater: case P::LessEq: case P::GreaterEq:
          return orderCompare(op);
        default:
          return std::nullopt;
      }
    }

    // The address `object.*member` / `pointer->*member` denotes: the object plus the offset.
    std::string memberPointerAddress(BinaryExpr* b)
    {
      Value object = rvalue(b->lhs);
      std::string base = object.text;
      if (P(b->op) == P::ArrowStar)
      {
        if (object.ty->kind != TyKind::Pointer)
          unsupported(b, "->* on something that is not a pointer");
      }
      else if (!isClass(object.ty))
        unsupported(b, ".* on something that is not an object");
      Value offset = rvalue(b->rhs);
      if (!offset.ty || types.stripRef(offset.ty)->kind != TyKind::MemberPointer)
        unsupported(b, "a member pointer of unknown type");
      std::string r = newTemp();
      fn += "  " + r + " = getelementptr inbounds i8, ptr " + base + ", i64 " + offset.text + "\n";
      return r;
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
      size_t temps = statementTemps.size();
      Value r = convert(rvalue(rhs), types.boolType(), at);
      destroyTempsFrom(temps); // `flag && Noisy(2).value() > 5`: the Noisy exists only here
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
        destroyObject(statementTemps[i]);
      statementTemps.clear();
    }

    // Destroys the temporaries made since mark: those of one branch of `?:` or of the right
    // operand of `&&`/`||`, which exist only when that branch ran, so they die inside it.
    void destroyTempsFrom(size_t mark)
    {
      for (size_t i = statementTemps.size(); i-- > mark;)
        destroyObject(statementTemps[i]);
      statementTemps.resize(mark);
    }

    void destroyObject(const Object& o)
    {
      if (o.exception)
        fn += "  call void @kov_eh_end(ptr " + o.addr + ")" + bundle() + "\n";
      else
        destroy(o.addr, o.ty, nullptr);
    }

    void pushScope() { scopes.emplace_back(); }

    void popScope()
    {
      if (!blockTerminated)
        for (size_t i = scopes.back().size(); i-- > 0;)
          destroyObject(scopes.back()[i]);
      scopes.pop_back();
    }

    // Destroys the objects of the innermost scopes down to (not including) depth.
    void unwindTo(size_t depth) { unwindRange(depth, scopes.size()); }

    // Destroys the objects of scopes [low, high), innermost first; the scopes stay open.
    void unwindRange(size_t low, size_t high)
    {
      for (size_t s = high; s-- > low;)
        for (size_t i = scopes[s].size(); i-- > 0;)
          destroyObject(scopes[s][i]);
    }

    // Destroys the scopes above depth for a `return`, `break` or `continue`, leaving the catch
    // handlers in between through catchret: a funclet cannot branch out directly.
    void leaveScopes(size_t depth)
    {
      std::vector<EHFrame> saved = ehFrames;
      size_t top = scopes.size();
      for (size_t i = ehFrames.size(); i-- > 0;)
      {
        if (!ehFrames[i].isHandler)
          continue;
        if (ehFrames[i].scopeDepth < depth)
          break; // the target is inside this handler
        unwindRange(ehFrames[i].scopeDepth, top);
        top = ehFrames[i].scopeDepth;
        std::string next = newLabel("catchret");
        fn += "  catchret from " + ehFrames[i].funclet + " to label %" + next + "\n";
        ehFrames.resize(i);
        startBlock(next);
      }
      unwindRange(depth, top);
      ehFrames = saved;
    }

    // --- exceptions ---

    // The catchpad token of the handler the code being emitted belongs to, or "" outside handlers.
    std::string currentFunclet()
    {
      if (!funcletOverride.empty())
        return funcletOverride;
      for (size_t i = ehFrames.size(); i-- > 0;)
        if (ehFrames[i].isHandler)
          return ehFrames[i].funclet;
      return "";
    }

    // The operand bundle a call inside a funclet carries.
    std::string bundle()
    {
      std::string f = currentFunclet();
      return f.empty() ? "" : " [ \"funclet\"(token " + f + ") ]";
    }

    void markEH()
    {
      usesEH = true;
      if (!declaredNames["@__C_specific_handler"])
      {
        decls += "declare i32 @__C_specific_handler(...)\n";
        declaredNames["@__C_specific_handler"] = true;
      }
    }

    // Emits `call` or, when an exception leaving the call has to run destructors or reach a handler
    // of this function, `invoke`. prefix is "" or "%r = "; callee is the text after `call`.
    void emitCall(const std::string& prefix, const std::string& callee, bool mayThrow = true)
    {
      std::string dest = mayThrow && funcletOverride.empty() ? unwindDestination() : std::string();
      if (dest.empty())
      {
        fn += "  " + prefix + "call " + callee + bundle() + "\n";
        return;
      }
      markEH();
      std::string cont = newLabel("invoke.cont");
      fn += "  " + prefix + "invoke " + callee + bundle() + " to label %" + cont + " unwind label %" + dest + "\n";
      startBlock(cont);
    }

    // Where an exception thrown at this point unwinds to: a cleanup pad destroying the objects
    // constructed since the innermost try or handler began (shared by all calls with the same
    // objects), the try's dispatch, or "" when nothing in this function has to happen.
    std::string unwindDestination()
    {
      size_t limit = ehFrames.empty() ? 0 : ehFrames.back().scopeDepth;
      std::vector<Object> objects;
      for (size_t i = statementTemps.size(); i-- > 0;)
        objects.push_back(statementTemps[i]);
      for (size_t s = scopes.size(); s-- > limit;)
        for (size_t i = scopes[s].size(); i-- > 0;)
          objects.push_back(scopes[s][i]);
      std::string next; // "" = out of the function or funclet
      if (!ehFrames.empty() && !ehFrames.back().isHandler)
      {
        next = ehFrames.back().dispatchLabel;
        if (objects.empty())
          return ehFrames.back().used = true, next;
      }
      if (objects.empty())
        return "";
      std::string parent = currentFunclet();
      std::string key = next + "|" + parent;
      for (const Object& o : objects)
        key += "|" + o.addr;
      auto it = cleanupPads.find(key);
      if (it != cleanupPads.end())
        return it->second;
      markEH();
      if (!next.empty())
        ehFrames.back().used = true;
      std::string padLabel = newLabel("cleanup"), pad = newTemp();
      cleanupPads[key] = padLabel;
      // The pad is emitted aside, after the function body.
      std::string savedFn = std::move(fn), savedBlock = currentBlock, savedOverride = funcletOverride;
      bool savedTerminated = blockTerminated;
      fn.clear();
      startBlock(padLabel);
      fn += "  " + pad + " = cleanuppad within " + (parent.empty() ? "none" : parent) + " []\n";
      funcletOverride = pad;
      for (const Object& o : objects)
        destroyObject(o);
      funcletOverride = savedOverride;
      fn += "  cleanupret from " + pad + " unwind " + (next.empty() ? "to caller" : "label %" + next) + "\n";
      ehBlocks += fn;
      fn = std::move(savedFn);
      currentBlock = savedBlock;
      blockTerminated = savedTerminated;
      return padLabel;
    }

    std::string sharedComdat(const std::string& name) { return "$" + name.substr(1) + " = comdat any\n"; }

    // The type descriptor matched against catch clauses: { base descriptor, name, base offset }.
    std::string typeinfoFor(const Ty* t, Node* at)
    {
      t = types.unqualified(types.stripRef(t));
      std::string name, base = "null", offset = "0", display;
      if (isClass(t))
      {
        StructInfo info = structFor(t, at); // copy: the base's descriptor may add structs
        name = "@kov.ti." + info.name.substr(8);
        display = types.toString(t, &symbols);
        if (!info.bases.empty())
        {
          base = typeinfoFor(info.fieldTypes[info.bases[0].second], at);
          offset = "ptrtoint (ptr getelementptr (" + info.name + ", ptr null, i32 0, i32 " + std::to_string(info.bases[0].second) + ") to i64)";
        }
        if (display == "Exception" && exceptionTypeinfo.empty())
          exceptionTypeinfo = name;
      }
      else
      {
        display = types.toString(t, &symbols);
        name = "@\"kov.ti." + display + "\""; // one descriptor per scalar type, pointers by pointee
      }
      if (emittedShared[name])
        return name;
      emittedShared[name] = true;
      bool was = emittingShared;
      emittingShared = true;
      std::string str = cStringConstant(display);
      emittingShared = was;
      shared += sharedComdat(name) + name + " = linkonce_odr constant { ptr, ptr, i64 } { ptr " + base + ", ptr " + str + ", i64 " +
                offset + " }, comdat\n";
      return name;
    }

    // A function destroying an object of type t through a pointer (for the exception runtime), or null.
    std::string destructorThunk(const Ty* t, Node* at)
    {
      if (!needsDestruction(t, at))
        return "null";
      std::string name = "@kov.dtor." + structFor(t, at).name.substr(8);
      if (emittedShared[name])
        return name;
      emittedShared[name] = true;
      FunctionState saved = saveState();
      std::string savedOverride = funcletOverride;
      funcletOverride.clear();
      resetFunctionState();
      try
      {
        destroy("%o", t, at);
        fn += "  ret void\n";
        shared += sharedComdat(name) + "define linkonce_odr void " + name + "(ptr %o) comdat {\nentry:\n" + allocas + fn + "}\n";
      }
      catch (Unsupported&)
      {
        restoreState(saved);
        funcletOverride = savedOverride;
        throw;
      }
      restoreState(saved);
      funcletOverride = savedOverride;
      return name;
    }

    // `throw expr`: the object is built on the heap and raised with its descriptor and destructor.
    // `throw;` raises the innermost handler's exception again, which then no longer owns it. The
    // runtime's raise functions are declared as returning (they never do), see below.
    void emitThrow(ThrowExpr* t)
    {
      usesExceptions = true;
      if (!t->operand)
      {
        const EHFrame* handler = nullptr;
        for (size_t i = ehFrames.size(); i-- > 0 && !handler;)
          if (ehFrames[i].isHandler)
            handler = &ehFrames[i];
        if (!handler)
          unsupported(t, "`throw;` outside a catch handler");
        std::string slot = handler->exceptionSlot;
        destroyTemps();
        emitCall("", "void @kov_eh_rethrow(ptr " + slot + ")");
      }
      else
      {
        const Ty* ty = t->operand->ty ? types.unqualified(types.stripRef(t->operand->ty)) : nullptr;
        if (!ty || !ty->isKnown() || types.isDependent(ty))
          unsupported(t, "throwing a value of unknown type");
        if (ty->kind == TyKind::Array)
          ty = types.pointer(ty->inner);
        ensureMalloc();
        std::string obj = newTemp();
        emitCall(obj + " = ", "ptr @malloc(i64 " + std::to_string(sizeOf(ty, t)) + ")", false);
        if (isClass(ty))
          initializeFrom(obj, ty, t->operand, t);
        else
          store(convert(rvalue(t->operand), ty, t), obj, ty, t);
        std::string ti = typeinfoFor(ty, t), dtor = destructorThunk(ty, t);
        destroyTemps();
        emitCall("", "void @kov_throw(ptr " + obj + ", ptr " + ti + ", ptr " + dtor + ")");
      }
      // No `unreachable` here, on purpose: the code after a throw stays, so that the optimizer never
      // proves a function noreturn. The SEH scope tables need an instruction after every throwing
      // call; a catch handler placed right at a call's return address would make the runtime skip
      // the cleanups between them, taking the unwind for a jump within the scope.
    }

    // try { body } catch (T1 a) {...} catch (...) {...}: the body's calls unwind to a catchswitch
    // whose filter function (run by the OS during the search phase) matches the thrown type against
    // the handlers in order and records the exception for the one that takes it. The handler code
    // is a catchpad funclet reading that record; leaving it normally ends the exception.
    void tryStatement(TryStmt* t)
    {
      usesExceptions = true;
      markEH();
      std::string endL = newLabel("try.end"), dispatchL = newLabel("catch.dispatch"), handlerL = newLabel("catch.pad");
      ehFrames.push_back({false, dispatchL, currentFunclet(), "", "", scopes.size()});
      statement(t->body);
      emitBranch(endL);
      EHFrame frame = ehFrames.back();
      ehFrames.pop_back();
      if (!frame.used)
      {
        startBlock(endL); // nothing in the body can throw to the handlers
        return;
      }
      // The filter function.
      std::string filter = "@kov.filter." + std::to_string(filterCount++);
      std::string ft = "define i32 " + filter + "(ptr %pointers, ptr %frame) {\nentry:\n"
                       "  %rec = load ptr, ptr %pointers\n  %code = load i32, ptr %rec\n"
                       "  %tip = getelementptr inbounds i8, ptr %rec, i64 40\n  %ti = load ptr, ptr %tip\n"
                       "  %is = icmp eq i32 %code, -536870911\n  br i1 %is, label %check0, label %no\n";
      for (size_t i = 0; i < t->handlers.size(); ++i)
      {
        std::string n = std::to_string(i), nextL = i + 1 < t->handlers.size() ? "check" + std::to_string(i + 1) : "no";
        ft += "check" + n + ":\n";
        if (!t->handlers[i].param)
        {
          ft += "  call void @kov_eh_push(ptr %rec, i32 " + n + ", i64 0)\n  ret i32 1\n";
          break;
        }
        const Ty* pt = declType(t->handlers[i].param);
        if (!pt || !pt->isKnown())
          unsupported(t->handlers[i].param, "a catch parameter of unknown type");
        ft += "  %off" + n + " = call i64 @kov_eh_offset(ptr %ti, ptr " + typeinfoFor(pt, t) + ")\n"
              "  %ok" + n + " = icmp sge i64 %off" + n + ", 0\n  br i1 %ok" + n + ", label %yes" + n + ", label %" + nextL + "\n"
              "yes" + n + ":\n  call void @kov_eh_push(ptr %rec, i32 " + n + ", i64 %off" + n + ")\n  ret i32 1\n";
      }
      ft += "no:\n  ret i32 0\n}\n\n";
      addDefinition(filter, ft);
      // The dispatch and the handlers.
      std::string outer = unwindDestination(); // objects outside the try, then the enclosing try
      startBlock(dispatchL);
      std::string cs = newTemp(), pad = newTemp();
      fn += "  " + cs + " = catchswitch within " + (frame.parentFunclet.empty() ? "none" : frame.parentFunclet) + " [label %" + handlerL +
            "] unwind " + (outer.empty() ? "to caller" : "label %" + outer) + "\n";
      startBlock(handlerL);
      fn += "  " + pad + " = catchpad within " + cs + " [ptr " + filter + "]\n";
      std::string slot = "%v" + std::to_string(temp++);
      allocas += "  " + slot + " = alloca [5 x i64]\n"; // object, typeinfo, destructor, handler, adjusted object
      ehFrames.push_back({true, "", frame.parentFunclet, pad, slot, scopes.size()});
      fn += "  call void @kov_eh_begin(ptr " + slot + ")" + bundle() + "\n";
      std::vector<std::string> handlerLabels;
      for (size_t i = 0; i < t->handlers.size(); ++i)
        handlerLabels.push_back(newLabel("catch.body"));
      if (t->handlers.size() == 1)
        emitBranch(handlerLabels[0]);
      else
      {
        std::string hp = newTemp(), h = newTemp();
        fn += "  " + hp + " = getelementptr inbounds i8, ptr " + slot + ", i64 24\n  " + h + " = load i64, ptr " + hp + "\n";
        fn += "  switch i64 " + h + ", label %" + handlerLabels[0] + " [";
        for (size_t i = 1; i < t->handlers.size(); ++i)
          fn += " i64 " + std::to_string(i) + ", label %" + handlerLabels[i];
        fn += " ]\n";
        blockTerminated = true;
      }
      for (size_t i = 0; i < t->handlers.size(); ++i)
      {
        const TryStmt::Handler& h = t->handlers[i];
        startBlock(handlerLabels[i]);
        pushScope();
        scopes.back().push_back({slot, nullptr, true});
        if (h.param)
        {
          const Ty* pt = declType(h.param);
          std::string ap = newTemp(), obj = newTemp();
          fn += "  " + ap + " = getelementptr inbounds i8, ptr " + slot + ", i64 32\n  " + obj + " = load ptr, ptr " + ap + "\n";
          if (pt->isReference())
          {
            std::string ref = "%v" + std::to_string(temp++);
            allocas += "  " + ref + " = alloca ptr\n";
            fn += "  store ptr " + obj + ", ptr " + ref + "\n";
            locals[h.param] = {ref, pt, true};
          }
          else if (isClass(pt))
          {
            std::string copy = newObjectSlot(pt, h.param);
            copyConstruct(copy, obj, pt, h.param);
            locals[h.param] = {copy, pt, false};
            if (needsDestruction(pt, h.param))
              scopes.back().push_back({copy, pt});
          }
          else
          {
            std::string v = "%v" + std::to_string(temp++), loaded = newTemp();
            allocas += "  " + v + " = alloca " + llType(pt, h.param) + "\n";
            fn += "  " + loaded + " = load " + llType(pt, h.param) + ", ptr " + obj + "\n  store " + llType(pt, h.param) + " " + loaded + ", ptr " + v + "\n";
            locals[h.param] = {v, pt, false};
          }
        }
        statement(h.body);
        popScope();
        if (!blockTerminated)
        {
          fn += "  catchret from " + pad + " to label %" + endL + "\n";
          blockTerminated = true;
        }
      }
      ehFrames.pop_back();
      startBlock(endL);
    }

    // The exception runtime (linkonce_odr, in every module) and the real @main, which calls the
    // program's main and reports an exception nobody caught.
    void finishExceptionRuntime()
    {
      std::string mainName = "@kov.main";
      auto declareOnce = [&](const char* name, const std::string& line)
      {
        if (declaredNames[name])
          return;
        declaredNames[name] = true;
        decls += line + "\n";
      };
      if (usesExceptions)
      {
        ensureMalloc();
        declareOnce("@RaiseException", "declare dllimport void @RaiseException(i32, i32, i32, ptr)");
        declareOnce("@fprintf", "declare i32 @fprintf(ptr, ptr, ...)");
        declareOnce("@__acrt_iob_func", "declare ptr @__acrt_iob_func(i32)");
        declareOnce("@abort", "declare void @abort()");
        markEH();
        emittingShared = true;
        std::string fmt1 = cStringConstant("unhandled exception of type %s"), fmt2 = cStringConstant(": %s"), fmt3 = cStringConstant("\n");
        emittingShared = false;
        shared += sharedComdat("@kov.eh.stack") + "@kov.eh.stack = linkonce_odr thread_local global [160 x i64] zeroinitializer, comdat\n" +
                  sharedComdat("@kov.eh.depth") + "@kov.eh.depth = linkonce_odr thread_local global i64 0, comdat\n";
        shared += sharedComdat("@kov_throw") +
                  "define linkonce_odr void @kov_throw(ptr %object, ptr %typeinfo, ptr %dtor) noinline comdat {\nentry:\n"
                  "  %args = alloca [3 x i64]\n  %a1 = getelementptr inbounds i64, ptr %args, i64 1\n  %a2 = getelementptr inbounds i64, ptr %args, i64 2\n"
                  "  store ptr %object, ptr %args\n  store ptr %typeinfo, ptr %a1\n  store ptr %dtor, ptr %a2\n"
                  "  call void @RaiseException(i32 -536870911, i32 1, i32 3, ptr %args)\n  ret void\n}\n";
        shared += sharedComdat("@kov_eh_offset") +
                  "define linkonce_odr i64 @kov_eh_offset(ptr %typeinfo, ptr %target) comdat {\nentry:\n  br label %loop\n"
                  "loop:\n  %ti = phi ptr [ %typeinfo, %entry ], [ %base, %next ]\n  %off = phi i64 [ 0, %entry ], [ %off1, %next ]\n"
                  "  %null = icmp eq ptr %ti, null\n  br i1 %null, label %no, label %check\n"
                  "check:\n  %same = icmp eq ptr %ti, %target\n  br i1 %same, label %yes, label %next\n"
                  "next:\n  %base = load ptr, ptr %ti\n  %op = getelementptr inbounds i8, ptr %ti, i64 16\n  %bo = load i64, ptr %op\n"
                  "  %off1 = add i64 %off, %bo\n  br label %loop\n"
                  "yes:\n  ret i64 %off\nno:\n  ret i64 -1\n}\n";
        shared += sharedComdat("@kov_eh_push") +
                  "define linkonce_odr void @kov_eh_push(ptr %record, i32 %handler, i64 %offset) comdat {\nentry:\n"
                  "  %d = load i64, ptr @kov.eh.depth\n  %i = mul i64 %d, 5\n  %slot = getelementptr inbounds i64, ptr @kov.eh.stack, i64 %i\n"
                  "  %info = getelementptr inbounds i8, ptr %record, i64 32\n  %obj = load i64, ptr %info\n"
                  "  %tip = getelementptr inbounds i8, ptr %record, i64 40\n  %ti = load i64, ptr %tip\n"
                  "  %dp = getelementptr inbounds i8, ptr %record, i64 48\n  %dtor = load i64, ptr %dp\n"
                  "  %h = zext i32 %handler to i64\n  %adj = add i64 %obj, %offset\n"
                  "  %s1 = getelementptr inbounds i64, ptr %slot, i64 1\n  %s2 = getelementptr inbounds i64, ptr %slot, i64 2\n"
                  "  %s3 = getelementptr inbounds i64, ptr %slot, i64 3\n  %s4 = getelementptr inbounds i64, ptr %slot, i64 4\n"
                  "  store i64 %obj, ptr %slot\n  store i64 %ti, ptr %s1\n  store i64 %dtor, ptr %s2\n  store i64 %h, ptr %s3\n  store i64 %adj, ptr %s4\n"
                  "  %d1 = add i64 %d, 1\n  store i64 %d1, ptr @kov.eh.depth\n  ret void\n}\n";
        shared += sharedComdat("@kov_eh_begin") +
                  "define linkonce_odr void @kov_eh_begin(ptr %out) comdat {\nentry:\n"
                  "  %d = load i64, ptr @kov.eh.depth\n  %d1 = sub i64 %d, 1\n  store i64 %d1, ptr @kov.eh.depth\n"
                  "  %i = mul i64 %d1, 5\n  %slot = getelementptr inbounds i64, ptr @kov.eh.stack, i64 %i\n"
                  "  call void @llvm.memcpy.p0.p0.i64(ptr %out, ptr %slot, i64 40, i1 false)\n  ret void\n}\n";
        shared += sharedComdat("@kov_eh_end") +
                  "define linkonce_odr void @kov_eh_end(ptr %out) comdat {\nentry:\n"
                  "  %obj = load ptr, ptr %out\n  %null = icmp eq ptr %obj, null\n  br i1 %null, label %done, label %live\n"
                  "live:\n  %dp = getelementptr inbounds i8, ptr %out, i64 16\n  %dtor = load ptr, ptr %dp\n"
                  "  %nodtor = icmp eq ptr %dtor, null\n  br i1 %nodtor, label %release, label %destroy\n"
                  "destroy:\n  call void %dtor(ptr %obj)\n  br label %release\n"
                  "release:\n  call void @free(ptr %obj)\n  store ptr null, ptr %out\n  br label %done\n"
                  "done:\n  ret void\n}\n";
        shared += sharedComdat("@kov_eh_rethrow") +
                  "define linkonce_odr void @kov_eh_rethrow(ptr %out) noinline comdat {\nentry:\n"
                  "  %obj = load ptr, ptr %out\n  %tp = getelementptr inbounds i8, ptr %out, i64 8\n  %ti = load ptr, ptr %tp\n"
                  "  %dp = getelementptr inbounds i8, ptr %out, i64 16\n  %dtor = load ptr, ptr %dp\n  store ptr null, ptr %out\n"
                  "  call void @kov_throw(ptr %obj, ptr %ti, ptr %dtor)\n  ret void\n}\n";
        shared += sharedComdat("@kov.eh.filter.any") +
                  "define linkonce_odr i32 @kov.eh.filter.any(ptr %pointers, ptr %frame) comdat {\nentry:\n"
                  "  %rec = load ptr, ptr %pointers\n  %code = load i32, ptr %rec\n  %is = icmp eq i32 %code, -536870911\n"
                  "  br i1 %is, label %yes, label %no\nyes:\n  call void @kov_eh_push(ptr %rec, i32 0, i64 0)\n  ret i32 1\nno:\n  ret i32 0\n}\n";
        std::string message;
        if (!exceptionTypeinfo.empty())
          message = "  %off = call i64 @kov_eh_offset(ptr %typeinfo, ptr " + exceptionTypeinfo + ")\n  %isExc = icmp sge i64 %off, 0\n"
                    "  br i1 %isExc, label %msg, label %end\n"
                    "msg:\n  %ep = getelementptr inbounds i8, ptr %object, i64 %off\n  %data = load ptr, ptr %ep\n"
                    "  %nodata = icmp eq ptr %data, null\n  br i1 %nodata, label %end, label %print\n"
                    "print:\n  %r2 = call i32 (ptr, ptr, ...) @fprintf(ptr %err, ptr " + fmt2 + ", ptr %data)\n  br label %end\n";
        else
          message = "  br label %end\n";
        shared += sharedComdat("@kov_eh_unhandled") +
                  "define linkonce_odr void @kov_eh_unhandled(ptr %object, ptr %typeinfo) noinline comdat {\nentry:\n"
                  "  %err = call ptr @__acrt_iob_func(i32 2)\n  %np = getelementptr inbounds i8, ptr %typeinfo, i64 8\n  %name = load ptr, ptr %np\n"
                  "  %r1 = call i32 (ptr, ptr, ...) @fprintf(ptr %err, ptr " + fmt1 + ", ptr %name)\n" + message +
                  "end:\n  %r3 = call i32 (ptr, ptr, ...) @fprintf(ptr %err, ptr " + fmt3 + ")\n  call void @abort()\n  unreachable\n}\n";
        declareOnce("@llvm.memcpy.p0.p0.i64", "declare void @llvm.memcpy.p0.p0.i64(ptr, ptr, i64, i1)");
      }
      // @main.
      std::string declare = declareLines[mainName];
      if (moduleCount <= 1)
      {
        size_t at = body.find("define i32 " + mainName + "(");
        if (at == std::string::npos)
          return;
        size_t close = body.find(")", at);
        declare = body.substr(at, close - at + 1);
      }
      if (declare.empty())
        return;
      bool args = declare.find("(i32, ptr)") != std::string::npos || declare.find("(i32 %p0, ptr %p1)") != std::string::npos;
      std::string call = "i32 " + mainName + (args ? "(i32 %argc, ptr %argv)" : "()");
      std::string text = "define i32 @main(i32 %argc, ptr %argv) " + std::string(usesExceptions ? "personality ptr @__C_specific_handler " : "") + "{\nentry:\n";
      if (!usesExceptions)
        text += "  %r = call " + call + "\n  ret i32 %r\n}\n\n";
      else
        text += "  %out = alloca [5 x i64]\n  %r = invoke " + call + " to label %done unwind label %dispatch\n"
                "done:\n  ret i32 %r\n"
                "dispatch:\n  %cs = catchswitch within none [label %handler] unwind to caller\n"
                "handler:\n  %pad = catchpad within %cs [ptr @kov.eh.filter.any]\n"
                "  call void @kov_eh_begin(ptr %out) [ \"funclet\"(token %pad) ]\n"
                "  %obj = load ptr, ptr %out\n  %tp = getelementptr inbounds i8, ptr %out, i64 8\n  %ti = load ptr, ptr %tp\n"
                "  call void @kov_eh_unhandled(ptr %obj, ptr %ti) [ \"funclet\"(token %pad) ]\n"
                "  catchret from %pad to label %failed\n"
                "failed:\n  ret i32 3\n}\n\n";
      if (moduleCount <= 1)
        body += text;
      else
        bodies[definedIn[mainName]] += text;
    }

    // `static T x = ...;` inside a function: a global of the program (in the module of the function),
    // constructed on the first pass through the declaration, never destroyed. Not thread-safe yet.
    void staticLocal(VariableDecl* v, const Ty* t)
    {
      if (t->isReference())
        unsupported(v, "a static local reference");
      std::string owner = currentFunctionName;
      if (owner.rfind("@\"", 0) == 0)
        owner = owner.substr(2, owner.size() - 3);
      else if (!owner.empty() && owner[0] == '@')
        owner = owner.substr(1);
      std::string name = "@\"" + owner + "::" + sym(v->name.last().symbol) + "." + std::to_string(staticLocals++) + "\"";
      std::string guard = name.substr(0, name.size() - 1) + ".guard\"";
      moduleDecls[currentModule] += name + " = internal global " + llType(t, v) + " zeroinitializer\n";
      moduleDecls[currentModule] += guard + " = internal global i1 false\n";
      globals[v] = name;
      locals[v] = {name, t, false};
      std::string done = newLabel("static.done"), init = newLabel("static.init");
      std::string flag = newTemp();
      fn += "  " + flag + " = load i1, ptr " + guard + "\n";
      fn += "  br i1 " + flag + ", label %" + done + ", label %" + init + "\n";
      startBlock(init);
      fn += "  store i1 true, ptr " + guard + "\n";
      if (v->init)
        initializeFrom(name, t, v->init, v);
      else if (isClass(t))
        defaultConstruct(name, t, v);
      else if (t->kind == TyKind::Array && isClass(t->inner) && hasUserConstructors(classDef(t->inner->decl, v)))
        constructArray(name, t, v);
      destroyTemps();
      emitBranch(done);
      startBlock(done);
    }

    void declareLocal(VariableDecl* v)
    {
      const Ty* t = declType(v);
      if (!t || !t->isKnown() || types.isDependent(t))
        unsupported(v, "a local of unknown type (" + (v->name.empty() ? std::string("[bindings]") : sym(v->name.last().symbol)) + ")");
      if ((v->flags & DF_Static) && !(v->flags & DF_Typedef))
      {
        staticLocal(v, t);
        return;
      }
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
        constructArray(slot, t, v); // `StringView text[2];`
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
        case NodeKind::GotoStmt:
        {
          // A jump to a label of this function (no destructors run on the way: the labels Factorio
          // uses jump forward out of loops, past nothing that needs destroying).
          uint32_t symbol = static_cast<GotoStmt*>(s)->label;
          if (!labelBlocks.count(symbol))
            labelBlocks[symbol] = newLabel("goto");
          fn += "  br label %" + labelBlocks[symbol] + "\n";
          blockTerminated = true;
          break;
        }
        case NodeKind::LabelStmt:
        {
          uint32_t symbol = static_cast<LabelStmt*>(s)->label;
          if (!labelBlocks.count(symbol))
            labelBlocks[symbol] = newLabel("goto");
          emitBranch(labelBlocks[symbol]);
          startBlock(labelBlocks[symbol]);
          break;
        }
        case NodeKind::DeclStmt:
          for (Decl* d : static_cast<DeclStmt*>(s)->decls)
          {
            if (d->kind == NodeKind::VariableDecl)
            {
              declareLocal(static_cast<VariableDecl*>(d));
              destroyTemps();
            }
            else if (d->kind == NodeKind::ClassDecl || d->kind == NodeKind::EnumDecl || d->kind == NodeKind::UsingAliasDecl ||
                     d->kind == NodeKind::UsingDirectiveDecl || d->kind == NodeKind::UsingNameDecl ||
                     d->kind == NodeKind::EmptyDecl || d->kind == NodeKind::StaticAssertDecl)
              continue; // names only (`using namespace gui;` in a body): resolved, nothing to emit
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
            if (returnedLocalObject(r->value))
              moveConstruct(sretAddr, lvalue(r->value), currentReturn, s); // `return result;`: the local is moved, as in C++
            else
              initializeFrom(sretAddr, currentReturn, r->value, s);
          }
          else if (currentReturn->isReference())
            retText = isAggregate(types.stripRef(currentReturn)) ? rvalue(r->value).text : lvalue(r->value);
          else
          {
            if (!r->value)
              unsupported(s, "return without a value in a non-void function");
            retText = convert(rvalue(r->value), currentReturn, s).text;
          }
          destroyTemps();
          std::string retLl = currentReturn->kind == TyKind::Void || isClass(currentReturn) ? std::string()
                              : currentReturn->isReference()                                ? std::string("ptr")
                                                                                             : llType(currentReturn, s);
          if (!retLl.empty() && !currentFunclet().empty())
          {
            // Leaving a catch handler: the value crosses a funclet boundary through memory.
            std::string slot = "%v" + std::to_string(temp++);
            allocas += "  " + slot + " = alloca " + retLl + "\n";
            fn += "  store " + retLl + " " + retText + ", ptr " + slot + "\n";
            leaveScopes(0);
            retText = newTemp();
            fn += "  " + retText + " = load " + retLl + ", ptr " + slot + "\n";
          }
          else
            leaveScopes(0);
          if (retLl.empty())
            fn += "  ret void\n";
          else
            fn += "  ret " + retLl + " " + retText + "\n";
          blockTerminated = true;
          break;
        }
        case NodeKind::IfStmt:
        {
          auto* i = static_cast<IfStmt*>(s);
          if (i->isConstexpr && i->constexprKnown >= 0)
          {
            // The resolver already folded this if constexpr during instantiation.
            statement(i->constexprKnown ? i->then : i->else_);
            break;
          }
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
          emitCall("", "void @kov_assert_failed(ptr " + cStringConstant(currentFile->path) + ", i32 " + std::to_string(line) + ", i32 " +
                           std::to_string(column) + ", ptr " + cStringConstant(sourceText(a->startToken, a->endToken)) + ")", false);
          fn += "  unreachable\n";
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
          leaveScopes(loops.back().scopeDepth);
          emitBranch(loops.back().breakLabel);
          break;
        case NodeKind::ContinueStmt:
        {
          for (size_t i = loops.size(); i-- > 0;)
            if (!loops[i].continueLabel.empty())
            {
              leaveScopes(loops[i].scopeDepth);
              emitBranch(loops[i].continueLabel);
              return;
            }
          unsupported(s, "continue outside a loop");
        }
        case NodeKind::SwitchStmt:
          switchStatement(static_cast<SwitchStmt*>(s));
          break;
        case NodeKind::TryStmt:
          tryStatement(static_cast<TryStmt*>(s));
          break;
        case NodeKind::RangeForStmt:
          rangeFor(static_cast<RangeForStmt*>(s));
          break;
        case NodeKind::ForConstexprStmt:
          forConstexpr(static_cast<ForConstexprStmt*>(s));
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

    // A range whose begin()/end() return iterator objects: `it != end`, `*it` and `++it` go through
    // the iterator class's operators. Both iterators live in the loop's scope (already open).
    void iteratorLoop(RangeForStmt* f, Value begin, Value end)
    {
      auto own = [&](const Value& v)
      {
        for (size_t i = statementTemps.size(); i-- > 0;)
          if (statementTemps[i].addr == v.text)
            statementTemps.erase(statementTemps.begin() + i);
        if (needsDestruction(v.ty, f))
          scopes.back().push_back({v.text, v.ty});
      };
      own(begin);
      own(end);
      destroyTemps();
      // `it != end`, or `!(it == end)`.
      const Ty* in = nullptr;
      std::vector<const Ty*> argTypes{types.lref(types.qualified(begin.ty, true, false))};
      bool negate = false;
      FunctionDecl* compare = findMemberFunction(begin.ty, "operator!=", argTypes, begin.ty, f, &in);
      if (!compare)
      {
        compare = findMemberFunction(begin.ty, "operator==", argTypes, begin.ty, f, &in);
        negate = true;
      }
      if (!compare)
        unsupported(f, "an iterator without operator!= or operator==");
      TemplateArgs compareArgs = global.classArgsOf(in);
      std::string compareName = useFunction(compare, compareArgs, in);
      Signature compareSig;
      {
        ArgsScope scope(*this, &compareArgs);
        compareSig = signatureOf(compare);
      }
      std::string cL = newLabel("iter.cond"), bL = newLabel("iter.body"), sL = newLabel("iter.step"), eL = newLabel("iter.end");
      emitBranch(cL);
      startBlock(cL);
      std::string thisArg = begin.text;
      if (canonicalClass(begin.ty, f) != in)
        baseAddress(begin.ty, in, begin.text, thisArg, f);
      std::string cond = newTemp();
      emitCall(cond + " = ", compareSig.returnType + " " + compareName + "(ptr " + thisArg + ", ptr " + end.text + ")");
      if (compareSig.returnType != "i1")
        cond = convert(Value{cond, types.intType(32)}, types.boolType(), f).text;
      if (negate)
      {
        std::string flipped = newTemp();
        fn += "  " + flipped + " = xor i1 " + cond + ", true\n";
        cond = flipped;
      }
      fn += "  br i1 " + cond + ", label %" + bL + ", label %" + eL + "\n";
      startBlock(bL);
      pushScope();
      // The loop variable from `*it`: a reference binds to the element, a value copies it.
      auto* v = static_cast<VariableDecl*>(f->var);
      const Ty* vt = declType(v);
      if (!vt || !vt->isKnown() || types.isDependent(vt))
        unsupported(v, "a loop variable of unknown type");
      Value element = operatorCall(begin, "operator*", {}, f, true);
      std::string slot = "%v" + std::to_string(temp++);
      if (vt->isReference())
      {
        std::string addr = element.text;
        if (!element.ty->isReference() && !isClass(element.ty))
        {
          addr = newObjectSlot(element.ty, v);
          fn += "  store " + llType(element.ty, v) + " " + element.text + ", ptr " + addr + "\n";
        }
        allocas += "  " + slot + " = alloca ptr\n";
        fn += "  store ptr " + addr + ", ptr " + slot + "\n";
        locals[v] = {slot, vt, true};
      }
      else
      {
        allocas += "  " + slot + " = alloca " + llType(vt, v) + "\n";
        if (isClass(vt))
        {
          if (element.ty->isReference() || !element.isTemporary)
            copyConstruct(slot, element.text, vt, v);
          else
            moveTemporary(slot, element, vt, v);
        }
        else
          store(callRvalue(element, v), slot, vt, v);
        locals[v] = {slot, vt, false};
        if (needsDestruction(vt, v))
          scopes.back().push_back({slot, vt});
      }
      destroyTemps();
      loops.push_back({eL, sL, scopes.size()});
      statement(f->body);
      loops.pop_back();
      popScope();
      emitBranch(sL);
      startBlock(sL);
      operatorCall(begin, "operator++", {}, f, true);
      destroyTemps();
      emitBranch(cL);
      startBlock(eL);
    }

    // --- typed unions (design.md 8.5): a tag and the storage of one alternative ---

    ClassDecl* typedUnionOf(const Ty* t, Node* at)
    {
      t = t ? types.unqualified(types.stripRef(t)) : nullptr;
      if (!isClass(t))
        return nullptr;
      ClassDecl* c = classDef(t->decl, at);
      return c && c->isTypedUnion ? c : nullptr;
    }
    const Ty* tagType(ClassDecl* c) { return types.enumType(c->tagEnum); }
    std::string tagAddress(ClassDecl* c, const Ty* t, const std::string& obj, Node* at)
    {
      std::string addr;
      if (!fieldAddress(types.unqualified(types.stripRef(t)), c->tagField, obj, addr, at))
        unsupported(at, "a typed union without its tag");
      return addr;
    }
    std::string loadTag(ClassDecl* c, const Ty* t, const std::string& obj, Node* at)
    {
      return load(tagAddress(c, t, obj, at), tagType(c), at).text;
    }
    const Ty* alternativeType(ClassDecl* c, const Ty* t, size_t index, Node* at)
    {
      const Ty* cls = canonicalClass(types.unqualified(types.stripRef(t)), at);
      const Ty* alt = global.typeOfDeclIn(c->alternatives[index], global.classArgsOf(cls));
      if (!alt || !alt->isKnown())
        unsupported(at, "an alternative of a typed union whose type is unknown");
      return alt;
    }
    std::string alternativeAddress(ClassDecl* c, const Ty* t, const std::string& obj, size_t index, Node* at)
    {
      std::string addr;
      if (!fieldAddress(types.unqualified(types.stripRef(t)), c->alternatives[index], obj, addr, at))
        unsupported(at, "a typed union without its storage");
      return addr;
    }
    // The alternative an index names: `p[Property::Integer]`, a constant enumerator of the tag.
    size_t alternativeIndex(ClassDecl* c, IndexExpr* i, Node* at)
    {
      if (i->args.size() != 1)
        unsupported(at, "a typed union indexed with more than one alternative");
      std::optional<int64_t> v = evalConst(i->args[0]);
      if (!v || *v < 0 || size_t(*v) >= c->alternatives.size())
        unsupported(at, "a typed union indexed with something other than one of its alternatives");
      return size_t(*v);
    }
    // The address of the alternative p[E] names; with check, a release_assert that p holds it.
    std::string alternativeLvalue(ClassDecl* c, Value base, IndexExpr* i, Node* at, bool check)
    {
      size_t index = alternativeIndex(c, i, at);
      if (check)
      {
        std::string tag = loadTag(c, base.ty, base.text, at);
        std::string same = newTemp();
        fn += "  " + same + " = icmp eq " + llType(tagType(c), at) + " " + tag + ", " + std::to_string(index) + "\n";
        std::string okL = newLabel("alt.ok"), failL = newLabel("alt.fail");
        fn += "  br i1 " + same + ", label %" + okL + ", label %" + failL + "\n";
        startBlock(failL);
        auto [line, column] = lineColumnOf(at->token);
        emitCall("", "void @kov_assert_failed(ptr " + cStringConstant(currentFile->path) + ", i32 " + std::to_string(line) + ", i32 " +
                         std::to_string(column) + ", ptr " + cStringConstant("the typed union holds another alternative") + ")", false);
        fn += "  unreachable\n";
        usesAssert = true;
        startBlock(okL);
      }
      return alternativeAddress(c, base.ty, base.text, index, at);
    }
    void setTag(ClassDecl* c, const Ty* t, const std::string& obj, size_t index, Node* at)
    {
      fn += "  store " + llType(tagType(c), at) + " " + std::to_string(index) + ", ptr " + tagAddress(c, t, obj, at) + "\n";
    }
    // Sets the tag and builds the alternative from init (default-constructed without one).
    void constructAlternative(ClassDecl* c, const Ty* t, const std::string& obj, size_t index, Expr* init, Node* at)
    {
      setTag(c, t, obj, index, at);
      const Ty* altTy = alternativeType(c, t, index, at);
      std::string addr = alternativeAddress(c, t, obj, index, at);
      if (init)
        initializeFrom(addr, altTy, init, at);
      else if (isClass(altTy))
        defaultConstruct(addr, altTy, at);
      else if (altTy->kind == TyKind::Array && needsDestruction(altTy, at))
        constructArray(addr, altTy, at);
      else
        fn += "  store " + llType(altTy, at) + " zeroinitializer, ptr " + addr + "\n";
    }
    // Runs body(index) for the alternative the object holds, among those `wanted` selects: a
    // switch on the tag.
    template<typename Wanted, typename Body>
    void forHeldAlternative(ClassDecl* c, const Ty* t, const std::string& obj, Node* at, Wanted&& wanted, Body&& body)
    {
      std::vector<size_t> indices;
      for (size_t i = 0; i < c->alternatives.size(); ++i)
        if (wanted(i))
          indices.push_back(i);
      if (indices.empty())
        return;
      std::string tag = loadTag(c, t, obj, at);
      std::string k = llType(tagType(c), at);
      std::string endL = newLabel("alt.end");
      std::vector<std::string> labels;
      std::string cases;
      for (size_t i : indices)
      {
        labels.push_back(newLabel("alt"));
        cases += "    " + k + " " + std::to_string(i) + ", label %" + labels.back() + "\n";
      }
      fn += "  switch " + k + " " + tag + ", label %" + endL + " [\n" + cases + "  ]\n";
      blockTerminated = true;
      for (size_t n = 0; n < indices.size(); ++n)
      {
        startBlock(labels[n]);
        body(indices[n]);
        emitBranch(endL);
      }
      startBlock(endL);
    }
    void destroyAlternative(ClassDecl* c, const Ty* t, const std::string& obj, Node* at)
    {
      forHeldAlternative(c, t, obj, at,
                         [&](size_t i) { return needsDestruction(alternativeType(c, t, i, at), at); },
                         [&](size_t i) { destroy(alternativeAddress(c, t, obj, i, at), alternativeType(c, t, i, at), at); });
    }
    // Copies (or moves) the alternative src holds into dst, tag included.
    void copyAlternative(ClassDecl* c, const Ty* t, const std::string& dst, const std::string& src, bool move, Node* at)
    {
      std::string tag = loadTag(c, t, src, at);
      fn += "  store " + llType(tagType(c), at) + " " + tag + ", ptr " + tagAddress(c, t, dst, at) + "\n";
      forHeldAlternative(c, t, src, at, [&](size_t) { return true; }, [&](size_t i)
      {
        const Ty* altTy = alternativeType(c, t, i, at);
        std::string from = alternativeAddress(c, t, src, i, at), to = alternativeAddress(c, t, dst, i, at);
        if (isClass(altTy))
        {
          if (move)
            moveConstruct(to, from, altTy, at);
          else
            copyConstruct(to, from, altTy, at);
        }
        else
        {
          std::string v = newTemp();
          fn += "  " + v + " = load " + llType(altTy, at) + ", ptr " + from + "\n";
          fn += "  store " + llType(altTy, at) + " " + v + ", ptr " + to + "\n";
        }
      });
    }
    // `p[Integer] = 5`: destroys the alternative held, sets the tag, constructs the new value.
    Value assignAlternative(IndexExpr* i, Expr* rhs, Node* at)
    {
      Value base = rvalue(i->base);
      ClassDecl* c = typedUnionOf(base.ty, at);
      size_t index = alternativeIndex(c, i, at);
      destroyAlternative(c, base.ty, base.text, at);
      constructAlternative(c, base.ty, base.text, index, rhs, at);
      const Ty* altTy = alternativeType(c, base.ty, index, at);
      std::string addr = alternativeAddress(c, base.ty, base.text, index, at);
      return isClass(altTy) ? Value{addr, altTy, false} : load(addr, altTy, at);
    }
    // `p == Property::Integer`: the tag against the enumerator.
    Value compareTag(BinaryExpr* b, bool negate)
    {
      bool unionLeft = typedUnionOf(types.stripRef(b->lhs->ty), b) != nullptr;
      Expr* unionExpr = unionLeft ? b->lhs : b->rhs;
      Expr* tagExpr = unionLeft ? b->rhs : b->lhs;
      Value obj = rvalue(unionExpr);
      ClassDecl* c = typedUnionOf(obj.ty, b);
      const Ty* underlying = enumUnderlying(c->tagEnum);
      Value tag = convert(Value{loadTag(c, obj.ty, obj.text, b), tagType(c), false}, underlying, b);
      Value other = convert(rvalue(tagExpr), underlying, b);
      std::string r = newTemp();
      fn += "  " + r + " = icmp " + (negate ? "ne " : "eq ") + llType(underlying, b) + " " + tag.text + ", " + other.text + "\n";
      return {r, types.boolType(), false};
    }

    // `types<A, B, C>`: the compile-time list of types (design.md 8.1).
    bool isTypesList(const NamedType* n)
    {
      return n->name.parts.size() == 1 && n->name.parts[0].hasTemplateArgs && sym(n->name.parts[0].symbol) == "types";
    }

    // `members(T)` (design.md 8.3): the type reflected on, or null when the node is no such list.
    Expr* reflectionSubject(Node* list)
    {
      if (!list || list->kind != NodeKind::CallExpr)
        return nullptr;
      auto* c = static_cast<CallExpr*>(list);
      if (!c->callee || c->callee->kind != NodeKind::NameExpr || c->args.size() != 1)
        return nullptr;
      const QualifiedName& name = static_cast<NameExpr*>(c->callee)->name;
      if (name.parts.size() != 1 || name.parts[0].hasTemplateArgs || sym(name.parts[0].symbol) != "members")
        return nullptr;
      return c->args[0];
    }

    // The descriptors of a class's non-static data members, bases first, in declaration order.
    void collectMembers(const Ty* cls, std::vector<const Ty*>& out, Node* at, int depth)
    {
      if (depth > 16)
        unsupported(at, "a class nested too deep for members(T)");
      ClassDecl* def = classDef(cls->decl, at);
      if (!def)
        unsupported(at, "members(T) of a class without a definition");
      TemplateArgs classArgs = global.classArgsOf(cls);
      for (const ClassDecl::Base& b : def->bases)
      {
        const Ty* bt = global.typeOfAstIn(b.type, def, classArgs);
        bt = bt ? types.unqualified(types.stripRef(bt)) : nullptr;
        if (!isClass(bt))
          unsupported(at, "members(T) with a base class of unknown type");
        collectMembers(canonicalClass(bt, at), out, at, depth + 1);
      }
      for (Decl* d : def->members)
      {
        if (d->kind != NodeKind::VariableDecl || (d->flags & (DF_Static | DF_Typedef)))
          continue;
        auto* v = static_cast<VariableDecl*>(d);
        if (v->name.empty())
          continue;
        const Ty* t = global.typeOfDeclIn(v, classArgs);
        if (!t || !t->isKnown())
          unsupported(at, "members(T) with a member of unknown type (" + sym(v->name.last().symbol) + ")");
        out.push_back(types.member(t, v, cls));
      }
    }

    // `for constexpr (typename T : types<A, B>) body` / `for constexpr (int i : {1, 2}) body`: the
    // body once per element, re-typed and emitted with the loop variable bound to the element (a
    // type, or a constant), the way a pack expansion binds each element in turn.
    void forConstexpr(ForConstexprStmt* f)
    {
      forEachConstexprElement(f, [&](size_t)
      {
        pushScope();
        statement(f->body);
        popScope();
      });
    }

    // The statements of a for constexpr body: those of its block, or the statement itself.
    static std::vector<Stmt*> bodyStatements(Stmt* body)
    {
      if (!body)
        return {};
      if (body->kind == NodeKind::BlockStmt)
        return std::vector<Stmt*>(static_cast<BlockStmt*>(body)->stmts.begin(), static_cast<BlockStmt*>(body)->stmts.end());
      return {body};
    }

    // Runs body(index) once per element of a for constexpr list, with the function re-typed for
    // the element and currentArgs binding the loop variable to it; the original typing is
    // restored afterwards.
    void forEachConstexprElement(ForConstexprStmt* f, const std::function<void(size_t)>& body)
    {
      auto* var = static_cast<TemplateParamDecl*>(f->var);
      if (!currentFunction)
        unsupported(f, "for constexpr outside a function");
      TemplateArgs none;
      TemplateArgs original = currentArgs ? *currentArgs : none;
      std::vector<const Ty*> elements;
      Node* list = f->list;
      if (list && list->kind == NodeKind::NamedType && isTypesList(static_cast<NamedType*>(list)))
      {
        if (!var->isType)
          unsupported(f, "a list of types for a for constexpr value variable");
        for (Node* arg : static_cast<NamedType*>(list)->name.parts[0].templateArgs)
        {
          if (!arg || uint8_t(arg->kind) > uint8_t(NodeKind::PackType))
            unsupported(f, "an element of types<...> that is not a type");
          const Ty* t = global.typeOfAstIn(static_cast<Type*>(arg), currentFunction, original);
          if (!t || !t->isKnown())
            unsupported(f, "an element of types<...> whose type is unknown");
          elements.push_back(t);
        }
      }
      else if (list && list->kind == NodeKind::BraceInitExpr)
      {
        if (var->isType)
          unsupported(f, "a list of values for a for constexpr type variable");
        const Ty* elementTy = var->valueType ? global.typeOfAstIn(var->valueType, currentFunction, original) : nullptr;
        if (!elementTy || !elementTy->isKnown() || elementTy->kind == TyKind::Auto)
          unsupported(f, "a for constexpr value variable without a written type");
        for (Expr* e : static_cast<BraceInitExpr*>(list)->args)
        {
          std::optional<int64_t> v = evalConst(e);
          if (!v)
            unsupported(e, "a for constexpr element that is not a constant");
          elements.push_back(types.valueType(elementTy, *v));
        }
      }
      else if (Expr* subject = reflectionSubject(list))
      {
        // `members(T)`: T's non-static data members, bases first, in declaration order.
        if (var->isType)
          unsupported(f, "members(T) for a for constexpr type variable");
        const Ty* cls = nullptr;
        if (subject->kind == NodeKind::NameExpr)
        {
          NamedType tmp{};
          tmp.kind = NodeKind::NamedType;
          tmp.name = static_cast<NameExpr*>(subject)->name;
          cls = global.typeOfAstIn(&tmp, currentFunction, original);
        }
        cls = cls ? types.unqualified(types.stripRef(cls)) : nullptr;
        if (!isClass(cls))
          unsupported(f, "members(T) where T is not a class");
        collectMembers(canonicalClass(cls, f), elements, f, 0);
      }
      else
        unsupported(f, "a for constexpr list that is neither types<...>, members(T) nor a brace list of constants");
      const TemplateArgs* savedArgs = currentArgs;
      for (size_t index = 0; index < elements.size(); ++index)
      {
        TemplateArgs elementArgs = original;
        bool bound = false;
        for (auto& [d, t] : elementArgs.pairs)
          if (d == var)
            t = elements[index], bound = true;
        if (!bound)
          elementArgs.pairs.push_back({var, elements[index]});
        global.retypeFor(currentFunction, elementArgs, currentClassTy);
        currentArgs = &elementArgs;
        body(index);
        currentArgs = savedArgs;
      }
      if (!elements.empty())
        global.retypeFor(currentFunction, original, currentClassTy);
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
        std::string base;
        if (f->range->kind == NodeKind::BraceInitExpr)
        {
          // `for (X* kids : {&a, &b})`: the elements in a temporary array.
          auto* b = static_cast<BraceInitExpr*>(f->range);
          base = newObjectSlot(rt, f);
          for (size_t i = 0; i < b->args.size(); ++i)
          {
            std::string slot = newTemp();
            fn += "  " + slot + " = getelementptr inbounds " + llType(rt, f) + ", ptr " + base + ", i64 0, i64 " + std::to_string(i) + "\n";
            initializeFrom(slot, rt->inner, b->args[i], f);
          }
        }
        else
          base = lvalue(f->range);
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
        if (b.ty && isClass(b.ty))
        {
          iteratorLoop(f, b, e);
          popScope();
          return;
        }
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
      const Ty* switchedTy = types.stripRef(static_cast<Expr*>(sw->cond)->ty); // the written type: a Kov enum's switch names every value
      if (ClassDecl* c = typedUnionOf(cond.ty, sw))
      {
        // `switch (p)` on a typed union: its tag.
        cond = Value{loadTag(c, cond.ty, cond.text, sw), tagType(c), false};
        switchedTy = cond.ty;
      }
      else if (isClass(cond.ty))
      {
        // `switch (direction)` on an object: its conversion to an enum or integer (Direction's `operator Enum()`).
        FunctionDecl* op = integralConversionOperator(cond.ty, sw);
        if (!op)
          unsupported(sw, "a switch on a class without a conversion to an enum or an integer");
        TemplateArgs args = global.classArgsOf(canonicalClass(cond.ty, sw));
        cond = callRvalue(callFunction(op, args, canonicalClass(cond.ty, sw), cond.text, "", {}, sw), sw);
        switchedTy = cond.ty;
      }
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
      std::map<std::pair<const Stmt*, size_t>, std::string> caseLabels; // (label statement, for constexpr element) -> block
      std::set<int64_t> covered;
      std::string cases;
      constexpr size_t topLevel = SIZE_MAX;
      auto addLabel = [&](Stmt* st, size_t element)
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
          caseLabels[{st, element}] = l;
          covered.insert(*v);
          cases += "    " + llType(ct, sw) + " " + std::to_string(*v) + ", label %" + l + "\n";
        }
        else if (st->kind == NodeKind::DefaultStmt)
        {
          defaultL = newLabel("default");
          caseLabels[{st, element}] = defaultL;
        }
      };
      for (Stmt* st : body->stmts)
      {
        if (st->kind == NodeKind::ForConstexprStmt)
        {
          // `for constexpr (typename T : kinds) case T::kind: ...`: one case per element.
          auto* fc = static_cast<ForConstexprStmt*>(st);
          forEachConstexprElement(fc, [&](size_t element)
          {
            for (Stmt* inner : bodyStatements(fc->body))
              addLabel(inner, element);
          });
          continue;
        }
        addLabel(st, topLevel);
      }
      std::string invalidL;
      const Ty* condTy = switchedTy; // before the conversion to the underlying type
      if (condTy && condTy->kind == TyKind::Enum && defaultL.empty())
      {
        Decl* def = global.definitionOf(condTy->decl);
        auto* e = def && def->kind == NodeKind::EnumDecl ? static_cast<EnumDecl*>(def) : nullptr;
        if (e && e->isKov)
        {
          // A Kov enum: every value needs a case, and a value outside them aborts.
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
        emitCall("", "void @kov_assert_failed(ptr " + cStringConstant(currentFile->path) + ", i32 " + std::to_string(line) + ", i32 " +
                         std::to_string(column) + ", ptr " + cStringConstant("invalid value in switch on " + global.qualifiedName(condTy->decl)) + ")", false);
        fn += "  unreachable\n";
        usesAssert = true;
        blockTerminated = true;
      }
      loops.push_back({endL, "", scopes.size()});
      pushScope();
      auto emitLabelled = [&](Stmt* st, size_t element)
      {
        if (st->kind == NodeKind::CaseStmt || st->kind == NodeKind::DefaultStmt)
        {
          const std::string& l = caseLabels[{st, element}];
          emitBranch(l);
          startBlock(l);
          return;
        }
        if (st->kind == NodeKind::FallthroughStmt)
          return;
        statement(st);
      };
      for (Stmt* st : body->stmts)
      {
        if (st->kind == NodeKind::ForConstexprStmt)
        {
          auto* fc = static_cast<ForConstexprStmt*>(st);
          forEachConstexprElement(fc, [&](size_t element)
          {
            pushScope();
            for (Stmt* inner : bodyStatements(fc->body))
              emitLabelled(inner, element);
            popScope();
          });
          continue;
        }
        emitLabelled(st, topLevel);
      }
      popScope();
      loops.pop_back();
      emitBranch(endL);
      startBlock(endL);
      popScope();
    }

    // --- function definitions ---

    void emitFunction(FunctionDecl* f, const TemplateArgs& args, const Ty* classTy, const std::string& nameOverride = "")
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
      resetFunctionState();
      currentFunction = f;
      currentReturn = ft->inner;
      sretAddr.clear();
      currentFunctionName = nameOverride.empty() ? functionName(f, args, classTy) : nameOverride;
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
        std::string name = currentFunctionName;
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
        if ((f->flags & DF_Default) && !f->body)
        {
          // `= default`: the memberwise body; the source object, if any, is the parameter after this.
          size_t sourceIndex = s.hasSret ? 2 : 1;
          std::string source = paramNames.size() > sourceIndex ? paramNames[sourceIndex] : std::string();
          Value r = defaultedOperation(f, types.unqualified(types.stripRef(currentClassTy)), thisPointer(f), source, f);
          destroyTemps();
          popScope();
          if (isClass(currentReturn))
          {
            // A class result (a defaulted operator<=>'s strong_ordering): into the result slot.
            if (!sretAddr.empty() && r.ty && isClass(r.ty))
              fn += "  store i8 " + orderOf(r, f).text + ", ptr " + gep(structFor(currentReturn, f).name, sretAddr, 0) + "\n";
            fn += "  ret void\n";
          }
          else if (currentReturn->kind == TyKind::Void)
            fn += "  ret void\n";
          else if (currentReturn->isReference())
            fn += "  ret ptr " + r.text + "\n";
          else
            fn += "  ret " + llType(currentReturn, f) + " " + r.text + "\n";
          addDefinition(name, functionText(header));
          ++result.functions;
          currentArgs = nullptr;
          currentClassTy = nullptr;
          return;
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
          else if (name == "@kov.main")
            fn += "  ret i32 0\n";
          else // falling off the end (after a throw, usually): a zero rather than `unreachable`, see emitThrow
            fn += "  ret " + (currentReturn->isReference() ? std::string("ptr null") : llType(currentReturn, f) + " zeroinitializer") + "\n";
        }
        else if (f->flags & DF_Destructor)
        {
          error(f, "codegen does not support return statements in destructors yet");
          currentArgs = nullptr;
          return;
        }
        addDefinition(name, functionText(header));
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
  e.moduleCtors.resize(e.moduleCount);
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
    if (f->name.parts.size() > 1)
      global.declarationOf(f); // `void Text::split(...)`: static or virtual as its declaration says, whoever asks first
    e.emitFunction(f, {}, nullptr);
  }
  // Instantiations requested while emitting, including ones requested by other instantiations.
  while (!e.worklist.empty())
  {
    Emitter::Instantiation inst = e.worklist.front();
    e.worklist.pop_front();
    e.currentModule = inst.module;
    e.currentRequestedBy = inst.requestedBy;
    e.emitFunction(inst.f, inst.args, inst.classTy, inst.name);
    e.currentRequestedBy.clear();
  }
  e.finishAssertRuntime();
  e.finishExceptionRuntime();
  std::string prefix = "; Kov v0: generated by kovc\n"
                       "target datalayout = \"e-m:w-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128\"\n"
                       "target triple = \"x86_64-pc-windows-msvc\"\n\n";
  if (e.moduleCount <= 1)
  {
    result.ir = prefix + e.declsFor(0) + e.moduleDecls[0] + e.ctorsText(0) + "\n" + e.shared + e.body;
    return result;
  }
  for (unsigned k = 0; k < e.moduleCount; ++k)
    result.modules.push_back(e.moduleText(k, prefix));
  return result;
}
