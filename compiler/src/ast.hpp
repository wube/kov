#pragma once
// AST for the Kov subset (tolerant of C++ so the Factorio corpus can be used as a benchmark).
// Nodes live in a per-file arena and are never freed individually.
#include "lexer.hpp"
#include <atomic>
#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

class Arena
{
public:
  // Sizes the first chunk so that a typical file needs exactly one allocation. Many small
  // allocations of a fixed large chunk size serialise on the OS page allocator.
  void reserve(size_t bytes)
  {
    if (capacity == 0 && bytes > 0)
    {
      chunks.emplace_back(new char[bytes]);
      base = chunks.back().get();
      capacity = bytes;
      pos = 0;
    }
  }
  void* allocate(size_t size, size_t align)
  {
    size_t aligned = (pos + align - 1) & ~(align - 1);
    if (aligned + size > capacity)
    {
      size_t chunk = std::max<size_t>(size + align, std::max<size_t>(capacity, 64 << 10));
      chunks.emplace_back(new char[chunk]); // Not value-initialised: no 1 MB memset per chunk.
      base = chunks.back().get();
      capacity = chunk;
      pos = 0;
      aligned = (reinterpret_cast<uintptr_t>(base) + align - 1) & ~(align - 1);
      aligned -= reinterpret_cast<uintptr_t>(base);
    }
    void* result = base + aligned;
    pos = aligned + size;
    bytes += size;
    return result;
  }
  template<typename T, typename... Args>
  T* make(Args&&... args)
  {
    return new (allocate(sizeof(T), alignof(T))) T(std::forward<Args>(args)...);
  }
  template<typename T>
  T* array(const T* src, size_t count)
  {
    if (count == 0)
      return nullptr;
    T* result = static_cast<T*>(allocate(sizeof(T) * count, alignof(T)));
    for (size_t i = 0; i < count; ++i)
      new (result + i) T(src[i]);
    return result;
  }
  size_t bytesAllocated() const { return bytes; }

private:
  std::vector<std::unique_ptr<char[]>> chunks;
  char* base = nullptr;
  size_t capacity = 0;
  size_t pos = 0;
  size_t bytes = 0;
};

template<typename T>
struct Span
{
  T* data = nullptr;
  uint32_t count = 0;
  T* begin() const { return data; }
  T* end() const { return data + count; }
  size_t size() const { return count; }
  bool empty() const { return count == 0; }
  T& operator[](size_t i) const { return data[i]; }
};

#define KOV_NODE_KINDS(X) \
  /* types */ \
  X(NamedType) X(PointerType) X(LRefType) X(RRefType) X(ConstType) X(ArrayType) X(FunctionType) \
  X(DecltypeType) X(AutoType) X(BuiltinType) X(MemberPointerType) X(PackType) \
  /* declarations */ \
  X(NamespaceDecl) X(ClassDecl) X(EnumDecl) X(EnumeratorDecl) X(FunctionDecl) X(ParamDecl) \
  X(VariableDecl) X(UsingAliasDecl) X(UsingDirectiveDecl) X(UsingNameDecl) X(TemplateParamDecl) \
  X(StaticAssertDecl) X(MacroDecl) X(ErrorDecl) X(EmptyDecl) X(AsmDecl) X(FriendDecl) \
  /* statements */ \
  X(BlockStmt) X(ExprStmt) X(DeclStmt) X(IfStmt) X(ForStmt) X(RangeForStmt) X(WhileStmt) \
  X(DoStmt) X(SwitchStmt) X(CaseStmt) X(DefaultStmt) X(ReturnStmt) X(BreakStmt) X(ContinueStmt) \
  X(GotoStmt) X(LabelStmt) X(TryStmt) X(EmptyStmt) X(ErrorStmt) X(FallthroughStmt) X(AsmStmt) X(MacroStmt) \
  /* expressions */ \
  X(NameExpr) X(LiteralExpr) X(ThisExpr) X(CallExpr) X(IndexExpr) X(MemberExpr) X(UnaryExpr) \
  X(BinaryExpr) X(ConditionalExpr) X(CastExpr) X(NamedCastExpr) X(SizeofExpr) X(NewExpr) \
  X(DeleteExpr) X(LambdaExpr) X(BraceInitExpr) X(ThrowExpr) X(FunctionalCastExpr) X(TypeidExpr) \
  X(FoldExpr) X(RequiresExpr) X(ErrorExpr) X(ParenExpr)

enum class NodeKind : uint8_t
{
#define X(name) name,
  KOV_NODE_KINDS(X)
#undef X
};
const char* nodeKindName(NodeKind kind);

struct Node
{
  NodeKind kind;
  uint32_t token; // Index of the first token, for diagnostics.
};

struct Type;
struct Expr;
struct Stmt;
struct Decl;

// One component of a qualified name: `name<args>`.
struct NamePart
{
  uint32_t symbol = UINT32_MAX; // UINT32_MAX for the leading `::` component or destructor/operator parts.
  Span<Node*> templateArgs;     // Type* or Expr*.
  bool hasTemplateArgs = false;
  // Special names:
  uint8_t special = 0; // 0 none, 1 destructor (~symbol), 2 operator (opToken = punctuator/keyword id), 3 conversion operator (convType)
  uint32_t opToken = 0;
  Type* convType = nullptr;
};

struct QualifiedName
{
  Span<NamePart> parts;
  bool global = false; // Leading `::`.
  bool empty() const { return parts.empty(); }
  const NamePart& last() const { return parts[parts.size() - 1]; }
};

// --- Types ---

struct Type : Node {};
struct NamedType : Type { QualifiedName name; bool typename_ = false; Decl* resolved = nullptr; };
struct BuiltinType : Type { Kw keyword; uint8_t longCount = 0; bool isUnsigned = false, isSigned = false; };
struct PointerType : Type { Type* pointee; };
struct LRefType : Type { Type* referenced; };
struct RRefType : Type { Type* referenced; };
struct ConstType : Type { Type* inner; bool isVolatile = false; };
struct ArrayType : Type { Type* element; Expr* size; };
struct FunctionType : Type { Type* result; Span<Decl*> params; bool isConst = false, isNoexcept = false; };
struct DecltypeType : Type { Expr* expr; bool isAuto = false; };
struct AutoType : Type { bool isDecltypeAuto = false; };
struct MemberPointerType : Type { Type* classType; Type* pointee; };
struct PackType : Type { Type* pattern; };

// --- Declarations ---

enum DeclFlags : uint32_t
{
  DF_Static = 1 << 0, DF_Virtual = 1 << 1, DF_Inline = 1 << 2, DF_Constexpr = 1 << 3,
  DF_Explicit = 1 << 4, DF_Friend = 1 << 5, DF_Extern = 1 << 6, DF_Mutable = 1 << 7,
  DF_Const = 1 << 8, DF_Override = 1 << 9, DF_Final = 1 << 10, DF_Pure = 1 << 11,
  DF_Default = 1 << 12, DF_Delete = 1 << 13, DF_Noexcept = 1 << 14, DF_ThreadLocal = 1 << 15,
  DF_Consteval = 1 << 16, DF_Implicit = 1 << 17, DF_Variadic = 1 << 18, DF_RefQual = 1 << 19,
  DF_Volatile = 1 << 20, DF_Typedef = 1 << 21, DF_Export = 1 << 22, DF_BothConst = 1 << 23,
  DF_Accumulate = 1 << 24, DF_Register = 1 << 25, DF_Operator = 1 << 26, DF_Constructor = 1 << 27,
  DF_Destructor = 1 << 28, DF_Conversion = 1 << 29, DF_Specialization = 1 << 30,
  DF_ExternC = 1u << 31, // Declared inside extern "C": C linkage, unmangled name.
};

enum class Access : uint8_t { Default, Public, Protected, Private };

struct Scope;
struct Ty;
struct ParsedFile;
struct Decl : Node
{
  ParsedFile* file = nullptr; // The file this declaration was parsed from.
  uint32_t flags = 0;
  Access access = Access::Default;
  Span<Decl*> templateParams; // TemplateParamDecl*. Non-empty for templates.
  bool isTemplate = false;    // `template<...>` was present (possibly empty for specializations).
  Decl* parent = nullptr;     // Lexically enclosing declaration, set by the parser.
  Scope* ownScope = nullptr;  // Set by the resolver for namespaces, classes and enums.
  std::atomic<const Ty*> cachedType{nullptr}; // Declared type, filled lazily by the type checker (non-dependent only).
};

struct LambdaExpr;
struct NamespaceDecl : Decl { uint32_t name = UINT32_MAX; Span<Decl*> decls; bool isInline = false; QualifiedName nested; };
struct TemplateParamDecl : Decl { uint32_t name = UINT32_MAX; bool isType = true; bool isPack = false; Type* valueType = nullptr; Node* defaultArg = nullptr; Span<Decl*> innerParams; };
struct ClassDecl : Decl
{
  Kw keyword = Kw::struct_;
  QualifiedName name; // Empty for anonymous.
  struct Base { Type* type; Access access; bool isVirtual; bool isPack; };
  Span<Base> bases;
  Span<Decl*> members;
  bool isDefinition = false;
  bool isFinal = false;
  bool isClassConst = false;
  bool isClosure = false;         // Synthetic class standing for a lambda's closure type.
  LambdaExpr* lambda = nullptr;   // The lambda, for closures.
};
struct EnumeratorDecl : Decl { uint32_t name; Expr* value = nullptr; };
struct EnumDecl : Decl
{
  QualifiedName name;
  bool isClass = false;
  bool isDefinition = false;
  Type* underlying = nullptr;
  Span<Decl*> enumerators;
  Span<Decl*> members; // Kov: methods inside enums.
};
struct ParamDecl : Decl { Type* type; uint32_t name = UINT32_MAX; Expr* defaultValue = nullptr; bool isPack = false; };
struct FunctionDecl : Decl
{
  Type* returnType = nullptr; // Null for constructors, destructors, conversion operators.
  QualifiedName name;
  Span<Decl*> params; // ParamDecl*
  Type* trailingReturn = nullptr;
  Stmt* body = nullptr;
  struct CtorInit { QualifiedName member; Span<Expr*> args; bool braced; bool isPack; };
  Span<CtorInit> ctorInits;
  Expr* requiresClause = nullptr;
};
struct VariableDecl : Decl
{
  Type* type = nullptr; // Type of the first declarator; others share the base type with their own declarator.
  QualifiedName name;
  Expr* init = nullptr;
  bool bracedInit = false;
  Expr* bitfield = nullptr;
  Span<Expr*> structuredBindings; // NameExpr* for `auto [a, b] = ...`
};
struct UsingAliasDecl : Decl { uint32_t name; Type* type; };
struct UsingDirectiveDecl : Decl { QualifiedName name; };
struct UsingNameDecl : Decl { QualifiedName name; bool isEnum = false; };
struct StaticAssertDecl : Decl { Expr* cond; };
struct MacroDecl : Decl { uint32_t name; uint32_t endToken; };
struct ErrorDecl : Decl { uint32_t endToken; };
struct EmptyDecl : Decl {};
struct AsmDecl : Decl {};
struct FriendDecl : Decl { Type* type; Decl* decl; };

// --- Statements ---

struct Stmt : Node {};
struct BlockStmt : Stmt { Span<Stmt*> stmts; };
struct ExprStmt : Stmt { Expr* expr; };
struct DeclStmt : Stmt { Span<Decl*> decls; };
struct IfStmt : Stmt { Stmt* init = nullptr; Node* cond; Stmt* then; Stmt* else_ = nullptr; bool isConstexpr = false; };
struct ForStmt : Stmt { Stmt* init = nullptr; Node* cond = nullptr; Expr* step = nullptr; Stmt* body; };
struct RangeForStmt : Stmt { Decl* var; Expr* range; Stmt* body; };
struct WhileStmt : Stmt { Node* cond; Stmt* body; };
struct DoStmt : Stmt { Stmt* body; Expr* cond; };
struct SwitchStmt : Stmt { Stmt* init = nullptr; Node* cond; Stmt* body; };
struct CaseStmt : Stmt { Expr* value; Expr* rangeEnd = nullptr; };
struct DefaultStmt : Stmt {};
struct ReturnStmt : Stmt { Expr* value = nullptr; };
struct BreakStmt : Stmt {};
struct ContinueStmt : Stmt {};
struct FallthroughStmt : Stmt {};
struct GotoStmt : Stmt { uint32_t label; };
struct LabelStmt : Stmt { uint32_t label; };
struct TryStmt : Stmt { Stmt* body; struct Handler { Decl* param; Stmt* body; }; Span<Handler> handlers; };
struct EmptyStmt : Stmt {};
struct AsmStmt : Stmt {};
struct ErrorStmt : Stmt { uint32_t endToken; };
struct MacroStmt : Stmt { uint32_t name; };

// --- Expressions ---

struct Expr : Node { const Ty* ty = nullptr; }; // ty: filled by the type checker.
struct NameExpr : Expr { QualifiedName name; Decl* resolved = nullptr; };
struct LiteralExpr : Expr { TokenKind literalKind; Kw keyword; }; // keyword for true/false/nullptr
struct ThisExpr : Expr {};
struct CallExpr : Expr
{
  Expr* callee;
  Span<Expr*> args;
  // Deduced template arguments of the called function template, in parameter order (filled by
  // the type checker; at most 4, which is all code generation supports).
  const Ty* templateArgs[4] = {};
  uint8_t templateArgCount = 0;
};
struct IndexExpr : Expr { Expr* base; Span<Expr*> args; };
struct MemberExpr : Expr { Expr* base; QualifiedName member; P op; Decl* resolved = nullptr; };
struct UnaryExpr : Expr { Expr* operand; uint32_t op; bool isKeyword; bool postfix; };
struct BinaryExpr : Expr { Expr* lhs; Expr* rhs; uint32_t op; bool isKeyword; };
struct ConditionalExpr : Expr { Expr* cond; Expr* then; Expr* else_; }; // then == null for Elvis.
struct CastExpr : Expr { Type* type; Expr* operand; };
struct NamedCastExpr : Expr { Kw castKind; Type* type; Expr* operand; };
struct SizeofExpr : Expr { Kw sizeofKind; Type* type = nullptr; Expr* operand = nullptr; bool isPack = false; };
struct NewExpr : Expr { Type* type; Span<Expr*> placement; Span<Expr*> args; bool braced = false; bool global = false; };
struct DeleteExpr : Expr { Expr* operand; bool isArray; bool global; };
struct LambdaCapture
{
  uint32_t symbol = UINT32_MAX; // Captured variable (UINT32_MAX for `this`).
  bool byRef = false;
  bool isThis = false;
  Expr* init = nullptr;         // Init-capture `[x = expr]`.
};
struct LambdaExpr : Expr
{
  Span<Decl*> params;
  Stmt* body;
  Type* returnType = nullptr;
  Span<Decl*> templateParams;
  Span<LambdaCapture> captures;
  uint8_t defaultCapture = 0; // 0 none, 1 by copy (=), 2 by reference (&)
  Decl* closure = nullptr;    // Synthetic ClassDecl giving this lambda an identity and a name.
};
struct BraceInitExpr : Expr { Span<Expr*> args; };
struct ThrowExpr : Expr { Expr* operand; };
struct FunctionalCastExpr : Expr { Type* type; Span<Expr*> args; bool braced; };
struct TypeidExpr : Expr { Node* operand; };
struct FoldExpr : Expr { Expr* lhs; Expr* rhs; uint32_t op; };
struct RequiresExpr : Expr {};
struct ParenExpr : Expr { Expr* inner; };
struct ErrorExpr : Expr {};

struct Diagnostic
{
  uint32_t token;
  const char* message;
};

struct ParsedFile
{
  Arena arena;
  Span<Decl*> decls;
  const std::vector<Token>* tokens = nullptr; // Set by parseFile; the driver keeps the vector alive.
  std::string_view text;
  std::string path; // Set by the driver, for diagnostics.
  std::vector<Diagnostic> diagnostics;
  uint32_t declCount = 0; // All declaration nodes created.
  uint32_t functionBodies = 0;
  uint32_t nodeCount = 0;
};
