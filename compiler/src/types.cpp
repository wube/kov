#include "types.hpp"
#include <cstring>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <vector>

namespace
{
  uint64_t mix(uint64_t h, uint64_t v)
  {
    h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
    return h;
  }

  uint64_t hashTy(const Ty& t)
  {
    uint64_t h = uint64_t(t.kind);
    h = mix(h, t.bits);
    h = mix(h, (t.isUnsigned << 0) | (t.isConst << 1) | (t.isVolatile << 2) | (t.variadic << 3));
    h = mix(h, t.count);
    h = mix(h, uint64_t(t.value));
    h = mix(h, reinterpret_cast<uintptr_t>(t.inner));
    h = mix(h, reinterpret_cast<uintptr_t>(t.decl));
    h = mix(h, t.args.size());
    for (const Ty* a : t.args)
      h = mix(h, reinterpret_cast<uintptr_t>(a));
    return h;
  }

  bool sameTy(const Ty& a, const Ty& b)
  {
    if (a.kind != b.kind || a.bits != b.bits || a.isUnsigned != b.isUnsigned || a.isConst != b.isConst ||
        a.isVolatile != b.isVolatile || a.variadic != b.variadic || a.count != b.count || a.value != b.value || a.inner != b.inner ||
        a.decl != b.decl || a.args.size() != b.args.size())
      return false;
    for (size_t i = 0; i < a.args.size(); ++i)
      if (a.args[i] != b.args[i])
        return false;
    return true;
  }
}

struct TypeTable::Impl
{
  static constexpr uint32_t shardCount = 64;
  struct Shard
  {
    std::shared_mutex mutex;
    std::unordered_multimap<uint64_t, const Ty*> map;
    std::vector<std::unique_ptr<Ty>> owned;
    std::vector<std::unique_ptr<const Ty*[]>> argStorage;
  };
  Shard shards[shardCount];
  std::atomic<size_t> count{0};
};

TypeTable::TypeTable() : impl(new Impl)
{
  Ty t{TyKind::Void};
  void_ = intern(t);
  t = Ty{TyKind::Bool};
  bool_ = intern(t);
  t = Ty{TyKind::Nullptr};
  nullptr_ = intern(t);
  t = Ty{TyKind::Unknown};
  unknown_ = intern(t);
  t = Ty{TyKind::Auto};
  auto_ = intern(t);
}

TypeTable::~TypeTable() { delete impl; }

const Ty* TypeTable::intern(const Ty& t)
{
  uint64_t h = hashTy(t);
  Impl::Shard& shard = impl->shards[h % Impl::shardCount];
  {
    std::shared_lock lock(shard.mutex);
    auto range = shard.map.equal_range(h);
    for (auto it = range.first; it != range.second; ++it)
      if (sameTy(*it->second, t))
        return it->second;
  }
  std::unique_lock lock(shard.mutex);
  auto range = shard.map.equal_range(h);
  for (auto it = range.first; it != range.second; ++it)
    if (sameTy(*it->second, t))
      return it->second;
  auto owned = std::make_unique<Ty>(t);
  if (!t.args.empty())
  {
    // Copy the argument list so the caller's storage can be temporary.
    auto storage = std::make_unique<const Ty*[]>(t.args.size());
    for (size_t i = 0; i < t.args.size(); ++i)
      storage[i] = t.args[i];
    owned->args.data = storage.get();
    shard.argStorage.push_back(std::move(storage));
  }
  const Ty* result = owned.get();
  shard.owned.push_back(std::move(owned));
  shard.map.emplace(h, result);
  ++impl->count;
  return result;
}

size_t TypeTable::size() const { return impl->count.load(); }

const Ty* TypeTable::intType(uint8_t bits, bool isUnsigned)
{
  Ty t{TyKind::Int};
  t.bits = bits;
  t.isUnsigned = isUnsigned;
  return intern(t);
}

const Ty* TypeTable::charType(uint8_t bits, bool isUnsigned)
{
  Ty t{TyKind::Char};
  t.bits = bits;
  t.isUnsigned = isUnsigned;
  return intern(t);
}

const Ty* TypeTable::floatType(uint8_t bits)
{
  Ty t{TyKind::Float};
  t.bits = bits;
  return intern(t);
}

const Ty* TypeTable::pack(Span<const Ty*> elements)
{
  Ty t{TyKind::Pack};
  t.args = elements;
  return intern(t);
}

// Type constructors distribute over packs: `const Args&...` with Args = (int, String) is the
// pack (const int&, const String&).
#define KOV_DISTRIBUTE(expr) \
  if (inner && inner->kind == TyKind::Pack) \
  { \
    std::vector<const Ty*> mapped; \
    for (const Ty* e : inner->args) \
    { \
      const Ty* inner = e; \
      mapped.push_back(expr); \
    } \
    return pack(Span<const Ty*>{mapped.data(), uint32_t(mapped.size())}); \
  }

const Ty* TypeTable::pointer(const Ty* inner)
{
  KOV_DISTRIBUTE(pointer(inner))
  Ty t{TyKind::Pointer};
  t.inner = inner;
  return intern(t);
}

const Ty* TypeTable::lref(const Ty* inner)
{
  KOV_DISTRIBUTE(lref(inner))
  if (inner->isReference())
    return inner->kind == TyKind::LRef ? inner : lref(inner->inner); // reference collapsing
  Ty t{TyKind::LRef};
  t.inner = inner;
  return intern(t);
}

const Ty* TypeTable::rref(const Ty* inner)
{
  KOV_DISTRIBUTE(rref(inner))
  if (inner->isReference())
    return inner;
  Ty t{TyKind::RRef};
  t.inner = inner;
  return intern(t);
}

const Ty* TypeTable::array(const Ty* element, uint32_t count)
{
  Ty t{TyKind::Array};
  t.inner = element;
  t.count = count;
  return intern(t);
}

const Ty* TypeTable::function(const Ty* result, Span<const Ty*> params, bool variadic)
{
  Ty t{TyKind::Function};
  t.inner = result;
  t.args = params;
  t.variadic = variadic;
  return intern(t);
}

const Ty* TypeTable::classType(Decl* decl, Span<const Ty*> args)
{
  Ty t{TyKind::Class};
  t.decl = decl;
  t.args = args;
  return intern(t);
}

const Ty* TypeTable::enumType(Decl* decl)
{
  Ty t{TyKind::Enum};
  t.decl = decl;
  return intern(t);
}

const Ty* TypeTable::valueType(const Ty* of, int64_t value)
{
  Ty t{TyKind::Value};
  t.inner = of;
  t.value = value;
  return intern(t);
}

const Ty* TypeTable::param(Decl* decl)
{
  Ty t{TyKind::Param};
  t.decl = decl;
  return intern(t);
}

const Ty* TypeTable::lambda(const Ty* result, Span<const Ty*> params, Decl* closure, int64_t instance)
{
  Ty t{TyKind::Lambda};
  t.inner = result;
  t.args = params;
  t.decl = closure;
  t.value = instance;
  return intern(t);
}

const Ty* TypeTable::memberPointer(const Ty* member, const Ty* cls)
{
  Ty t{TyKind::MemberPointer};
  t.inner = member;
  t.decl = cls && cls->kind == TyKind::Class ? cls->decl : nullptr;
  const Ty* args[1] = {cls};
  if (cls)
    t.args = Span<const Ty*>{args, 1};
  return intern(t);
}

const Ty* TypeTable::qualified(const Ty* t, bool isConst, bool isVolatile)
{
  if (!t || (!isConst && !isVolatile) || t->isReference() || t->kind == TyKind::Function)
    return t;
  if (t->kind == TyKind::Pack)
  {
    std::vector<const Ty*> mapped;
    for (const Ty* e : t->args)
      mapped.push_back(qualified(e, isConst, isVolatile));
    return pack(Span<const Ty*>{mapped.data(), uint32_t(mapped.size())});
  }
  if (t->kind == TyKind::Array)
    return array(qualified(t->inner, isConst, isVolatile), t->count);
  Ty copy = *t;
  copy.isConst |= isConst;
  copy.isVolatile |= isVolatile;
  if (copy.isConst == t->isConst && copy.isVolatile == t->isVolatile)
    return t;
  return intern(copy);
}

const Ty* TypeTable::unqualified(const Ty* t)
{
  if (!t || (!t->isConst && !t->isVolatile))
    return t;
  Ty copy = *t;
  copy.isConst = false;
  copy.isVolatile = false;
  return intern(copy);
}

const Ty* TypeTable::decay(const Ty* t)
{
  if (!t)
    return t;
  t = stripRef(t);
  if (t->kind == TyKind::Array)
    return pointer(t->inner);
  if (t->kind == TyKind::Function)
    return pointer(t);
  return unqualified(t);
}

bool TypeTable::isDependent(const Ty* t) const
{
  if (!t)
    return false;
  switch (t->kind)
  {
    case TyKind::Param: case TyKind::Auto: return true;
    case TyKind::Pointer: case TyKind::LRef: case TyKind::RRef: case TyKind::Array: case TyKind::Value:
      return isDependent(t->inner);
    case TyKind::MemberPointer:
      return isDependent(t->inner) || (!t->args.empty() && isDependent(t->args[0]));
    case TyKind::Function: case TyKind::Class: case TyKind::Lambda: case TyKind::Pack:
      if (isDependent(t->inner))
        return true;
      for (const Ty* a : t->args)
        if (isDependent(a))
          return true;
      return false;
    default:
      return false;
  }
}

std::string TypeTable::toString(const Ty* t, const SymbolTable* symbols) const
{
  if (!t)
    return "<null>";
  std::string s;
  if (t->isConst)
    s += "const ";
  if (t->isVolatile)
    s += "volatile ";
  auto declName = [&](Decl* d) -> std::string
  {
    if (!d || !symbols)
      return "?";
    const QualifiedName* name = nullptr;
    if (d->kind == NodeKind::ClassDecl)
      name = &static_cast<ClassDecl*>(d)->name;
    else if (d->kind == NodeKind::EnumDecl)
      name = &static_cast<EnumDecl*>(d)->name;
    else if (d->kind == NodeKind::TemplateParamDecl)
    {
      uint32_t n = static_cast<TemplateParamDecl*>(d)->name;
      return n == UINT32_MAX ? "T" : std::string(symbols->name(n));
    }
    if (!name || name->empty() || name->last().symbol == UINT32_MAX)
      return "?";
    return std::string(symbols->name(name->last().symbol));
  };
  switch (t->kind)
  {
    case TyKind::Void: s += "void"; break;
    case TyKind::Bool: s += "bool"; break;
    case TyKind::Char: s += t->bits == 8 ? (t->isUnsigned ? "unsigned char" : "char") : t->bits == 16 ? "char16_t" : "char32_t"; break;
    case TyKind::Int:
      if (t->isUnsigned)
        s += "u";
      s += "int" + std::to_string(t->bits);
      break;
    case TyKind::Float: s += t->bits == 32 ? "float" : "double"; break;
    case TyKind::Nullptr: s += "nullptr_t"; break;
    case TyKind::Pointer: s += toString(t->inner, symbols) + "*"; break;
    case TyKind::LRef: s += toString(t->inner, symbols) + "&"; break;
    case TyKind::RRef: s += toString(t->inner, symbols) + "&&"; break;
    case TyKind::Array: s += toString(t->inner, symbols) + "[" + (t->count ? std::to_string(t->count) : "") + "]"; break;
    case TyKind::Lambda:
      if (t->decl)
      {
        s += declName(t->decl);
        if (t->value != 0)
        {
          char buf[24];
          snprintf(buf, sizeof buf, "#%llx", (unsigned long long)t->value); // the enclosing instantiation
          s += buf;
        }
        break;
      }
      [[fallthrough]];
    case TyKind::Function:
      s += (t->kind == TyKind::Lambda ? "lambda " : "") + toString(t->inner, symbols) + "(";
      for (size_t i = 0; i < t->args.size(); ++i)
        s += (i ? ", " : "") + toString(t->args[i], symbols);
      s += t->variadic ? ", ...)" : ")";
      break;
    case TyKind::Class:
      s += declName(t->decl);
      if (!t->args.empty())
      {
        s += "<";
        for (size_t i = 0; i < t->args.size(); ++i)
          s += (i ? ", " : "") + toString(t->args[i], symbols);
        s += ">";
      }
      if (t->decl && (t->decl->flags & DF_Specialization) && !t->decl->templateParams.empty())
        s += "/spec" + std::to_string(t->decl->token); // a partial specialisation's own arguments: not the primary's instantiation
      break;
    case TyKind::Enum: s += declName(t->decl); break;
    case TyKind::Param: s += declName(t->decl); break;
    case TyKind::Value:
      s += t->inner && t->inner->kind == TyKind::Bool ? (t->value ? "true" : "false") : std::to_string(t->value);
      break;
    case TyKind::MemberPointer:
      s += toString(t->inner, symbols) + " " + (t->args.empty() ? declName(t->decl) : toString(t->args[0], symbols)) + "::*";
      break;
    case TyKind::Auto: s += "auto"; break;
    case TyKind::Pack:
      for (size_t i = 0; i < t->args.size(); ++i)
        s += (i ? ", " : "") + toString(t->args[i], symbols);
      break;
    case TyKind::Unknown: s += "<unknown>"; break;
  }
  return s;
}
