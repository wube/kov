#pragma once
// Canonical types. Every distinct type exists exactly once in the TypeTable, so type equality is
// pointer equality and two threads that build the same type (e.g. the same template
// instantiation) get the same object. Qualifiers are part of the type: `const int` and `int`
// are different Ty objects that share nothing but their kind.
#include "ast.hpp"
#include <cstdint>
#include <string>

enum class TyKind : uint8_t
{
  Void,
  Bool,
  Char,    // bits = 8/16/32, isUnsigned
  Int,     // bits = 8/16/32/64, isUnsigned
  Float,   // bits = 32/64
  Nullptr,
  Pointer, // inner
  LRef,    // inner
  RRef,    // inner
  Array,   // inner, count (0 = unknown bound)
  Function,// inner = result, args = parameters, variadic
  Class,   // decl = ClassDecl (definition if known), args = template arguments (empty for non-templates)
  Enum,    // decl = EnumDecl
  Param,   // decl = TemplateParamDecl: dependent on a template parameter
  Lambda,  // inner = return type (null if unknown), args = parameters; decl = the LambdaExpr owner (unused)
  MemberPointer, // inner = member type, decl = owning class decl (may be null)
  Auto,    // Placeholder inside a declared type, replaced by deduction.
  Pack,    // args = the types a parameter pack was deduced to.
  Value,   // A non-type template argument: inner = its type, value = the constant.
  Unknown, // Could not be determined; reason is tracked in the statistics, not in the type.
};

struct Ty
{
  TyKind kind;
  uint8_t bits = 0;
  bool isUnsigned = false;
  bool isConst = false;
  bool isVolatile = false;
  bool variadic = false;
  uint32_t count = 0;
  int64_t value = 0; // Value kinds only.
  const Ty* inner = nullptr;
  Span<const Ty*> args;
  Decl* decl = nullptr;

  bool isReference() const { return kind == TyKind::LRef || kind == TyKind::RRef; }
  bool isArithmetic() const { return kind == TyKind::Bool || kind == TyKind::Char || kind == TyKind::Int || kind == TyKind::Float; }
  bool isIntegral() const { return kind == TyKind::Bool || kind == TyKind::Char || kind == TyKind::Int || kind == TyKind::Enum; }
  bool isKnown() const { return kind != TyKind::Unknown; }
};

class TypeTable
{
public:
  TypeTable();
  ~TypeTable();

  const Ty* voidType() const { return void_; }
  const Ty* boolType() const { return bool_; }
  const Ty* nullptrType() const { return nullptr_; }
  const Ty* unknown() const { return unknown_; }
  const Ty* autoType() const { return auto_; }
  const Ty* intType(uint8_t bits = 32, bool isUnsigned = false);
  const Ty* charType(uint8_t bits = 8, bool isUnsigned = false);
  const Ty* floatType(uint8_t bits = 64);
  const Ty* sizeType() { return intType(64, true); }
  const Ty* ptrdiffType() { return intType(64, false); }

  const Ty* pointer(const Ty* pointee);
  const Ty* lref(const Ty* t);
  const Ty* rref(const Ty* t);
  const Ty* array(const Ty* element, uint32_t count);
  const Ty* function(const Ty* result, Span<const Ty*> params, bool variadic);
  const Ty* classType(Decl* decl, Span<const Ty*> args);
  const Ty* enumType(Decl* decl);
  const Ty* param(Decl* decl);
  const Ty* valueType(const Ty* of, int64_t value); // a non-type template argument
  // instance: a hash of the enclosing instantiation's arguments, so that a lambda written in a
  // template is a different type (with its own closure and function) in each instantiation.
  const Ty* lambda(const Ty* result, Span<const Ty*> params, Decl* closure = nullptr, int64_t instance = 0);
  const Ty* memberPointer(const Ty* member, const Ty* cls); // args[0] = the class (may be a parameter), decl = its declaration
  const Ty* pack(Span<const Ty*> elements);
  // Same type with qualifiers added (references and functions ignore them).
  const Ty* qualified(const Ty* t, bool isConst, bool isVolatile);
  const Ty* unqualified(const Ty* t);

  // Reference and top-level qualifier removal, as for `auto` deduction.
  const Ty* decay(const Ty* t);
  // Reference removal only.
  const Ty* stripRef(const Ty* t) const { return t && t->isReference() ? t->inner : t; }

  bool isDependent(const Ty* t) const;
  std::string toString(const Ty* t, const SymbolTable* symbols) const;
  size_t size() const;

private:
  const Ty* intern(const Ty& t);
  struct Impl;
  Impl* impl;
  const Ty* void_;
  const Ty* bool_;
  const Ty* nullptr_;
  const Ty* unknown_;
  const Ty* auto_;
};
