#pragma once
// Order-independent name resolution and type checking: phase 1 indexes every file's declarations
// into one shared scope tree (in parallel), phase 2 resolves type names and identifiers in all
// declarations and function bodies against that tree and computes the type of every expression
// (in parallel; the tree is read-only, canonical types are interned in a shared table).
#include "ast.hpp"
#include "types.hpp"
#include <cstdint>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <vector>

struct Scope;

// Template parameter -> argument type, for queries about one instantiation.
struct TemplateArgs
{
  std::vector<std::pair<Decl*, const Ty*>> pairs;
  bool empty() const { return pairs.empty(); }
};

// Whether an expression names an object that outlives the expression (an lvalue): a variable or
// parameter (a named rvalue reference included), a field, an element of an array or pointer, a
// dereference. Calls and operators that yield references carry the reference in their type.
inline bool isLvalueExpression(const Expr* e)
{
  if (!e)
    return false;
  switch (e->kind)
  {
    case NodeKind::NameExpr:
    {
      Decl* d = static_cast<const NameExpr*>(e)->resolved;
      return d && (d->kind == NodeKind::VariableDecl || d->kind == NodeKind::ParamDecl);
    }
    case NodeKind::MemberExpr:
    {
      Decl* d = static_cast<const MemberExpr*>(e)->resolved;
      return d && d->kind == NodeKind::VariableDecl;
    }
    case NodeKind::IndexExpr:
    {
      const Ty* base = static_cast<const IndexExpr*>(e)->base->ty;
      if (base && base->isReference())
        base = base->inner;
      return base && (base->kind == TyKind::Pointer || base->kind == TyKind::Array);
    }
    case NodeKind::UnaryExpr:
    {
      auto* u = static_cast<const UnaryExpr*>(e);
      const Ty* t = u->operand ? u->operand->ty : nullptr;
      if (t && t->isReference())
        t = t->inner;
      return !u->isKeyword && !u->postfix && P(u->op) == P::Star && t && t->kind == TyKind::Pointer;
    }
    case NodeKind::ParenExpr:
      return isLvalueExpression(static_cast<const ParenExpr*>(e)->inner);
    default:
      return false;
  }
}

// The type an argument expression presents to overload resolution: an lvalue of a non-reference
// type is an lvalue reference to it, so that `T&&` parameters take only temporaries and `move(x)`
// (C++ rules); `t` is the expression's type, or one element of a pack it expands.
inline const Ty* argumentType(const Expr* e, const Ty* t, TypeTable& types)
{
  if (!t || !t->isKnown() || t->kind == TyKind::Pack || !isLvalueExpression(e))
    return t;
  if (t->kind == TyKind::RRef)
    return types.lref(t->inner); // a named rvalue reference is an lvalue
  return t->isReference() ? t : types.lref(t);
}

// The type a brace list `{a, b, c}` presents to overload resolution: InitializerList<E> of the
// first element's type (null when the elements are untyped or there is no such class). Only for
// ranking and deduction: the expression itself stays untyped, since a brace list also initialises
// aggregates and calls constructors.
inline const Ty* braceListType(const Expr* e, Decl* initializerList, TypeTable& types)
{
  if (!e || e->kind != NodeKind::BraceInitExpr || !initializerList)
    return nullptr;
  auto* b = static_cast<const BraceInitExpr*>(e);
  if (b->args.empty() || !b->args[0] || !b->args[0]->ty || !b->args[0]->ty->isKnown())
    return nullptr;
  const Ty* element = types.decay(b->args[0]->ty);
  return types.classType(initializerList, Span<const Ty*>{&element, 1});
}

struct ResolveStats
{
  // Type names (in declarations, casts, template arguments...).
  uint64_t typesResolved = 0, typesDependent = 0, typesUnresolved = 0, typesBuiltinByName = 0;
  // Identifiers in expressions.
  uint64_t namesLocal = 0, namesMember = 0, namesGlobal = 0, namesDependent = 0, namesUnresolved = 0;
  uint64_t qualifiedUnresolvedStd = 0; // std:: and other names that are not part of the corpus.
  uint64_t unresolvedMacroLike = 0;    // ALL_CAPS identifiers: almost always macros.
  std::unordered_map<uint32_t, uint32_t> unresolvedSymbols; // symbol -> count
  struct Where { uint32_t file; uint32_t token; };
  std::unordered_map<uint32_t, Where> unresolvedFirst; // symbol -> first occurrence
  uint32_t fileIndex = 0; // Set by the driver before resolving, so first occurrences can be reported.

  // Member accesses `a.b` / `a->b` / `this->b`, bound through the type of the object.
  uint64_t membersBound = 0, membersDependent = 0, membersUnknownBase = 0, membersNotFound = 0;
  std::unordered_map<uint32_t, uint32_t> missingMembers; // symbol -> count (known class, no such member)
  std::unordered_map<uint32_t, Where> missingMembersFirst;
  std::unordered_map<uint32_t, uint32_t> unknownBaseMembers; // member name -> count, when the object type is unknown

  // Expressions.
  uint64_t exprsTyped = 0, exprsDependent = 0, exprsUnknown = 0;
  // Why an expression's type is unknown (first cause wins).
  uint64_t unknownName = 0, unknownBase = 0, unknownMember = 0, unknownOverload = 0, unknownDeduction = 0,
           unknownAutoReturn = 0, unknownUnsupported = 0, unknownOperator = 0, unknownAuto = 0;
  uint64_t callsResolved = 0, callsAmbiguousArity = 0;
  // Expressions typed while checking template bodies for an instantiation (also included above).
  uint64_t instExprsTyped = 0, instExprsDependent = 0, instExprsUnknown = 0;

  void add(const ResolveStats& o);
};

class GlobalScope
{
public:
  explicit GlobalScope(SymbolTable& symbols);
  ~GlobalScope();

  // Phase 1, thread-safe: index all top-level declarations of one file.
  void indexFile(ParsedFile& file);
  // After all files are indexed: attach out-of-line class definitions (`struct A::B {}`).
  void finishIndex();
  // Phase 2, thread-safe and read-only on the scope tree: resolve and type one file.
  void resolveFile(ParsedFile& file, const std::vector<Token>& tokens, std::string_view text, ResolveStats& stats);

  size_t scopeCount() const;
  size_t entryCount() const;
  size_t typeCount() const;
  struct InstantiationStats
  {
    size_t classes, functions, bodies, requests, waits;
    double seconds;
  };
  InstantiationStats instantiationStats() const;

  // Queries for code generation (after resolveFile ran on every file).
  TypeTable& types();
  const Ty* typeOfDecl(Decl* d);                 // Declared (or deduced) canonical type.
  const Ty* typeOfAst(Type* t, Decl* context);   // A written type, resolved in the context of a declaration.
  Decl* definitionOf(Decl* d);                   // Class/enum definition, or the out-of-line definition of a member function or a static data member.
  Decl* initializerListDecl();                   // The library's InitializerList class, or null.
  Decl* typeInfoDecl();                          // The library's TypeInfo class (what typeid yields), or null.
  Decl* owningClass(Decl* d);                    // The class a member (possibly defined out of line) belongs to.
  // The in-class declaration of an out-of-line member definition (`static` and `virtual` are
  // written there and copied onto the definition); null when there is none.
  FunctionDecl* declarationOf(FunctionDecl* def);
  std::string qualifiedName(Decl* d);            // `ns::Class::name`
  bool isExternC(FunctionDecl* f);
  bool isTemplateContext(Decl* d);               // Inside a template (any enclosing declaration has parameters).
  // Instantiation queries, single-threaded (code generation):
  const Ty* typeOfDeclIn(Decl* d, const TemplateArgs& args);
  const Ty* typeOfAstIn(Type* t, Decl* context, const TemplateArgs& args);
  // Re-types the body of f for one instantiation (expression types and resolved callees are
  // rewritten in place). objectType: the instantiated class for members, or null.
  void retypeFor(FunctionDecl* f, const TemplateArgs& args, const Ty* objectType);
  // Re-types the body of a generic lambda (one with template parameters, written or invented for
  // its `auto` parameters) for one set of its own arguments, in the environment the lambda was
  // written in. Returns its call operator's type (parameters and result) or null; fullArgs gets
  // the enclosing instantiation's arguments plus the lambda's own, for declarations in the body.
  const Ty* retypeLambdaFor(const Ty* lambdaTy, const TemplateArgs& own, TemplateArgs* fullArgs);
  TemplateArgs classArgsOf(const Ty* classType); // Parameters of the class template -> arguments of the type.
  // Overload resolution by conversion ranking among candidates (members: objectType is the
  // object they are called on, classArgs its instantiation). Null if none is viable.
  FunctionDecl* chooseOverload(const std::vector<FunctionDecl*>& candidates, const std::vector<const Ty*>& argTypes,
                               const TemplateArgs& classArgs, const Ty* objectType, bool* ambiguous = nullptr);
  // The free function `operator<punctuator>(lhs[, rhs])` visible from context (the namespaces of
  // the operand classes and context's own), chosen by overload ranking; null if none.
  FunctionDecl* findFreeOperator(uint32_t punctuator, const Ty* lhs, const Ty* rhs, Decl* context);
  // Adds to args what f's own template parameters deduce to from the argument types (for a
  // constructor template chosen by code generation; args holds the class's arguments).
  void deduceArgs(FunctionDecl* f, const std::vector<const Ty*>& argTypes, TemplateArgs& args);
  // The value of a builtin boolean type trait (`isArray<T>`, `isSame<A, B>`...) written as a
  // name with template arguments, under the instantiation args; nullopt if it is not one or
  // its arguments are still dependent.
  std::optional<bool> typeTraitValue(NameExpr* e, Decl* context, const TemplateArgs& args);


private:
  struct Impl;
  Impl* impl;
};
