#include "codegen.hpp"
#include <cstdio>
#include <cstring>
#include <functional>
#include <optional>
#include <unordered_map>

namespace
{
  struct StructInfo
  {
    std::string name;                        // %struct.N
    std::vector<const Ty*> fieldTypes;       // In layout order: bases first, then data members.
    std::unordered_map<Decl*, uint32_t> fieldIndex;
    std::vector<std::pair<Decl*, uint32_t>> bases; // base class definition -> field index
    bool emitted = false;
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
    FunctionDecl* currentFunction = nullptr;
    const Ty* currentReturn = nullptr;

    struct Local
    {
      std::string addr; // Address of the variable (or of the pointer slot for references).
      const Ty* ty;
      bool isRef;
    };
    std::unordered_map<Decl*, Local> locals;
    std::unordered_map<Decl*, StructInfo> structs;
    std::unordered_map<Decl*, std::string> globals;    // VariableDecl -> @name
    std::unordered_map<Decl*, std::string> functionNames;
    std::unordered_map<Decl*, bool> declared;          // functions with a declare/define line
    std::unordered_map<Decl*, int64_t> enumeratorValues;
    struct Loop
    {
      std::string breakLabel, continueLabel;
    };
    std::vector<Loop> loops;
    ParsedFile* currentFile = nullptr;

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
        where = std::string(currentFile->path) + ":" + std::to_string(line);
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
        case TyKind::Class: return structFor(t->decl, at).name;
        case TyKind::Enum: return llType(enumUnderlying(t->decl), at);
        default:
          unsupported(at, "the type " + types.toString(t, &symbols));
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

    // Size and alignment, for sizeof and layout.
    uint64_t sizeOf(const Ty* t, Node* at = nullptr)
    {
      switch (t->kind)
      {
        case TyKind::Bool: return 1;
        case TyKind::Char: case TyKind::Int: return t->bits / 8;
        case TyKind::Float: return t->bits / 8;
        case TyKind::Nullptr: case TyKind::Pointer: case TyKind::LRef: case TyKind::RRef: case TyKind::Function:
        case TyKind::Lambda: case TyKind::MemberPointer:
          return 8;
        case TyKind::Array: return t->count * sizeOf(t->inner, at);
        case TyKind::Enum: return sizeOf(enumUnderlying(t->decl), at);
        case TyKind::Class:
        {
          StructInfo& info = structFor(t->decl, at);
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
          for (const Ty* f : structFor(t->decl, at).fieldTypes)
            align = std::max(align, alignOf(f, at));
          return align;
        }
        default: return std::max<uint64_t>(1, sizeOf(t, at));
      }
    }

    StructInfo& structFor(Decl* decl, Node* at)
    {
      Decl* def = global.definitionOf(decl);
      if (!def)
        unsupported(at, "a class without a definition (" + global.qualifiedName(decl) + ")");
      auto it = structs.find(def);
      if (it != structs.end())
        return it->second;
      StructInfo& info = structs[def];
      info.name = "%struct." + std::to_string(structs.size());
      auto* c = static_cast<ClassDecl*>(def);
      if (c->keyword == Kw::union_)
        unsupported(c, "unions");
      for (const ClassDecl::Base& b : c->bases)
      {
        const Ty* bt = global.typeOfAst(b.type, c);
        if (!bt || bt->kind != TyKind::Class)
          unsupported(c, "a base class that is not a known class");
        Decl* baseDef = global.definitionOf(bt->decl);
        if (!baseDef)
          unsupported(c, "a base class without a definition");
        structFor(baseDef, c);
        info.bases.push_back({baseDef, uint32_t(info.fieldTypes.size())});
        info.fieldTypes.push_back(bt);
      }
      for (Decl* m : c->members)
      {
        if (m->kind != NodeKind::VariableDecl || (m->flags & (DF_Static | DF_Typedef)))
          continue;
        const Ty* ft = global.typeOfDecl(m);
        if (!ft || !ft->isKnown())
          unsupported(m, "a member of unknown type (" + sym(static_cast<VariableDecl*>(m)->name.last().symbol) + ")");
        info.fieldIndex[m] = uint32_t(info.fieldTypes.size());
        info.fieldTypes.push_back(ft);
      }
      std::string line = info.name + " = type { ";
      for (size_t i = 0; i < info.fieldTypes.size(); ++i)
        line += (i ? ", " : "") + llType(info.fieldTypes[i], c);
      line += " }\n";
      decls += line;
      info.emitted = true;
      ++result.structs;
      return info;
    }

    // Address of a field of `def` reachable from an object address, searching bases.
    bool fieldAddress(Decl* def, Decl* member, const std::string& objectAddr, std::string& out, Node* at)
    {
      StructInfo& info = structFor(def, at);
      auto it = info.fieldIndex.find(member);
      if (it != info.fieldIndex.end())
      {
        out = newTemp();
        fn += "  " + out + " = getelementptr inbounds " + info.name + ", ptr " + objectAddr + ", i32 0, i32 " +
              std::to_string(it->second) + "\n";
        return true;
      }
      for (auto& [baseDef, index] : info.bases)
      {
        std::string baseAddr = newTemp();
        fn += "  " + baseAddr + " = getelementptr inbounds " + info.name + ", ptr " + objectAddr + ", i32 0, i32 " +
              std::to_string(index) + "\n";
        if (fieldAddress(baseDef, member, baseAddr, out, at))
          return true;
      }
      return false;
    }

    std::string functionName(FunctionDecl* f)
    {
      auto it = functionNames.find(f);
      if (it != functionNames.end())
        return it->second;
      std::string name;
      if (f->flags & DF_Extern || global.isExternC(f))
        name = "@" + sym(f->name.last().symbol);
      else if (f->name.parts.size() == 1 && sym(f->name.parts[0].symbol) == "main" && !global.owningClass(f))
        name = "@main";
      else
      {
        const Ty* ft = global.typeOfDecl(f);
        std::string q = global.qualifiedName(f);
        std::string params;
        for (size_t i = 0; ft && i < ft->args.size(); ++i)
          params += (i ? ", " : "") + types.toString(ft->args[i], &symbols);
        name = "@\"" + q + "(" + params + ")\"";
      }
      functionNames[f] = name;
      return name;
    }

    // Signature text: "<ret> (<params>)" and the parameter list for a definition.
    std::string signature(FunctionDecl* f, bool withNames, std::vector<std::pair<std::string, const Ty*>>* paramsOut)
    {
      const Ty* ft = global.typeOfDecl(f);
      if (!ft || ft->kind != TyKind::Function)
        unsupported(f, "a function whose type is unknown");
      std::string s = llType(ft->inner, f) + " ";
      std::string params;
      bool method = global.owningClass(f) && !(f->flags & DF_Static) && !global.isExternC(f);
      if (method)
      {
        params += "ptr";
        if (withNames)
          params += " %this";
        if (paramsOut)
          paramsOut->push_back({"%this", nullptr});
      }
      for (size_t i = 0; i < ft->args.size(); ++i)
      {
        const Ty* pt = ft->args[i];
        if (!pt->isKnown())
          unsupported(f, "a parameter of unknown type");
        if (!params.empty())
          params += ", ";
        params += llType(pt, f);
        if (withNames)
        {
          std::string pn = "%p" + std::to_string(i);
          params += " " + pn;
          if (paramsOut)
            paramsOut->push_back({pn, pt});
        }
      }
      if (ft->variadic)
        params += params.empty() ? "..." : ", ...";
      return s + "(" + params + ")";
    }

    void ensureDeclared(FunctionDecl* f)
    {
      FunctionDecl* def = global.definitionOf(f) && global.definitionOf(f)->kind == NodeKind::FunctionDecl
                            ? static_cast<FunctionDecl*>(global.definitionOf(f)) : f;
      if (def->body)
        return; // Defined in this program.
      if (declared[f])
        return;
      declared[f] = true;
      std::string sig = signature(f, false, nullptr);
      size_t space = sig.find(' ');
      decls += "declare " + sig.substr(0, space) + " " + functionName(f) + sig.substr(space + 1) + "\n";
    }

    // --- constants ---

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
            std::string s(spelling(l->token));
            std::string digits;
            for (char c : s)
              if (c != '\'')
                digits += c;
            try
            {
              size_t pos = 0;
              long long v = std::stoll(digits, &pos, 0);
              return v;
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
          if (d->kind == NodeKind::VariableDecl && (d->flags & DF_Constexpr || static_cast<VariableDecl*>(d)->type))
          {
            auto* v = static_cast<VariableDecl*>(d);
            const Ty* t = global.typeOfDecl(v);
            if (t && t->isConst && v->init && (d->flags & (DF_Constexpr | DF_Static)) != 0)
              return evalConst(v->init, depth + 1);
            if (t && t->isConst && v->init && d->parent == nullptr)
              return evalConst(v->init, depth + 1);
          }
          return std::nullopt;
        }
        case NodeKind::SizeofExpr:
        {
          auto* s = static_cast<SizeofExpr*>(e);
          const Ty* t = nullptr;
          if (s->type && s->type->kind == NodeKind::NamedType && static_cast<NamedType*>(s->type)->resolved &&
              (static_cast<NamedType*>(s->type)->resolved->kind == NodeKind::VariableDecl ||
               static_cast<NamedType*>(s->type)->resolved->kind == NodeKind::ParamDecl))
            t = global.typeOfDecl(static_cast<NamedType*>(s->type)->resolved);
          else
            t = s->type ? global.typeOfAst(s->type, currentFunction) : (s->operand ? s->operand->ty : nullptr);
          if (!t || !t->isKnown())
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
      if (e)
        for (Decl* d : e->enumerators)
        {
          if (d->kind != NodeKind::EnumeratorDecl)
            continue;
          auto* cur = static_cast<EnumeratorDecl*>(d);
          if (cur->value)
          {
            auto v = evalConst(cur->value);
            if (v)
              next = *v;
          }
          enumeratorValues[cur] = next;
          ++next;
        }
      it = enumeratorValues.find(en);
      return it == enumeratorValues.end() ? 0 : it->second;
    }

    std::string_view spelling(uint32_t token) const
    {
      const std::vector<Token>& toks = *currentFile->tokens;
      const Token& t = toks[std::min<size_t>(token, toks.size() - 1)];
      return currentFile->text.substr(t.offset, t.length);
    }

    // Decodes one C string or char literal token (with prefix) into bytes.
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
        ++i;
        if (i >= s.size())
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
      // Adjacent string literal tokens are one literal.
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

    struct Value
    {
      std::string text; // IR operand
      const Ty* ty;     // Value type (never a reference)
    };

    static const Ty* unref(TypeTable& types, const Ty* t) { return types.stripRef(t); }

    // Converts a value to another type (implicit or explicit conversion rules of the subset).
    Value convert(Value v, const Ty* to, Node* at)
    {
      const Ty* from = v.ty;
      to = types.stripRef(to);
      if (!to || !to->isKnown())
        unsupported(at, "a conversion to an unknown type");
      if (from == to || types.unqualified(from) == types.unqualified(to))
        return {v.text, to};
      auto kindOf = [&](const Ty* t) { return t->kind == TyKind::Enum ? TyKind::Int : t->kind; };
      TyKind fk = kindOf(from), tk = kindOf(to);
      auto bitsOf = [&](const Ty* t) -> uint8_t { return t->kind == TyKind::Enum ? uint8_t(enumUnderlying(t->decl)->bits) : t->bits; };
      auto unsignedOf = [&](const Ty* t) { return t->kind == TyKind::Enum ? enumUnderlying(t->decl)->isUnsigned : t->isUnsigned; };
      std::string r = newTemp();
      std::string fromT = llType(from, at), toT = llType(to, at);
      if (fk == TyKind::Class || tk == TyKind::Class)
      {
        if (fk == TyKind::Class && tk == TyKind::Class)
          return {v.text, to}; // Same layout assumed (derived-to-base by value is not supported).
        unsupported(at, "a conversion between a class and a non-class type");
      }
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
        return {fk == TyKind::Nullptr ? "null" : v.text, to};
      if (tk == TyKind::Pointer && (fk == TyKind::Int))
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

    // Usual arithmetic conversions.
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
        // Arrays decay to a pointer to their first element.
        std::string r = newTemp();
        fn += "  " + r + " = getelementptr inbounds " + llType(ty, at) + ", ptr " + addr + ", i64 0, i64 0\n";
        return {r, types.pointer(ty->inner)};
      }
      std::string r = newTemp();
      fn += "  " + r + " = load " + llType(ty, at) + ", ptr " + addr + "\n";
      return {r, types.unqualified(ty)};
    }

    void store(Value v, const std::string& addr, const Ty* ty, Node* at)
    {
      Value c = convert(v, ty, at);
      fn += "  store " + llType(types.stripRef(ty), at) + " " + c.text + ", ptr " + addr + "\n";
    }

    // Address of an lvalue expression.
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
              // Implicit this->member.
              auto thisIt = locals.find(nullptr);
              if (thisIt == locals.end())
                unsupported(e, "a member used outside a method");
              std::string thisPtr = newTemp();
              fn += "  " + thisPtr + " = load ptr, ptr " + thisIt->second.addr + "\n";
              std::string addr;
              if (!fieldAddress(cls, d, thisPtr, addr, e))
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
          const Ty* baseTy = m->base->ty;
          if (m->op == P::Arrow)
          {
            Value p = rvalue(m->base);
            if (p.ty->kind != TyKind::Pointer)
              unsupported(e, "-> on something that is not a pointer");
            object = p.text;
            baseTy = p.ty->inner;
          }
          else
            object = lvalue(m->base);
          baseTy = types.stripRef(baseTy);
          if (!baseTy || baseTy->kind != TyKind::Class)
            unsupported(e, "a member access on a non-class object");
          if (d->flags & DF_Static)
            return globalFor(static_cast<VariableDecl*>(d), e);
          std::string addr;
          if (!fieldAddress(baseTy->decl, d, object, addr, e))
            unsupported(e, "a member that is not a field of the object's class");
          return addr;
        }
        case NodeKind::UnaryExpr:
        {
          auto* u = static_cast<UnaryExpr*>(e);
          if (!u->isKeyword && P(u->op) == P::Star)
          {
            Value p = rvalue(u->operand);
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
            rvalue(e);
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
          // A call returning a reference is an lvalue.
          Value v = callValue(static_cast<CallExpr*>(e), true);
          return v.text;
        }
        default:
          break;
      }
      // Materialise a temporary for an rvalue used as an lvalue (e.g. binding a const reference).
      Value v = rvalue(e);
      std::string slot = newTemp();
      allocas += "  " + slot + " = alloca " + llType(v.ty, e) + "\n";
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
      if (v->init)
      {
        auto c = evalConst(v->init);
        const Ty* st = types.stripRef(t);
        if (c && (st->kind == TyKind::Int || st->kind == TyKind::Char || st->kind == TyKind::Enum))
          init = std::to_string(*c);
        else if (c && st->kind == TyKind::Bool)
          init = *c ? "true" : "false";
        else if (v->init->kind == NodeKind::LiteralExpr && static_cast<LiteralExpr*>(v->init)->literalKind == TokenKind::String &&
                 st->kind == TyKind::Pointer)
          init = stringConstant(static_cast<LiteralExpr*>(v->init));
        else
          unsupported(at, "a global with a non-constant initializer");
      }
      decls += name + " = " + (t->isConst ? "constant " : "global ") + llType(types.stripRef(t), at) + " " + init + "\n";
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

    // Binary operation on two converted operands of arithmetic/pointer type.
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

    // Value of a call. If wantAddress and the function returns a reference, the pointer is returned.
    Value callValue(CallExpr* c, bool wantAddress)
    {
      FunctionDecl* f = nullptr;
      std::string thisArg;
      if (c->callee->kind == NodeKind::NameExpr)
      {
        auto* n = static_cast<NameExpr*>(c->callee);
        if (n->resolved && n->resolved->kind == NodeKind::FunctionDecl)
        {
          f = static_cast<FunctionDecl*>(n->resolved);
          if (global.owningClass(f) && !(f->flags & DF_Static))
          {
            auto thisIt = locals.find(nullptr);
            if (thisIt == locals.end())
              unsupported(c, "a method call outside a method");
            thisArg = newTemp();
            fn += "  " + thisArg + " = load ptr, ptr " + thisIt->second.addr + "\n";
          }
        }
        else if (n->resolved && (n->resolved->kind == NodeKind::ClassDecl || n->resolved->kind == NodeKind::EnumDecl ||
                                 n->resolved->kind == NodeKind::UsingAliasDecl) || !n->resolved)
        {
          // Functional cast `T(x)` / `uint32_t(x)`.
          if (c->args.size() == 1 && c->ty && c->ty->isKnown() && c->ty->kind != TyKind::Class)
            return convert(rvalue(c->args[0]), c->ty, c);
          if (c->args.empty() && c->ty && c->ty->isKnown() && c->ty->kind != TyKind::Class)
            return {"0", c->ty};
          unsupported(c, "constructing a class value in an expression");
        }
      }
      else if (c->callee->kind == NodeKind::MemberExpr)
      {
        auto* m = static_cast<MemberExpr*>(c->callee);
        if (m->resolved && m->resolved->kind == NodeKind::FunctionDecl)
        {
          f = static_cast<FunctionDecl*>(m->resolved);
          if (!(f->flags & DF_Static))
          {
            if (m->op == P::Arrow)
            {
              Value p = rvalue(m->base);
              thisArg = p.text;
            }
            else
              thisArg = lvalue(m->base);
          }
        }
      }
      if (!f)
      {
        // Call through a function pointer.
        Value callee = rvalue(c->callee);
        const Ty* ft = callee.ty->kind == TyKind::Pointer ? callee.ty->inner : callee.ty;
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
          fn += "  call " + sig + " " + callee.text + "(" + args + ")\n";
          return {"", types.voidType()};
        }
        std::string r = newTemp();
        fn += "  " + r + " = call " + sig + " " + callee.text + "(" + args + ")\n";
        return {r, types.stripRef(ft->inner)};
      }
      ensureDeclared(f);
      const Ty* ft = global.typeOfDecl(f);
      std::string args;
      if (!thisArg.empty())
        args += "ptr " + thisArg;
      for (size_t i = 0; i < c->args.size(); ++i)
      {
        Expr* argExpr = c->args[i];
        const Ty* pt = i < ft->args.size() ? ft->args[i] : nullptr;
        Value a;
        if (pt && pt->isReference())
          a = {lvalue(argExpr), pt};
        else
        {
          a = rvalue(argExpr);
          if (pt)
            a = convert(a, pt, c);
          else if (a.ty->kind == TyKind::Float && a.ty->bits == 32)
            a = convert(a, types.floatType(64), c); // default argument promotion for variadics
          else if ((a.ty->kind == TyKind::Int || a.ty->kind == TyKind::Char || a.ty->kind == TyKind::Bool) && (a.ty->kind == TyKind::Bool || a.ty->bits < 32))
            a = convert(a, types.intType(32), c);
        }
        if (!args.empty())
          args += ", ";
        args += llType(pt ? pt : a.ty, c) + " " + a.text;
      }
      std::string sig;
      if (ft->variadic)
      {
        sig = llType(ft->inner, c) + " (";
        bool first = true;
        if (!thisArg.empty())
        {
          sig += "ptr";
          first = false;
        }
        for (const Ty* p : ft->args)
        {
          sig += std::string(first ? "" : ", ") + llType(p, c);
          first = false;
        }
        sig += std::string(first ? "..." : ", ...") + ") ";
      }
      else
        sig = llType(ft->inner, c) + " ";
      if (ft->inner->kind == TyKind::Void)
      {
        fn += "  call " + sig + functionName(f) + "(" + args + ")\n";
        return {"", types.voidType()};
      }
      std::string r = newTemp();
      fn += "  " + r + " = call " + sig + functionName(f) + "(" + args + ")\n";
      if (ft->inner->isReference())
      {
        if (wantAddress)
          return {r, ft->inner};
        return load(r, ft->inner->inner, c);
      }
      return {r, ft->inner};
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
            std::string s(spelling(l->token));
            std::string digits;
            for (char c : s)
              if (c != '\'' && c != 'f' && c != 'F' && c != 'l' && c != 'L')
                digits += c;
            double d = strtod(digits.c_str(), nullptr);
            char buf[64];
            if (t->bits == 32)
            {
              // Single-precision constants are written as doubles that are exactly representable.
              float f = float(d);
              snprintf(buf, sizeof buf, "%.9e", double(f));
            }
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
            ensureDeclared(static_cast<FunctionDecl*>(d));
            return {functionName(static_cast<FunctionDecl*>(d)), types.pointer(global.typeOfDecl(d))};
          }
          if (d && d->kind == NodeKind::VariableDecl)
          {
            auto c = evalConst(e);
            const Ty* t = types.stripRef(global.typeOfDecl(d));
            if (c && t && t->isConst && (t->kind == TyKind::Int || t->kind == TyKind::Enum || t->kind == TyKind::Char))
              return {std::to_string(*c), types.unqualified(t)};
          }
          std::string addr = lvalue(e);
          return load(addr, e->ty ? e->ty : (d ? global.typeOfDecl(d) : nullptr), e);
        }
        case NodeKind::ThisExpr:
        {
          auto thisIt = locals.find(nullptr);
          if (thisIt == locals.end())
            unsupported(e, "this outside a method");
          std::string r = newTemp();
          fn += "  " + r + " = load ptr, ptr " + thisIt->second.addr + "\n";
          return {r, e->ty};
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
          std::string addr = lvalue(e);
          return load(addr, e->ty, e);
        }
        case NodeKind::CallExpr: return callValue(static_cast<CallExpr*>(e), false);
        case NodeKind::CastExpr:
          return convert(rvalue(static_cast<CastExpr*>(e)->operand), e->ty, e);
        case NodeKind::NamedCastExpr:
          return convert(rvalue(static_cast<NamedCastExpr*>(e)->operand), e->ty, e);
        case NodeKind::FunctionalCastExpr:
        {
          auto* c = static_cast<FunctionalCastExpr*>(e);
          if (c->args.size() == 1)
          {
            Expr* a = c->args[0];
            if (a->kind == NodeKind::BraceInitExpr && static_cast<BraceInitExpr*>(a)->args.size() == 1)
              a = static_cast<BraceInitExpr*>(a)->args[0];
            if (e->ty && e->ty->kind != TyKind::Class)
              return convert(rvalue(a), e->ty, e);
          }
          if (c->args.empty() && e->ty && e->ty->kind != TyKind::Class)
            return {e->ty->kind == TyKind::Float ? "0.0" : e->ty->kind == TyKind::Pointer ? "null" : "0", e->ty};
          unsupported(e, "constructing a class value in an expression");
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
          Value cond = convert(rvalue(c->cond), types.boolType(), e);
          std::string tL = newLabel("cond.then"), eL = newLabel("cond.else"), jL = newLabel("cond.end");
          fn += "  br i1 " + cond.text + ", label %" + tL + ", label %" + eL + "\n";
          startBlock(tL);
          Value a = convert(rvalue(c->then), e->ty, e);
          std::string aEnd = currentBlockName();
          emitBranch(jL);
          startBlock(eL);
          Value b = convert(rvalue(c->else_), e->ty, e);
          std::string bEnd = currentBlockName();
          emitBranch(jL);
          startBlock(jL);
          if (e->ty->kind == TyKind::Void)
            return {"", e->ty};
          std::string r = newTemp();
          fn += "  " + r + " = phi " + llType(e->ty, e) + " [ " + a.text + ", %" + aEnd + " ], [ " + b.text + ", %" + bEnd + " ]\n";
          return {r, e->ty};
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
          if (!n->args.empty())
          {
            if (n->args.size() == 1 && t->kind != TyKind::Class)
              store(rvalue(n->args[0]), r, t, e);
            else
              unsupported(e, "new with constructor arguments");
          }
          else
            fn += "  store " + llType(t, e) + " zeroinitializer, ptr " + r + "\n";
          return {r, e->ty};
        }
        case NodeKind::DeleteExpr:
        {
          Value p = rvalue(static_cast<DeleteExpr*>(e)->operand);
          ensureMalloc();
          fn += "  call void @free(ptr " + p.text + ")\n";
          return {"", types.voidType()};
        }
        default:
          unsupported(e, std::string("the expression kind ") + nodeKindName(e->kind));
      }
    }

    bool mallocDeclared = false;
    void ensureMalloc()
    {
      if (mallocDeclared)
        return;
      mallocDeclared = true;
      decls += "declare ptr @malloc(i64)\ndeclare void @free(ptr)\n";
    }

    // Name of the block currently being emitted (for phi nodes).
    std::string currentBlock = "entry";
    std::string currentBlockName() { return currentBlock; }

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
      switch (op)
      {
        case P::Amp:
        {
          std::string addr = lvalue(u->operand);
          return {addr, u->ty};
        }
        case P::Star:
        {
          std::string addr = lvalue(u);
          return load(addr, u->ty, u);
        }
        case P::Bang:
        {
          Value v = convert(rvalue(u->operand), types.boolType(), u);
          std::string r = newTemp();
          fn += "  " + r + " = xor i1 " + v.text + ", true\n";
          return {r, types.boolType()};
        }
        case P::Minus:
        {
          Value v = rvalue(u->operand);
          v = convert(v, u->ty, u);
          std::string r = newTemp();
          if (v.ty->kind == TyKind::Float)
            fn += "  " + r + " = fneg " + llType(v.ty, u) + " " + v.text + "\n";
          else
            fn += "  " + r + " = sub " + llType(v.ty, u) + " 0, " + v.text + "\n";
          return {r, v.ty};
        }
        case P::Plus: return convert(rvalue(u->operand), u->ty, u);
        case P::Tilde:
        {
          Value v = convert(rvalue(u->operand), u->ty, u);
          std::string r = newTemp();
          fn += "  " + r + " = xor " + llType(v.ty, u) + " " + v.text + ", -1\n";
          return {r, v.ty};
        }
        case P::PlusPlus:
        case P::MinusMinus:
        {
          std::string addr = lvalue(u->operand);
          const Ty* t = types.stripRef(u->operand->ty);
          Value old = load(addr, t, u);
          Value one{"1", types.intType(32)};
          Value updated = arithmetic(op == P::PlusPlus ? P::Plus : P::Minus, old, one, u);
          store(updated, addr, t, u);
          return u->postfix ? old : convert(updated, t, u);
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
        else if (t->kind == TyKind::Class)
        {
          if (rhs.ty->kind != TyKind::Class)
            unsupported(b, "assigning a non-class to a class");
          fn += "  store " + llType(t, b) + " " + rhs.text + ", ptr " + addr + "\n";
          return rhs;
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
      std::string lEnd = currentBlockName();
      if (isAnd)
        fn += "  br i1 " + l.text + ", label %" + rL + ", label %" + eL + "\n";
      else
        fn += "  br i1 " + l.text + ", label %" + eL + ", label %" + rL + "\n";
      startBlock(rL);
      Value r = convert(rvalue(rhs), types.boolType(), at);
      std::string rEnd = currentBlockName();
      emitBranch(eL);
      startBlock(eL);
      std::string res = newTemp();
      fn += "  " + res + " = phi i1 [ " + std::string(isAnd ? "false" : "true") + ", %" + lEnd + " ], [ " + r.text + ", %" + rEnd + " ]\n";
      return {res, types.boolType()};
    }

    // --- statements ---

    void declareLocal(VariableDecl* v)
    {
      const Ty* t = global.typeOfDecl(v);
      if (!t || !t->isKnown())
        unsupported(v, "a local of unknown type (" + sym(v->name.last().symbol) + ")");
      bool isRef = t->isReference();
      std::string slot = "%v" + std::to_string(temp++);
      allocas += "  " + slot + " = alloca " + (isRef ? std::string("ptr") : llType(t, v)) + "\n";
      locals[v] = {slot, t, isRef};
      if (isRef)
      {
        if (!v->init)
          unsupported(v, "a reference without an initializer");
        std::string addr = lvalue(v->init);
        fn += "  store ptr " + addr + ", ptr " + slot + "\n";
        return;
      }
      if (v->init)
      {
        Expr* init = v->init;
        if (init->kind == NodeKind::BraceInitExpr)
        {
          auto* bi = static_cast<BraceInitExpr*>(init);
          if (bi->args.empty())
          {
            fn += "  store " + llType(t, v) + " zeroinitializer, ptr " + slot + "\n";
            return;
          }
          if (bi->args.size() == 1 && t->kind != TyKind::Class)
            init = bi->args[0];
          else if (t->kind == TyKind::Class)
          {
            // Aggregate initialisation in field order.
            StructInfo& info = structFor(t->decl, v);
            fn += "  store " + llType(t, v) + " zeroinitializer, ptr " + slot + "\n";
            size_t i = 0;
            for (Decl* m : static_cast<ClassDecl*>(global.definitionOf(t->decl))->members)
            {
              auto it = info.fieldIndex.find(m);
              if (it == info.fieldIndex.end())
                continue;
              if (i >= bi->args.size())
                break;
              std::string addr = newTemp();
              fn += "  " + addr + " = getelementptr inbounds " + info.name + ", ptr " + slot + ", i32 0, i32 " + std::to_string(it->second) + "\n";
              store(rvalue(bi->args[i]), addr, info.fieldTypes[it->second], v);
              ++i;
            }
            return;
          }
          else
            unsupported(v, "this brace initializer");
        }
        if (init->kind == NodeKind::FunctionalCastExpr && t->kind == TyKind::Class)
        {
          auto* fc = static_cast<FunctionalCastExpr*>(init);
          if (fc->args.empty())
          {
            fn += "  store " + llType(t, v) + " zeroinitializer, ptr " + slot + "\n";
            return;
          }
          unsupported(v, "constructor arguments for a class local");
        }
        Value val = rvalue(init);
        if (t->kind == TyKind::Class)
        {
          if (val.ty->kind != TyKind::Class)
            unsupported(v, "initialising a class from a non-class value");
          fn += "  store " + llType(t, v) + " " + val.text + ", ptr " + slot + "\n";
        }
        else
          store(val, slot, t, v);
      }
      else
        fn += "  store " + llType(t, v) + " zeroinitializer, ptr " + slot + "\n";
    }

    void statement(Stmt* s)
    {
      if (!s || blockTerminated && s->kind != NodeKind::LabelStmt)
      {
        if (s && blockTerminated)
        {
          // Unreachable code after return/break: start a fresh block so the IR stays valid.
          std::string dead = newLabel("dead");
          startBlock(dead);
          currentBlock = dead;
        }
        else
          return;
      }
      switch (s->kind)
      {
        case NodeKind::BlockStmt:
          for (Stmt* inner : static_cast<BlockStmt*>(s)->stmts)
            statement(inner);
          break;
        case NodeKind::ExprStmt:
          if (static_cast<ExprStmt*>(s)->expr)
            rvalue(static_cast<ExprStmt*>(s)->expr);
          break;
        case NodeKind::DeclStmt:
          for (Decl* d : static_cast<DeclStmt*>(s)->decls)
          {
            if (d->kind == NodeKind::VariableDecl)
              declareLocal(static_cast<VariableDecl*>(d));
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
          if (currentReturn->kind == TyKind::Void)
          {
            if (r->value)
              rvalue(r->value);
            fn += "  ret void\n";
          }
          else if (currentReturn->isReference())
          {
            std::string addr = lvalue(r->value);
            fn += "  ret ptr " + addr + "\n";
          }
          else
          {
            if (!r->value)
              unsupported(s, "return without a value in a non-void function");
            Value v = rvalue(r->value);
            if (currentReturn->kind == TyKind::Class)
              fn += "  ret " + llType(currentReturn, s) + " " + v.text + "\n";
            else
            {
              v = convert(v, currentReturn, s);
              fn += "  ret " + llType(currentReturn, s) + " " + v.text + "\n";
            }
          }
          blockTerminated = true;
          break;
        }
        case NodeKind::IfStmt:
        {
          auto* i = static_cast<IfStmt*>(s);
          if (i->init)
            statement(i->init);
          Value cond = condition(i->cond, s);
          std::string tL = newLabel("if.then"), eL = newLabel("if.else"), jL = newLabel("if.end");
          fn += "  br i1 " + cond.text + ", label %" + tL + ", label %" + (i->else_ ? eL : jL) + "\n";
          startBlock(tL);
          currentBlock = tL;
          statement(i->then);
          emitBranch(jL);
          if (i->else_)
          {
            startBlock(eL);
            currentBlock = eL;
            statement(i->else_);
            emitBranch(jL);
          }
          startBlock(jL);
          currentBlock = jL;
          break;
        }
        case NodeKind::WhileStmt:
        {
          auto* w = static_cast<WhileStmt*>(s);
          std::string cL = newLabel("while.cond"), bL = newLabel("while.body"), eL = newLabel("while.end");
          emitBranch(cL);
          startBlock(cL);
          currentBlock = cL;
          Value cond = condition(w->cond, s);
          fn += "  br i1 " + cond.text + ", label %" + bL + ", label %" + eL + "\n";
          startBlock(bL);
          currentBlock = bL;
          loops.push_back({eL, cL});
          statement(w->body);
          loops.pop_back();
          emitBranch(cL);
          startBlock(eL);
          currentBlock = eL;
          break;
        }
        case NodeKind::DoStmt:
        {
          auto* d = static_cast<DoStmt*>(s);
          std::string bL = newLabel("do.body"), cL = newLabel("do.cond"), eL = newLabel("do.end");
          emitBranch(bL);
          startBlock(bL);
          currentBlock = bL;
          loops.push_back({eL, cL});
          statement(d->body);
          loops.pop_back();
          emitBranch(cL);
          startBlock(cL);
          currentBlock = cL;
          Value cond = convert(rvalue(d->cond), types.boolType(), s);
          fn += "  br i1 " + cond.text + ", label %" + bL + ", label %" + eL + "\n";
          startBlock(eL);
          currentBlock = eL;
          break;
        }
        case NodeKind::ForStmt:
        {
          auto* f = static_cast<ForStmt*>(s);
          if (f->init)
            statement(f->init);
          std::string cL = newLabel("for.cond"), bL = newLabel("for.body"), sL = newLabel("for.step"), eL = newLabel("for.end");
          emitBranch(cL);
          startBlock(cL);
          currentBlock = cL;
          if (f->cond)
          {
            Value cond = condition(f->cond, s);
            fn += "  br i1 " + cond.text + ", label %" + bL + ", label %" + eL + "\n";
          }
          else
            fn += "  br label %" + bL + "\n";
          startBlock(bL);
          currentBlock = bL;
          loops.push_back({eL, sL});
          statement(f->body);
          loops.pop_back();
          emitBranch(sL);
          startBlock(sL);
          currentBlock = sL;
          if (f->step)
            rvalue(f->step);
          emitBranch(cL);
          startBlock(eL);
          currentBlock = eL;
          break;
        }
        case NodeKind::BreakStmt:
          if (loops.empty())
            unsupported(s, "break outside a loop or switch");
          emitBranch(loops.back().breakLabel);
          break;
        case NodeKind::ContinueStmt:
        {
          for (size_t i = loops.size(); i-- > 0;)
            if (!loops[i].continueLabel.empty())
            {
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
        Value val = load(l.addr, l.ty, at);
        return convert(val, types.boolType(), at);
      }
      return convert(rvalue(static_cast<Expr*>(cond)), types.boolType(), at);
    }

    // C++ switch semantics: labels fall through unless the case ends with break.
    void switchStatement(SwitchStmt* sw)
    {
      if (sw->init)
        statement(sw->init);
      if (!sw->cond || sw->cond->kind == NodeKind::DeclStmt)
        unsupported(sw, "a switch on a declaration");
      Value cond = rvalue(static_cast<Expr*>(sw->cond));
      const Ty* ct = cond.ty->kind == TyKind::Enum ? enumUnderlying(cond.ty->decl) : cond.ty;
      if (ct->kind != TyKind::Int && ct->kind != TyKind::Char && ct->kind != TyKind::Bool)
        unsupported(sw, "a switch on a non-integer");
      cond = convert(cond, ct, sw);
      if (!sw->body || sw->body->kind != NodeKind::BlockStmt)
        unsupported(sw, "a switch whose body is not a block");
      auto* body = static_cast<BlockStmt*>(sw->body);
      // First pass: one label per case, the default label, and the switch instruction.
      std::string endL = newLabel("switch.end");
      std::string defaultL;
      std::vector<std::pair<Stmt*, std::string>> labels;
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
          labels.push_back({st, l});
          cases += "    " + llType(ct, sw) + " " + std::to_string(*v) + ", label %" + l + "\n";
        }
        else if (st->kind == NodeKind::DefaultStmt)
        {
          defaultL = newLabel("default");
          labels.push_back({st, defaultL});
        }
      }
      fn += "  switch " + llType(ct, sw) + " " + cond.text + ", label %" + (defaultL.empty() ? endL : defaultL) + " [\n" + cases + "  ]\n";
      blockTerminated = true;
      loops.push_back({endL, ""});
      size_t next = 0;
      for (Stmt* st : body->stmts)
      {
        if (st->kind == NodeKind::CaseStmt || st->kind == NodeKind::DefaultStmt)
        {
          const std::string& l = labels[next++].second;
          emitBranch(l); // fallthrough from the previous case
          startBlock(l);
          currentBlock = l;
          continue;
        }
        if (st->kind == NodeKind::FallthroughStmt)
          continue;
        statement(st);
      }
      loops.pop_back();
      emitBranch(endL);
      startBlock(endL);
      currentBlock = endL;
    }

    // --- functions ---

    void emitFunction(FunctionDecl* f)
    {
      const Ty* ft = global.typeOfDecl(f);
      if (!ft || ft->kind != TyKind::Function)
      {
        error(f, "function type unknown, skipped");
        return;
      }
      fn.clear();
      allocas.clear();
      locals.clear();
      loops.clear();
      temp = 0;
      label = 0;
      blockTerminated = false;
      currentBlock = "entry";
      currentFunction = f;
      currentReturn = ft->inner;
      std::vector<std::pair<std::string, const Ty*>> params;
      std::string sig = signature(f, true, &params);
      size_t space = sig.find(' ');
      std::string header = "define " + sig.substr(0, space) + " " + functionName(f) + sig.substr(space + 1) + " {\nentry:\n";
      // Parameters live in allocas so that they can be assigned and addressed.
      for (size_t i = 0; i < params.size(); ++i)
      {
        const Ty* pt = params[i].second;
        std::string slot = "%v" + std::to_string(temp++);
        if (!pt)
        {
          allocas += "  " + slot + " = alloca ptr\n";
          fn += "  store ptr %this, ptr " + slot + "\n";
          locals[nullptr] = {slot, nullptr, false};
          continue;
        }
        bool isRef = pt->isReference();
        allocas += "  " + slot + " = alloca " + (isRef ? std::string("ptr") : llType(pt, f)) + "\n";
        fn += "  store " + (isRef ? std::string("ptr") : llType(pt, f)) + " " + params[i].first + ", ptr " + slot + "\n";
        size_t paramIndex = i - (params[0].second == nullptr ? 1 : 0);
        locals[f->params[paramIndex]] = {slot, pt, isRef};
      }
      try
      {
        statement(f->body);
      }
      catch (Unsupported&)
      {
        return; // Error recorded; the function is left out of the module.
      }
      if (!blockTerminated)
      {
        if (currentReturn->kind == TyKind::Void)
          fn += "  ret void\n";
        else if (functionName(f) == "@main")
          fn += "  ret i32 0\n";
        else
          fn += "  unreachable\n";
      }
      body += header + allocas + fn + "}\n\n";
      ++result.functions;
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
  {
    e.currentFile = f->file;
    try
    {
      e.emitFunction(f);
    }
    catch (Emitter::Unsupported&)
    {
    }
  }
  result.ir = "; Kov v0: generated by kovc\n"
              "target datalayout = \"e-m:w-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128\"\n"
              "target triple = \"x86_64-pc-windows-msvc\"\n\n" +
              e.decls + "\n" + e.body;
  return result;
}
