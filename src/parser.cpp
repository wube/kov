// Recursive-descent parser for the Kov subset, tolerant of C++ so that real C++ code bases can
// be used as benchmark input. Where C++ needs semantic knowledge to disambiguate (declaration vs.
// expression, template-id vs. less-than), this parser uses purely syntactic heuristics; the
// resolver later checks the guesses. Unknown macros are recognised by their ALL_CAPS spelling.
#include "parser.hpp"
#include <cstring>

namespace
{
  struct Declarator
  {
    Type* type = nullptr;
    QualifiedName name;
    bool isFunction = false;
    Span<Decl*> params;
    uint32_t flags = 0;
    Type* trailingReturn = nullptr;
    Expr* bitfield = nullptr;
    Expr* requiresClause = nullptr;
    Span<Expr*> structuredBindings;
  };

  class Parser
  {
  public:
    Parser(std::vector<Token>& tokens, std::string_view text, SymbolTable& symbols, ParsedFile& out)
      : toks(tokens), text(text.data()), symbols(symbols), out(out), arena(out.arena)
    {
      kwImplicit = symbols.intern("implicit");
      kwFallthrough = symbols.intern("fallthrough");
      kwBothConst = symbols.intern("both_const");
      kwClassConst = symbols.intern("class_const");
      kwAccumulate = symbols.intern("accumulate");
    }
    uint32_t kwImplicit, kwFallthrough, kwBothConst, kwClassConst, kwAccumulate;

    void parseTranslationUnit()
    {
      size_t mark = scratch.size();
      while (!at(TokenKind::End))
      {
        if (at(P::RBrace))
        {
          error("unexpected '}'");
          ++pos;
          continue;
        }
        parseDeclaration(Scope::Namespace);
      }
      out.decls = makeSpan<Decl*>(mark);
    }

  private:
    enum class Scope { Namespace, Class, Enum, Local };

    std::vector<Token>& toks;
    const char* text;
    SymbolTable& symbols;
    ParsedFile& out;
    Arena& arena;
    uint32_t pos = 0;
    std::vector<Node*> scratch;
    Decl* currentParent = nullptr;
    uint32_t currentClassName = UINT32_MAX;
    Access currentAccess = Access::Default;
    bool inRecovery = false;
    int depth = 0;
    bool conditionMode = false; // Declarations inside `if (...)` end at ')' instead of ';'.
    bool inRequires = false;    // Inside a requires-clause names are always template-ids.
    std::vector<std::pair<uint32_t, Token>> splits; // '>>' tokens split in place, for undo on backtrack.

    // Backtracks to a saved position, undoing any token splits made after it.
    void restore(uint32_t save)
    {
      pos = save;
      while (!splits.empty() && splits.back().first >= save)
      {
        uint32_t at = splits.back().first;
        toks[at] = splits.back().second;
        toks.erase(toks.begin() + at + 1);
        splits.pop_back();
      }
    }

    // --- token access ---

    const Token& tok(int ahead = 0) const
    {
      size_t i = pos + size_t(ahead);
      return i < toks.size() ? toks[i] : toks.back();
    }
    bool at(TokenKind kind, int ahead = 0) const { return tok(ahead).getKind() == kind; }
    bool at(P p, int ahead = 0) const { return tok(ahead).is(p); }
    bool at(Kw k, int ahead = 0) const { return tok(ahead).is(k); }
    bool atIdent(int ahead = 0) const { return at(TokenKind::Identifier, ahead); }
    bool atIdent(uint32_t symbol, int ahead = 0) const { return atIdent(ahead) && tok(ahead).id == symbol; }
    std::string_view spelling(const Token& t) const { return std::string_view(text + t.offset, t.length); }
    std::string_view spelling(int ahead = 0) const { return spelling(tok(ahead)); }
    void advance()
    {
      if (pos < toks.size() - 1)
        ++pos;
    }
    bool accept(P p)
    {
      if (!at(p))
        return false;
      advance();
      return true;
    }
    bool accept(Kw k)
    {
      if (!at(k))
        return false;
      advance();
      return true;
    }
    bool expect(P p, const char* what)
    {
      if (accept(p))
        return true;
      error(what);
      return false;
    }
    // Closing '>' of a template argument list, splitting '>>', '>=' and '>>=' when needed.
    bool acceptTemplateClose()
    {
      if (accept(P::Greater))
        return true;
      Token t = toks[pos];
      if (t.is(P::Shr) || t.is(P::GreaterEq) || t.is(P::ShrAssign))
      {
        // Split into '>' and the rest. Inserting keeps earlier token indices valid, so speculative
        // parses that backtrack over this point re-parse the same token sequence.
        Token rest = t;
        rest.offset += 1;
        rest.length -= 1;
        std::string_view restSpelling = spelling(rest);
        rest.id = uint32_t(restSpelling == ">" ? P::Greater : restSpelling == "=" ? P::Assign : P::GreaterEq);
        splits.push_back({pos, t});
        toks[pos].length = 1;
        toks[pos].id = uint32_t(P::Greater);
        toks.insert(toks.begin() + pos + 1, rest);
        advance();
        return true;
      }
      return false;
    }

    bool isAllCaps(std::string_view s) const
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
    bool atMacroName(int ahead = 0) const { return atIdent(ahead) && isAllCaps(spelling(ahead)); }

    static bool isDeclStartKeyword(Kw k)
    {
      switch (k)
      {
        case Kw::const_: case Kw::volatile_: case Kw::static_: case Kw::virtual_: case Kw::inline_:
        case Kw::constexpr_: case Kw::consteval_: case Kw::constinit_: case Kw::explicit_: case Kw::friend_:
        case Kw::extern_: case Kw::mutable_: case Kw::typedef_: case Kw::class_: case Kw::struct_:
        case Kw::union_: case Kw::enum_: case Kw::auto_: case Kw::typename_: case Kw::template_:
        case Kw::using_: case Kw::operator_: case Kw::namespace_: case Kw::thread_local_: case Kw::decltype_:
        case Kw::void_: case Kw::bool_: case Kw::char_: case Kw::char8_t_: case Kw::char16_t_: case Kw::char32_t_:
        case Kw::wchar_t_: case Kw::short_: case Kw::int_: case Kw::long_: case Kw::signed_: case Kw::unsigned_:
        case Kw::float_: case Kw::double_:
          return true;
        default:
          return false;
      }
    }

    // --- diagnostics and recovery ---

    void error(const char* message)
    {
      if (inRecovery)
        return;
      inRecovery = true;
      out.diagnostics.push_back({pos, message});
    }

    // Skips a balanced bracket group starting at the current opening token.
    void skipBalanced()
    {
      P open = P(tok().id);
      P close = open == P::LParen ? P::RParen : open == P::LBracket ? P::RBracket : open == P::LBrace ? P::RBrace : P::Greater;
      int level = 0;
      for (;;)
      {
        if (at(TokenKind::End))
          return;
        if (at(open))
          ++level;
        else if (at(close))
        {
          --level;
          if (level == 0)
          {
            advance();
            return;
          }
        }
        advance();
      }
    }

    // Skips to the end of the current declaration or statement: past the next ';' at nesting
    // level 0, past a balanced '{...}', or up to (not past) a closing '}'.
    void recover()
    {
      int level = 0;
      for (;;)
      {
        if (at(TokenKind::End))
          break;
        if (at(P::LParen) || at(P::LBracket))
          ++level;
        else if (at(P::RParen) || at(P::RBracket))
        {
          if (level > 0)
            --level;
        }
        else if (level == 0 && at(P::Semi))
        {
          advance();
          break;
        }
        else if (level == 0 && at(P::LBrace))
        {
          skipBalanced();
          accept(P::Semi);
          break;
        }
        else if (level == 0 && at(P::RBrace))
          break;
        advance();
      }
      inRecovery = false;
    }

    // --- arena helpers ---

    template<typename T>
    T* node(uint32_t token)
    {
      T* n = arena.make<T>();
      n->kind = nodeKindOf<T>();
      n->token = token;
      ++out.nodeCount;
      return n;
    }
    template<typename T>
    T* decl(uint32_t token)
    {
      T* d = node<T>(token);
      d->parent = currentParent;
      d->access = currentAccess;
      ++out.declCount;
      return d;
    }
    template<typename T>
    static constexpr NodeKind nodeKindOf();

    template<typename T>
    Span<T> makeSpan(size_t mark)
    {
      Span<T> span;
      span.count = uint32_t(scratch.size() - mark);
      span.data = arena.array(reinterpret_cast<T*>(scratch.data() + mark), span.count);
      scratch.resize(mark);
      return span;
    }
    template<typename T>
    Span<T> makeSpanOf(const std::vector<T>& items)
    {
      Span<T> span;
      span.count = uint32_t(items.size());
      span.data = arena.array(items.data(), items.size());
      return span;
    }

    struct DepthGuard
    {
      Parser& p;
      DepthGuard(Parser& p) : p(p) { ++p.depth; }
      ~DepthGuard() { --p.depth; }
    };

    // --- attributes and other things we skip ---

    void skipAttributes()
    {
      for (;;)
      {
        if (at(P::LBracket) && at(P::LBracket, 1))
        {
          advance();
          advance();
          int level = 2;
          while (level > 0 && !at(TokenKind::End))
          {
            if (at(P::LBracket))
              ++level;
            else if (at(P::RBracket))
              --level;
            advance();
          }
        }
        else if (at(Kw::alignas_) && at(P::LParen, 1))
        {
          advance();
          skipBalanced();
        }
        else if (atIdent() && (spelling() == "__declspec" || spelling() == "__attribute__" || spelling() == "__pragma" ||
                               spelling() == "alignas") && at(P::LParen, 1))
        {
          advance();
          skipBalanced();
        }
        else if (atIdent() && (spelling() == "__forceinline" || spelling() == "__inline" || spelling() == "__cdecl" ||
                               spelling() == "__stdcall" || spelling() == "__fastcall" || spelling() == "__vectorcall" ||
                               spelling() == "__restrict" || spelling() == "__unaligned" || spelling() == "__thiscall"))
          advance();
        else if (at(TokenKind::Directive))
          advance();
        else
          return;
      }
    }

    // ALL_CAPS macro used as a specifier before a declaration, e.g. `FORCE_INLINE int f()` or
    // `KOV_API(dll) class X`. Returns true if something was skipped.
    bool skipMacroSpecifier()
    {
      if (!atMacroName())
        return false;
      // MACRO(...) followed by something that starts a declaration.
      if (at(P::LParen, 1))
      {
        uint32_t save = pos;
        advance();
        skipBalanced();
        if (atIdent() || (at(TokenKind::Keyword) && isDeclStartKeyword(Kw(tok().id))) || at(P::Tilde) || at(P::ColonColon))
          return true;
        restore(save);
        return false;
      }
      // MACRO followed by a keyword, or by an identifier that is itself followed by a type-like
      // continuation (`MACRO Foo x`, `MACRO Foo* x`). `ETA getETA(` and `ID id;` are types, not macros.
      if (at(TokenKind::Keyword, 1) && isDeclStartKeyword(Kw(tok(1).id)))
      {
        advance();
        return true;
      }
      if (atIdent(1) && (atIdent(2) || at(P::Star, 2) || at(P::Amp, 2) || at(P::AndAnd, 2) || at(P::Less, 2)))
      {
        advance();
        return true;
      }
      return false;
    }

    // --- names ---

    // Parses `a::b<args>::c`. In type context template arguments are always attempted; in
    // expression context they are only accepted when the token after '>' makes a template-id
    // plausible.
    bool parseQualifiedName(QualifiedName& name, bool typeContext, bool allowSpecial = true)
    {
      std::vector<NamePart> parts;
      name.global = accept(P::ColonColon);
      for (;;)
      {
        skipAttributes();
        NamePart part;
        if (accept(Kw::template_))
        {
        }
        if (atIdent())
        {
          part.symbol = tok().id;
          advance();
        }
        else if (allowSpecial && at(P::Tilde) && atIdent(1))
        {
          part.special = 1;
          advance();
          part.symbol = tok().id;
          advance();
        }
        else if (allowSpecial && at(Kw::operator_))
        {
          advance();
          if (!parseOperatorName(part))
            return false;
        }
        else if (at(Kw::decltype_) && at(P::LParen, 1) && !parts.empty())
        {
          // `A::decltype(x)::b` is weird but legal; treat as unresolved component.
          advance();
          skipBalanced();
          part.symbol = UINT32_MAX;
        }
        else
          return false;
        if (part.special == 0 && at(P::Less))
          tryParseTemplateArgs(part, typeContext);
        parts.push_back(part);
        if (at(P::ColonColon) && !at(P::Star, 1) && (atIdent(1) || at(P::Tilde, 1) || at(Kw::operator_, 1) || at(Kw::template_, 1)))
        {
          advance();
          continue;
        }
        break;
      }
      name.parts = makeSpanOf(parts);
      return true;
    }

    bool parseOperatorName(NamePart& part)
    {
      part.special = 2;
      if (at(TokenKind::Punctuator))
      {
        P p = P(tok().id);
        if (p == P::LParen && at(P::RParen, 1))
        {
          part.opToken = uint32_t(P::LParen);
          advance();
          advance();
          return true;
        }
        if (p == P::LBracket && at(P::RBracket, 1))
        {
          part.opToken = uint32_t(P::LBracket);
          advance();
          advance();
          return true;
        }
        part.opToken = tok().id;
        advance();
        return true;
      }
      if (at(Kw::new_) || at(Kw::delete_))
      {
        part.opToken = tok().id;
        advance();
        if (at(P::LBracket) && at(P::RBracket, 1))
        {
          advance();
          advance();
        }
        return true;
      }
      if (at(TokenKind::String))
      {
        // operator"" _suffix
        advance();
        if (atIdent())
          advance();
        part.opToken = 0;
        return true;
      }
      // Conversion operator: operator Type
      part.special = 3;
      part.convType = parseType(false);
      if (!part.convType)
      {
        error("expected operator name");
        return false;
      }
      return true;
    }

    // Template argument heuristics: see parseQualifiedName.
    void tryParseTemplateArgs(NamePart& part, bool typeContext)
    {
      uint32_t save = pos;
      size_t mark = scratch.size();
      bool savedRecovery = inRecovery;
      size_t savedDiagnostics = out.diagnostics.size();
      advance(); // '<'
      bool ok = true;
      bool sawLogical = false;
      if (!at(P::Greater) && !at(P::Shr))
      {
        for (;;)
        {
          Node* arg = parseTemplateArgument(sawLogical);
          if (!arg)
          {
            ok = false;
            break;
          }
          scratch.push_back(arg);
          if (accept(P::Ellipsis))
          {
          }
          if (accept(P::Comma))
            continue;
          break;
        }
      }
      if (ok && !acceptTemplateClose())
        ok = false;
      if (ok && !typeContext)
      {
        // In expression context, `a < b > c` style ambiguities are resolved towards comparison
        // unless the continuation only makes sense for a template-id.
        bool plausible = at(P::LParen) || at(P::ColonColon) || at(P::LBrace) || at(P::RParen) || at(P::Comma) ||
                         at(P::Greater) || at(P::Semi) || at(P::RBracket) || at(P::Dot) || at(P::Arrow) ||
                         at(P::Ellipsis) || at(P::Shr) || at(TokenKind::End);
        bool binaryOnly = at(P::EqEq) || at(P::NotEq) || at(P::AndAnd) || at(P::OrOr) || at(P::Question) ||
                          at(P::Slash) || at(P::Percent) || at(P::Pipe) || at(P::Caret) || at(P::Assign) ||
                          at(P::Less) || at(P::LessEq) || at(P::Shl) || at(P::Spaceship) || at(P::Elvis);
        if (inRequires)
          plausible = true, sawLogical = false;
        if (binaryOnly)
          ok = true;
        else if (!plausible || sawLogical)
          ok = false;
      }
      if (!ok)
      {
        restore(save);
        scratch.resize(mark);
        inRecovery = savedRecovery;
        out.diagnostics.resize(savedDiagnostics);
        return;
      }
      part.hasTemplateArgs = true;
      part.templateArgs = makeSpan<Node*>(mark);
    }

    Node* parseTemplateArgument(bool& sawLogical)
    {
      // Try a type first; if what follows is not a valid continuation, parse an expression.
      uint32_t save = pos;
      size_t mark = scratch.size();
      size_t savedDiagnosticsForArg = out.diagnostics.size();
      if (Type* t = parseType())
      {
        if (at(P::Comma) || at(P::Greater) || at(P::Shr) || at(P::GreaterEq) || at(P::ShrAssign) || at(P::Ellipsis))
          return t;
        if (at(P::LParen))
        {
          // Function type `R(Args...)`.
          Declarator d;
          parseFunctionSuffix(d);
          if (!inRecovery && (at(P::Comma) || at(P::Greater) || at(P::Shr) || at(P::GreaterEq) || at(P::ShrAssign)))
          {
            FunctionType* f = node<FunctionType>(t->token);
            f->result = t;
            f->params = d.params;
            f->isConst = (d.flags & DF_Const) != 0;
            f->isNoexcept = (d.flags & DF_Noexcept) != 0;
            return f;
          }
          inRecovery = false;
          out.diagnostics.resize(savedDiagnosticsForArg);
        }
        // `T::value + 1` or `sizeof(T)` etc.: fall back to expression.
      }
      restore(save);
      scratch.resize(mark);
      // Expressions inside template arguments stop at '>' (no relational operators at top level).
      Expr* e = parseExpressionNoGreater(sawLogical);
      return e;
    }

    // --- types ---

    static bool isBuiltinTypeKeyword(Kw k)
    {
      switch (k)
      {
        case Kw::void_: case Kw::bool_: case Kw::char_: case Kw::char8_t_: case Kw::char16_t_: case Kw::char32_t_:
        case Kw::wchar_t_: case Kw::short_: case Kw::int_: case Kw::long_: case Kw::signed_: case Kw::unsigned_:
        case Kw::float_: case Kw::double_:
          return true;
        default:
          return false;
      }
    }

    bool atBuiltinTypeIdent() const
    {
      if (!atIdent())
        return false;
      std::string_view s = spelling();
      return s == "__int64" || s == "__int32" || s == "__int16" || s == "__int8" || s == "size_t" || s == "__m128" ||
             s == "__m128i" || s == "__m256" || s == "__m256i" || s == "__m128d" || s == "__m256d";
    }

    // Parses a type (specifier + abstract pointer/reference suffixes, but not declarator names
    // or function/array suffixes). Returns null and restores position if no type starts here.
    Type* parseType(bool allowMacroSkip = true)
    {
      DepthGuard guard(*this);
      if (depth > 400)
      {
        error("nesting too deep");
        return nullptr;
      }
      uint32_t start = pos;
      bool isConst = false, isVolatile = false;
      skipAttributes();
      for (;;)
      {
        if (accept(Kw::const_))
          isConst = true;
        else if (accept(Kw::volatile_))
          isVolatile = true;
        else if (atIdent() && spelling() == "__restrict")
          advance();
        else
          break;
      }
      Type* type = nullptr;
      uint32_t typeToken = pos;
      if (at(TokenKind::Keyword) && isBuiltinTypeKeyword(Kw(tok().id)))
      {
        BuiltinType* b = node<BuiltinType>(pos);
        b->keyword = Kw::int_;
        bool any = false;
        while (at(TokenKind::Keyword) && isBuiltinTypeKeyword(Kw(tok().id)))
        {
          Kw k = Kw(tok().id);
          if (k == Kw::long_)
            ++b->longCount;
          else if (k == Kw::unsigned_)
            b->isUnsigned = true;
          else if (k == Kw::signed_)
            b->isSigned = true;
          else
            b->keyword = k;
          any = true;
          advance();
        }
        (void)any;
        type = b;
      }
      else if (atBuiltinTypeIdent())
      {
        NamedType* n = node<NamedType>(pos);
        NamePart part;
        part.symbol = tok().id;
        advance();
        n->name.parts = makeSpanOf(std::vector<NamePart>{part});
        type = n;
      }
      else if (at(Kw::auto_))
      {
        AutoType* a = node<AutoType>(pos);
        advance();
        type = a;
      }
      else if (at(Kw::decltype_) && at(P::LParen, 1))
      {
        advance();
        advance();
        if (at(Kw::auto_) && at(P::RParen, 1))
        {
          advance();
          advance();
          AutoType* a = node<AutoType>(typeToken);
          a->isDecltypeAuto = true;
          type = a;
        }
        else
        {
          DecltypeType* d = node<DecltypeType>(typeToken);
          d->expr = parseExpression();
          if (!expect(P::RParen, "expected ')' after decltype"))
            return nullptr;
          type = d;
          // decltype(x)::member
          if (at(P::ColonColon) && atIdent(1))
          {
            NamedType* n = node<NamedType>(typeToken);
            QualifiedName rest;
            advance();
            if (!parseQualifiedName(rest, true, false))
              return nullptr;
            n->name = rest;
            type = n;
          }
        }
      }
      else
      {
        bool typename_ = accept(Kw::typename_);
        if (at(Kw::class_) || at(Kw::struct_) || at(Kw::union_) || at(Kw::enum_))
        {
          // Elaborated type specifier `struct X`.
          advance();
          accept(Kw::class_); // enum class
          accept(Kw::struct_);
          skipAttributes();
        }
        if (allowMacroSkip)
          skipMacroSpecifier();
        if (!atIdent() && !at(P::ColonColon))
        {
          pos = start;
          return nullptr;
        }
        NamedType* n = node<NamedType>(pos);
        n->typename_ = typename_;
        if (!parseQualifiedName(n->name, true, false))
        {
          pos = start;
          return nullptr;
        }
        type = n;
      }
      // Trailing cv-qualifiers.
      for (;;)
      {
        if (accept(Kw::const_))
          isConst = true;
        else if (accept(Kw::volatile_))
          isVolatile = true;
        else
          break;
      }
      if (isConst || isVolatile)
      {
        ConstType* c = node<ConstType>(typeToken);
        c->inner = type;
        c->isVolatile = isVolatile;
        type = c;
      }
      return parseTypeSuffixes(type);
    }

    // Pointer, reference, member-pointer and pack suffixes with their cv-qualifiers.
    Type* parseTypeSuffixes(Type* type)
    {
      for (;;)
      {
        skipAttributes();
        if (at(P::Star))
        {
          PointerType* p = node<PointerType>(pos);
          advance();
          p->pointee = type;
          type = p;
        }
        else if (at(P::Amp))
        {
          LRefType* r = node<LRefType>(pos);
          advance();
          r->referenced = type;
          type = r;
        }
        else if (at(P::AndAnd))
        {
          RRefType* r = node<RRefType>(pos);
          advance();
          r->referenced = type;
          type = r;
        }
        else if (at(Kw::const_) || at(Kw::volatile_))
        {
          ConstType* c = node<ConstType>(pos);
          c->isVolatile = at(Kw::volatile_);
          advance();
          c->inner = type;
          type = c;
        }
        else if (atIdent() && spelling() == "__restrict")
          advance();
        else if (at(P::ColonColon) && at(P::Star, 1))
        {
          // T C::* : the qualified name was already consumed into `type`; treat as member pointer.
          advance();
          advance();
          MemberPointerType* m = node<MemberPointerType>(pos);
          m->classType = type;
          m->pointee = nullptr;
          type = m;
        }
        else if (at(P::LParen) && at(P::Star, 1) && at(P::RParen, 2) && at(P::LParen, 3))
        {
          // Abstract pointer to function: R (*)(Args)
          uint32_t start = pos;
          advance();
          advance();
          advance();
          Declarator d;
          parseFunctionSuffix(d);
          FunctionType* f = node<FunctionType>(start);
          f->result = type;
          f->params = d.params;
          PointerType* p = node<PointerType>(start);
          p->pointee = f;
          type = p;
        }
        else if (at(P::LParen) && atIdent(1) && at(P::ColonColon, 2) && memberFunctionPointerAhead())
        {
          uint32_t start = pos;
          advance();
          NamedType* cls = node<NamedType>(pos);
          parseQualifiedName(cls->name, true, false);
          advance(); // ::
          advance(); // *
          expect(P::RParen, "expected ')'");
          Declarator d;
          parseFunctionSuffix(d);
          FunctionType* f = node<FunctionType>(start);
          f->result = type;
          f->params = d.params;
          f->isConst = (d.flags & DF_Const) != 0;
          MemberPointerType* m = node<MemberPointerType>(start);
          m->classType = cls;
          m->pointee = f;
          type = m;
        }
        else if (atIdent() && memberPointerAhead())
        {
          // T C::* (abstract) or T C::* name: consume `C::*` here, the name is left to the declarator.
          MemberPointerType* m = node<MemberPointerType>(pos);
          NamedType* cls = node<NamedType>(pos);
          parseQualifiedName(cls->name, true, false);
          advance(); // ::
          advance(); // *
          m->classType = cls;
          m->pointee = type;
          type = m;
        }
        else
          return type;
      }
    }

    // Lookahead for `( Name::Name::* ) (` without allocating.
    bool memberFunctionPointerAhead() const
    {
      int i = 1;
      while (atIdent(i) && at(P::ColonColon, i + 1))
      {
        if (at(P::Star, i + 2))
          return at(P::RParen, i + 3) && at(P::LParen, i + 4);
        i += 2;
      }
      return false;
    }

    // Lookahead for `Name::Name::*` without allocating.
    bool memberPointerAhead() const
    {
      int i = 0;
      while (atIdent(i) && at(P::ColonColon, i + 1))
      {
        if (at(P::Star, i + 2))
          return true;
        i += 2;
      }
      return false;
    }

    // --- declarations ---

    template<typename T>
    void pushDecl(T* d) { scratch.push_back(d); }

    Decl* lastPushedDecl(size_t mark) const
    {
      return scratch.size() > mark ? static_cast<Decl*>(scratch.back()) : nullptr;
    }

    // Parses one declaration (possibly producing several Decl nodes, e.g. `int a, b;`) and pushes
    // the results onto scratch.
    void parseDeclaration(Scope scope)
    {
      DepthGuard guard(*this);
      if (depth > 400)
      {
        error("nesting too deep");
        recover();
        return;
      }
      size_t mark = scratch.size();
      uint32_t start = pos;
      parseDeclarationInner(scope, {});
      if (inRecovery)
      {
        scratch.resize(mark);
        if (pos == start)
          advance();
        recover();
        ErrorDecl* e = decl<ErrorDecl>(start);
        e->endToken = pos;
        pushDecl(e);
      }
    }

    struct TemplateHeader
    {
      Span<Decl*> params;
      bool present = false;
      Expr* requiresClause = nullptr;
    };

    void parseDeclarationInner(Scope scope, TemplateHeader templ)
    {
      skipAttributes();
      uint32_t start = pos;
      if (at(TokenKind::End))
        return;
      if (accept(P::Semi))
      {
        pushDecl(decl<EmptyDecl>(start));
        return;
      }
      if (atMacroName() && !at(P::LParen, 1) && !at(P::ColonColon, 1) && !at(P::Less, 1) &&
          (at(TokenKind::End, 1) || at(P::LBracket, 1) || at(P::RBrace, 1) || at(TokenKind::Directive, 1) ||
           (at(TokenKind::Keyword, 1) && !isDeclStartKeyword(Kw(tok(1).id)))))
      {
        MacroDecl* m = decl<MacroDecl>(start);
        m->name = tok().id;
        advance();
        m->endToken = pos;
        pushDecl(m);
        return;
      }
      if (at(Kw::template_))
      {
        parseTemplate(scope);
        return;
      }
      if (at(Kw::namespace_))
      {
        parseNamespace();
        return;
      }
      if (at(Kw::using_))
      {
        parseUsing(templ);
        return;
      }
      if (at(Kw::static_assert_))
      {
        StaticAssertDecl* s = decl<StaticAssertDecl>(start);
        advance();
        expect(P::LParen, "expected '('");
        s->cond = parseAssignment();
        if (accept(P::Comma))
          parseAssignment();
        expect(P::RParen, "expected ')'");
        expect(P::Semi, "expected ';'");
        pushDecl(s);
        return;
      }
      if (at(Kw::asm_) || (atIdent() && (spelling() == "__asm" || spelling() == "__asm__")))
      {
        advance();
        if (at(P::LParen))
          skipBalanced();
        else if (at(P::LBrace))
          skipBalanced();
        accept(P::Semi);
        pushDecl(decl<AsmDecl>(start));
        return;
      }
      if (at(Kw::extern_) && at(TokenKind::String, 1))
      {
        // extern "C" { ... } or extern "C" decl
        advance();
        advance();
        if (accept(P::LBrace))
        {
          while (!at(P::RBrace) && !at(TokenKind::End))
            parseDeclaration(scope);
          expect(P::RBrace, "expected '}'");
          return;
        }
        parseDeclarationInner(scope, templ);
        return;
      }
      if (at(Kw::public_) || at(Kw::private_) || at(Kw::protected_))
      {
        if (at(P::Colon, 1))
        {
          currentAccess = at(Kw::public_) ? Access::Public : at(Kw::private_) ? Access::Private : Access::Protected;
          advance();
          advance();
          return;
        }
      }
      if (at(Kw::concept_))
      {
        // concept Name = constraint-expr;
        advance();
        VariableDecl* v = decl<VariableDecl>(start);
        v->flags |= DF_Constexpr;
        if (!parseQualifiedName(v->name, false, false))
          error("expected concept name");
        expect(P::Assign, "expected '='");
        v->init = parseAssignment();
        expect(P::Semi, "expected ';'");
        v->templateParams = templ.params;
        v->isTemplate = templ.present;
        pushDecl(v);
        return;
      }

      // Specifiers.
      uint32_t flags = 0;
      parseDeclSpecifiers(flags);
      if (at(Kw::using_))
      {
        parseUsing(templ);
        return;
      }
      if (at(Kw::template_))
      {
        parseTemplate(scope, flags);
        return;
      }
      if (at(Kw::namespace_))
      {
        parseNamespace();
        return;
      }
      if (at(Kw::static_assert_) || (at(Kw::extern_) && at(TokenKind::String, 1)))
      {
        parseDeclarationInner(scope, templ);
        return;
      }

      // Class / enum definitions (which may be followed by declarators).
      if (at(Kw::class_) || at(Kw::struct_) || at(Kw::union_))
      {
        if (parseClassOrElaborated(scope, flags, templ))
          return;
      }
      else if (at(Kw::enum_))
      {
        if (parseEnumOrElaborated(scope, flags, templ))
          return;
      }

      // Macro invocation used as a declaration: MACRO(...) ; or MACRO(...) { ... }
      if (atMacroName() && at(P::LParen, 1) && !(scope == Scope::Class && tok().id == currentClassName))
      {
        uint32_t save = pos;
        uint32_t name = tok().id;
        advance();
        skipBalanced();
        if (at(P::Semi) || at(P::RBrace) || atMacroName() || at(TokenKind::Keyword) || at(TokenKind::End) ||
            (atIdent() && !at(P::LParen, 1) && !at(P::ColonColon, 1) && !at(P::Less, 1)) || at(P::LBrace) ||
            (at(P::LBracket) && at(P::LBracket, 1)))
        {
          if (at(P::LBrace))
            skipBalanced();
          accept(P::Semi);
          MacroDecl* m = decl<MacroDecl>(start);
          m->name = name;
          m->endToken = pos;
          pushDecl(m);
          return;
        }
        restore(save);
      }
      // Bare ALL_CAPS macro statement at class/namespace scope: `MACRO;` or `MACRO` on its own line.
      if (atMacroName() && at(P::Semi, 1))
      {
        MacroDecl* m = decl<MacroDecl>(start);
        m->name = tok().id;
        advance();
        advance();
        m->endToken = pos;
        pushDecl(m);
        return;
      }

      // Constructor / destructor / conversion operator: a name followed by '(' with no type.
      Type* type = nullptr;
      {
        uint32_t save = pos;
        QualifiedName name;
        size_t mark = scratch.size();
        if (parseQualifiedName(name, true) && at(P::LParen) && isConstructorLikeName(name, scope))
        {
          Declarator d;
          d.name = name;
          parseFunctionSuffix(d);
          finishFunction(start, nullptr, d, flags, templ, scope);
          return;
        }
        restore(save);
        scratch.resize(mark);
      }

      type = parseType();
      if (!type)
      {
        error("expected declaration");
        return;
      }
      // Declarators.
      Type* baseType = stripSuffixes(type);
      bool first = true;
      for (;;)
      {
        Declarator d;
        d.type = first ? type : baseType;
        parseDeclarator(d, scope);
        if (inRecovery)
          return;
        first = false;
        if (d.isFunction)
        {
          finishFunction(start, d.type, d, flags, templ, scope);
          return;
        }
        VariableDecl* v = decl<VariableDecl>(start);
        v->type = d.type;
        v->name = d.name;
        v->flags = flags | d.flags;
        v->bitfield = d.bitfield;
        v->structuredBindings = d.structuredBindings;
        v->templateParams = templ.params;
        v->isTemplate = templ.present;
        parseInitializer(v);
        pushDecl(v);
        if (accept(P::Comma))
          continue;
        if (conditionMode && at(P::RParen))
          break;
        if (!expect(P::Semi, "expected ';' after declaration"))
          return;
        break;
      }
    }

    Type* stripSuffixes(Type* t)
    {
      for (;;)
      {
        if (t->kind == NodeKind::PointerType)
          t = static_cast<PointerType*>(t)->pointee;
        else if (t->kind == NodeKind::LRefType)
          t = static_cast<LRefType*>(t)->referenced;
        else if (t->kind == NodeKind::RRefType)
          t = static_cast<RRefType*>(t)->referenced;
        else if (t->kind == NodeKind::ConstType && static_cast<ConstType*>(t)->inner->kind == NodeKind::PointerType)
          t = static_cast<ConstType*>(t)->inner;
        else
          return t;
      }
    }

    bool isConstructorLikeName(const QualifiedName& name, Scope scope) const
    {
      const NamePart& last = name.last();
      if (last.special == 1 || last.special == 3)
        return true;
      if (last.special == 2)
        return false;
      if (name.parts.size() >= 2 && name.parts[name.parts.size() - 2].symbol == last.symbol)
        return true;
      if (scope == Scope::Class && name.parts.size() == 1 && last.symbol == currentClassName)
        return true;
      return false;
    }

    void parseDeclSpecifiers(uint32_t& flags)
    {
      for (;;)
      {
        skipAttributes();
        if (accept(Kw::static_)) flags |= DF_Static;
        else if (accept(Kw::virtual_)) flags |= DF_Virtual;
        else if (accept(Kw::inline_)) flags |= DF_Inline;
        else if (accept(Kw::constexpr_)) flags |= DF_Constexpr;
        else if (accept(Kw::consteval_)) flags |= DF_Consteval;
        else if (accept(Kw::constinit_)) flags |= DF_Constexpr;
        else if (accept(Kw::friend_)) flags |= DF_Friend;
        else if (accept(Kw::extern_)) flags |= DF_Extern;
        else if (accept(Kw::mutable_)) flags |= DF_Mutable;
        else if (accept(Kw::thread_local_)) flags |= DF_ThreadLocal;
        else if (accept(Kw::typedef_)) flags |= DF_Typedef;
        else if (accept(Kw::register_)) flags |= DF_Register;
        else if (accept(Kw::export_)) flags |= DF_Export;
        else if (atIdent(kwImplicit) && !at(P::LParen, 1)) { advance(); flags |= DF_Implicit; }
        else if (atIdent(kwBothConst) && !at(P::LParen, 1)) { advance(); flags |= DF_BothConst; }
        else if (atIdent(kwAccumulate) && !at(P::LParen, 1) && !at(P::ColonColon, 1)) { advance(); flags |= DF_Accumulate; }
        else if (at(Kw::explicit_))
        {
          advance();
          flags |= DF_Explicit;
          if (at(P::LParen))
            skipBalanced();
        }
        else if (skipMacroSpecifier())
        {
        }
        else
          return;
      }
    }

    // --- declarators ---

    void parseDeclarator(Declarator& d, Scope scope)
    {
      d.type = parseTypeSuffixes(d.type);
      skipAttributes();
      // Function pointer / reference-to-array: ( * name )(params) or ( & name )[N]
      if (at(P::LParen) && (((at(P::Star, 1) || at(P::Amp, 1) || at(P::AndAnd, 1)) &&
                             (atIdent(2) || at(Kw::const_, 2) || at(P::Star, 2) || atIdent(1) && false)) ||
                            (atIdent(1) && at(P::ColonColon, 2) && at(P::Star, 3))))
      {
        advance();
        Type* inner = parseTypeSuffixes(d.type);
        if (atIdent(0) && at(P::ColonColon, 1))
        {
          // Pointer to member: ( C::* name )
          QualifiedName q;
          parseQualifiedName(q, true, false);
          if (accept(P::Star))
          {
            MemberPointerType* m = node<MemberPointerType>(pos);
            m->classType = nullptr;
            m->pointee = inner;
            inner = m;
          }
          inner = parseTypeSuffixes(inner);
        }
        if (atIdent())
          parseQualifiedName(d.name, true);
        // Array suffix inside the parens.
        while (at(P::LBracket))
          inner = parseArraySuffix(inner);
        if (!expect(P::RParen, "expected ')' in declarator"))
          return;
        if (at(P::LParen))
        {
          // Pointer to function: the declared type is a pointer to FunctionType.
          FunctionType* f = node<FunctionType>(pos);
          f->result = d.type;
          Declarator tmp;
          parseFunctionSuffix(tmp);
          f->params = tmp.params;
          f->isConst = (tmp.flags & DF_Const) != 0;
          f->isNoexcept = (tmp.flags & DF_Noexcept) != 0;
          PointerType* p = node<PointerType>(pos);
          p->pointee = f;
          d.type = p;
        }
        else
          d.type = inner;
        while (at(P::LBracket))
          d.type = parseArraySuffix(d.type);
        return;
      }
      // Structured binding: auto [a, b] = ...
      if (at(P::LBracket) && d.type->kind == NodeKind::AutoType)
      {
        advance();
        std::vector<Expr*> names;
        while (atIdent())
        {
          NameExpr* n = node<NameExpr>(pos);
          NamePart part;
          part.symbol = tok().id;
          advance();
          n->name.parts = makeSpanOf(std::vector<NamePart>{part});
          names.push_back(n);
          if (!accept(P::Comma))
            break;
        }
        expect(P::RBracket, "expected ']'");
        d.structuredBindings = makeSpanOf(names);
        return;
      }
      if (atMacroName() && (atIdent(1) || at(P::ColonColon, 1)) && !at(P::ColonColon, 1))
        advance();
      if (atIdent() || at(P::ColonColon) || at(P::Tilde) || at(Kw::operator_))
      {
        if (!parseQualifiedName(d.name, true))
        {
          error("expected declarator name");
          return;
        }
      }
      skipAttributes();
      while (at(P::LBracket) && !at(P::LBracket, 1))
        d.type = parseArraySuffix(d.type);
      if (at(P::LParen) && !d.name.empty())
      {
        // Function, unless this is a variable with parenthesised initializer. Decide by looking
        // at the contents: a parameter list starts with a type or is empty.
        if (looksLikeParameterList(scope))
        {
          d.isFunction = true;
          parseFunctionSuffix(d);
        }
      }
      if (at(P::Colon) && !at(P::ColonColon) && scope == Scope::Class && !d.isFunction)
      {
        advance();
        d.bitfield = parseAssignmentNoBrace();
      }
    }

    Type* parseArraySuffix(Type* element)
    {
      ArrayType* a = node<ArrayType>(pos);
      advance();
      a->element = element;
      a->size = at(P::RBracket) ? nullptr : parseExpression();
      expect(P::RBracket, "expected ']'");
      return a;
    }

    bool looksLikeParameterList(Scope scope)
    {
      if (at(TokenKind::Number, 1) || at(TokenKind::String, 1) || at(TokenKind::Char, 1) || at(P::Minus, 1) ||
          at(Kw::true_, 1) || at(Kw::false_, 1) || at(Kw::nullptr_, 1) || at(Kw::this_, 1))
        return false;
      if (scope != Scope::Local)
        return true; // At namespace/class scope `name(...)` is a function.
      // Local: `T x(args);` is a variable, `T f(int);` a function (rare). Guess: parameter list if
      // empty or starts with a builtin type keyword / const / `void`.
      if (at(P::RParen, 1))
        return true;
      const Token& t = tok(1);
      if (t.getKind() == TokenKind::Keyword && (isBuiltinTypeKeyword(Kw(t.id)) || t.is(Kw::const_)))
        return !at(P::LParen, 2) && !at(P::LBrace, 2) && !at(P::ColonColon, 2) && !at(P::Dot, 2);
      return false;
    }

    // Parses `(params) cv ref noexcept -> ret override final = 0/default/delete requires ...`.
    void parseFunctionSuffix(Declarator& d)
    {
      expect(P::LParen, "expected '('");
      size_t mark = scratch.size();
      if (!at(P::RParen))
      {
        for (;;)
        {
          if (accept(P::Ellipsis))
          {
            d.flags |= DF_Variadic;
            break;
          }
          Decl* p = parseParameter();
          if (!p)
            break;
          scratch.push_back(p);
          if (!accept(P::Comma))
            break;
        }
      }
      d.params = makeSpan<Decl*>(mark);
      if (!expect(P::RParen, "expected ')' after parameters"))
        return;
      for (;;)
      {
        skipAttributes();
        if (accept(Kw::const_)) d.flags |= DF_Const;
        else if (accept(Kw::volatile_)) d.flags |= DF_Volatile;
        else if (accept(P::Amp) || accept(P::AndAnd)) d.flags |= DF_RefQual;
        else if (at(Kw::noexcept_))
        {
          advance();
          d.flags |= DF_Noexcept;
          if (at(P::LParen))
            skipBalanced();
        }
        else if (at(Kw::throw_) && at(P::LParen, 1))
        {
          advance();
          skipBalanced();
        }
        else if (atIdent(0) && spelling() == "override") { advance(); d.flags |= DF_Override; }
        else if (atIdent(0) && spelling() == "final") { advance(); d.flags |= DF_Final; }
        else if (at(P::Arrow))
        {
          advance();
          d.trailingReturn = parseType();
          if (!d.trailingReturn)
            error("expected trailing return type");
        }
        else if (at(Kw::requires_))
        {
          advance();
          d.requiresClause = parseRequiresClause();
        }
        else if (at(P::Assign))
        {
          if (at(TokenKind::Number, 1)) { advance(); advance(); d.flags |= DF_Pure; }
          else if (at(Kw::default_, 1)) { advance(); advance(); d.flags |= DF_Default; }
          else if (at(Kw::delete_, 1)) { advance(); advance(); d.flags |= DF_Delete; }
          else return;
        }
        else if (atMacroName() && !at(P::LBrace, 1) && !at(P::Semi, 1) && !at(P::Colon, 1))
        {
          // e.g. `void f() KOV_NORETURN;`
          advance();
          if (at(P::LParen))
            skipBalanced();
        }
        else if (atMacroName() && (at(P::LBrace, 1) || at(P::Semi, 1) || at(P::Colon, 1)))
        {
          advance();
        }
        else
          return;
      }
    }

    Decl* parseParameter()
    {
      uint32_t start = pos;
      skipAttributes();
      uint32_t flags = 0;
      parseDeclSpecifiers(flags);
      ParamDecl* p = decl<ParamDecl>(start);
      p->type = parseType();
      if (!p->type)
      {
        error("expected parameter type");
        return nullptr;
      }
      if (accept(P::Ellipsis))
        p->isPack = true;
      Declarator d;
      d.type = p->type;
      parseDeclarator(d, Scope::Local);
      p->type = d.type;
      if (!d.name.empty())
        p->name = d.name.last().symbol;
      if (d.isFunction)
      {
        FunctionType* f = node<FunctionType>(start);
        f->result = d.type;
        f->params = d.params;
        p->type = f;
      }
      if (accept(P::Assign))
        p->defaultValue = parseAssignment();
      else if (at(P::LBrace))
        p->defaultValue = parseBraceInit();
      return p;
    }

    void finishFunction(uint32_t start, Type* returnType, Declarator& d, uint32_t flags, TemplateHeader templ, Scope scope)
    {
      FunctionDecl* f = decl<FunctionDecl>(start);
      f->returnType = returnType;
      f->name = d.name;
      f->params = d.params;
      f->flags = flags | d.flags;
      f->trailingReturn = d.trailingReturn;
      f->requiresClause = d.requiresClause ? d.requiresClause : templ.requiresClause;
      f->templateParams = templ.params;
      f->isTemplate = templ.present;
      const NamePart& last = d.name.last();
      if (last.special == 1) f->flags |= DF_Destructor;
      else if (last.special == 2) f->flags |= DF_Operator;
      else if (last.special == 3) f->flags |= DF_Conversion;
      else if (!returnType) f->flags |= DF_Constructor;
      (void)scope;
      bool functionTry = accept(Kw::try_);
      // Constructor initializers.
      if (at(P::Colon))
      {
        advance();
        std::vector<FunctionDecl::CtorInit> inits;
        for (;;)
        {
          FunctionDecl::CtorInit init{};
          if (!parseQualifiedName(init.member, true, false))
          {
            error("expected member initializer");
            break;
          }
          size_t mark = scratch.size();
          if (accept(P::LParen))
          {
            parseExpressionList(P::RParen);
            init.braced = false;
            expect(P::RParen, "expected ')'");
          }
          else if (accept(P::LBrace))
          {
            parseExpressionList(P::RBrace);
            init.braced = true;
            expect(P::RBrace, "expected '}'");
          }
          else
          {
            error("expected '(' or '{' in member initializer");
            break;
          }
          init.args = makeSpan<Expr*>(mark);
          init.isPack = accept(P::Ellipsis);
          inits.push_back(init);
          if (!accept(P::Comma))
            break;
        }
        f->ctorInits = makeSpanOf(inits);
      }
      if (functionTry || at(Kw::try_))
      {
        // function-try-block
        accept(Kw::try_);
        f->body = parseBlock();
        while (at(Kw::catch_))
        {
          advance();
          if (at(P::LParen))
            skipBalanced();
          parseBlock();
        }
      }
      else if (at(P::LBrace))
      {
        Decl* savedParent = currentParent;
        currentParent = f;
        f->body = parseBlock();
        currentParent = savedParent;
        ++out.functionBodies;
      }
      else if (!expect(P::Semi, "expected ';' or function body"))
        return;
      pushDecl(f);
    }

    void parseInitializer(VariableDecl* v)
    {
      if (accept(P::Assign))
      {
        v->init = at(P::LBrace) ? parseBraceInit() : parseAssignment();
      }
      else if (at(P::LBrace))
      {
        v->init = parseBraceInit();
        v->bracedInit = true;
      }
      else if (at(P::LParen))
      {
        // Direct initialization `T x(args)`.
        uint32_t start = pos;
        advance();
        FunctionalCastExpr* c = node<FunctionalCastExpr>(start);
        c->type = v->type;
        c->braced = false;
        size_t mark = scratch.size();
        parseExpressionList(P::RParen);
        c->args = makeSpan<Expr*>(mark);
        expect(P::RParen, "expected ')'");
        v->init = c;
      }
    }

    // --- namespaces, using, templates ---

    void parseNamespace()
    {
      uint32_t start = pos;
      advance();
      NamespaceDecl* ns = decl<NamespaceDecl>(start);
      if (at(Kw::inline_))
      {
        advance();
        ns->isInline = true;
      }
      skipAttributes();
      if (atIdent())
      {
        if (at(P::Assign, 1))
        {
          // namespace alias
          UsingAliasDecl* alias = decl<UsingAliasDecl>(start);
          alias->name = tok().id;
          advance();
          advance();
          NamedType* n = node<NamedType>(pos);
          if (!parseQualifiedName(n->name, true, false))
            error("expected namespace name");
          alias->type = n;
          expect(P::Semi, "expected ';'");
          pushDecl(alias);
          return;
        }
        parseQualifiedName(ns->nested, false, false);
        ns->name = ns->nested.parts[0].symbol;
      }
      if (!expect(P::LBrace, "expected '{' after namespace"))
        return;
      Decl* savedParent = currentParent;
      Access savedAccess = currentAccess;
      currentParent = ns;
      currentAccess = Access::Default;
      size_t mark = scratch.size();
      while (!at(P::RBrace) && !at(TokenKind::End))
        parseDeclaration(Scope::Namespace);
      ns->decls = makeSpan<Decl*>(mark);
      currentParent = savedParent;
      currentAccess = savedAccess;
      expect(P::RBrace, "expected '}' to close namespace");
      pushDecl(ns);
    }

    void parseUsing(TemplateHeader templ)
    {
      uint32_t start = pos;
      advance();
      if (accept(Kw::namespace_))
      {
        UsingDirectiveDecl* u = decl<UsingDirectiveDecl>(start);
        if (!parseQualifiedName(u->name, true, false))
          error("expected namespace name");
        expect(P::Semi, "expected ';'");
        pushDecl(u);
        return;
      }
      bool isEnum = accept(Kw::enum_);
      if (atIdent() && at(P::Assign, 1))
      {
        UsingAliasDecl* u = decl<UsingAliasDecl>(start);
        u->name = tok().id;
        u->templateParams = templ.params;
        u->isTemplate = templ.present;
        advance();
        advance();
        skipAttributes();
        u->type = parseType();
        if (!u->type)
          error("expected type in alias");
        expect(P::Semi, "expected ';'");
        pushDecl(u);
        return;
      }
      for (;;)
      {
        UsingNameDecl* u = decl<UsingNameDecl>(start);
        u->isEnum = isEnum;
        accept(Kw::typename_);
        if (!parseQualifiedName(u->name, true))
        {
          error("expected name after using");
          return;
        }
        accept(P::Ellipsis);
        pushDecl(u);
        if (!accept(P::Comma))
          break;
      }
      expect(P::Semi, "expected ';'");
    }

    void parseTemplate(Scope scope, uint32_t flags = 0)
    {
      uint32_t start = pos;
      advance();
      TemplateHeader header;
      header.present = true;
      if (at(P::Less))
      {
        advance();
        size_t mark = scratch.size();
        while (!at(P::Greater) && !at(P::Shr) && !at(TokenKind::End))
        {
          Decl* p = parseTemplateParam();
          if (!p)
            break;
          scratch.push_back(p);
          if (!accept(P::Comma))
            break;
        }
        header.params = makeSpan<Decl*>(mark);
        if (!acceptTemplateClose())
        {
          error("expected '>' after template parameters");
          return;
        }
      }
      else
      {
        // Explicit instantiation: `template class X<int>;` / `template void f<int>(int);`
        header.present = false;
      }
      if (at(Kw::requires_))
      {
        advance();
        header.requiresClause = parseRequiresClause();
      }
      if (at(Kw::template_))
      {
        // Nested template header (member template of a class template).
        size_t mark = scratch.size();
        parseTemplate(scope, flags);
        // Attach the outer parameters to the produced declaration.
        if (Decl* d = lastPushedDecl(mark))
        {
          if (d->templateParams.empty())
            d->templateParams = header.params;
        }
        return;
      }
      uint32_t declFlags = flags;
      parseDeclSpecifiers(declFlags);
      (void)start;
      if (at(Kw::class_) || at(Kw::struct_) || at(Kw::union_))
      {
        if (parseClassOrElaborated(scope, declFlags, header))
          return;
      }
      if (at(Kw::using_))
      {
        parseUsing(header);
        return;
      }
      if (at(Kw::concept_) || at(Kw::enum_) || true)
      {
        // Everything else: function or variable template, specialisation, friend.
        TemplateHeader h = header;
        if (h.params.empty() && h.present)
          declFlags |= DF_Specialization;
        parseDeclarationInnerWithFlags(scope, h, declFlags);
      }
    }

    void parseDeclarationInnerWithFlags(Scope scope, TemplateHeader templ, uint32_t flags)
    {
      // Re-enter the generic path with pre-parsed flags by temporarily stashing them: the
      // generic parser re-parses specifiers (none left), so just call it with templ.
      size_t mark = scratch.size();
      parseDeclarationInner(scope, templ);
      for (size_t i = mark; i < scratch.size(); ++i)
        static_cast<Decl*>(scratch[i])->flags |= flags;
    }

    Decl* parseTemplateParam()
    {
      uint32_t start = pos;
      TemplateParamDecl* p = decl<TemplateParamDecl>(start);
      if (at(Kw::typename_) || at(Kw::class_))
      {
        advance();
        p->isType = true;
        if (accept(P::Ellipsis))
          p->isPack = true;
        if (atIdent())
        {
          p->name = tok().id;
          advance();
        }
        if (accept(P::Assign))
          p->defaultArg = parseType();
        return p;
      }
      if (at(Kw::template_))
      {
        // template<template<...> class T>
        advance();
        if (at(P::Less))
        {
          advance();
          size_t mark = scratch.size();
          while (!at(P::Greater) && !at(TokenKind::End))
          {
            Decl* inner = parseTemplateParam();
            if (!inner)
              break;
            scratch.push_back(inner);
            if (!accept(P::Comma))
              break;
          }
          p->innerParams = makeSpan<Decl*>(mark);
          acceptTemplateClose();
        }
        if (!accept(Kw::class_))
          accept(Kw::typename_);
        if (accept(P::Ellipsis))
          p->isPack = true;
        if (atIdent())
        {
          p->name = tok().id;
          advance();
        }
        if (accept(P::Assign))
          p->defaultArg = parseType();
        return p;
      }
      // Non-type parameter (possibly a concept-constrained type parameter `Concept T`).
      p->isType = false;
      p->valueType = parseType();
      if (!p->valueType)
      {
        error("expected template parameter");
        return nullptr;
      }
      if (accept(P::Ellipsis))
        p->isPack = true;
      if (atIdent())
      {
        p->name = tok().id;
        advance();
        // Constrained type parameter: `Concept<X> T` parsed as valueType=Concept, name=T.
        if (p->valueType->kind == NodeKind::NamedType)
          p->isType = true; // Heuristic: could also be `int N`; builtin types are not NamedType.
      }
      if (accept(P::Assign))
      {
        bool dummy = false;
        p->defaultArg = parseTemplateArgument(dummy);
      }
      return p;
    }

    // --- classes and enums ---

    // Returns true if a declaration was produced; false if this is an elaborated type specifier
    // that the caller should parse as a normal declaration (`struct X* p;`).
    bool parseClassOrElaborated(Scope scope, uint32_t flags, TemplateHeader templ)
    {
      uint32_t start = pos;
      uint32_t save = pos;
      Kw keyword = Kw(tok().id);
      advance();
      skipAttributes();
      skipMacroSpecifier();
      ClassDecl* c = decl<ClassDecl>(start);
      c->keyword = keyword;
      c->flags = flags;
      c->templateParams = templ.params;
      c->isTemplate = templ.present;
      if (templ.present && templ.params.empty())
        c->flags |= DF_Specialization;
      if (atIdent() || at(P::ColonColon))
      {
        if (!parseQualifiedName(c->name, true, false))
        {
          restore(save);
          return false;
        }
      }
      skipAttributes();
      if (atIdent() && spelling() == "final")
      {
        advance();
        c->isFinal = true;
      }
      if (atIdent(kwClassConst))
      {
        advance();
        c->isClassConst = true;
      }
      if (at(P::Semi))
      {
        advance();
        pushDecl(c);
        return true;
      }
      if (!at(P::LBrace) && !at(P::Colon))
      {
        // Elaborated type specifier used in a declaration.
        restore(save);
        return false;
      }
      if (accept(P::Colon))
      {
        std::vector<ClassDecl::Base> bases;
        for (;;)
        {
          ClassDecl::Base base{};
          base.access = Access::Default;
          for (;;)
          {
            if (accept(Kw::virtual_)) base.isVirtual = true;
            else if (accept(Kw::public_)) base.access = Access::Public;
            else if (accept(Kw::private_)) base.access = Access::Private;
            else if (accept(Kw::protected_)) base.access = Access::Protected;
            else break;
          }
          base.type = parseType();
          if (!base.type)
          {
            error("expected base class");
            return true;
          }
          base.isPack = accept(P::Ellipsis);
          bases.push_back(base);
          if (!accept(P::Comma))
            break;
        }
        c->bases = makeSpanOf(bases);
      }
      if (!expect(P::LBrace, "expected '{' in class"))
        return true;
      c->isDefinition = true;
      Decl* savedParent = currentParent;
      Access savedAccess = currentAccess;
      uint32_t savedClassName = currentClassName;
      currentParent = c;
      currentAccess = keyword == Kw::class_ ? Access::Private : Access::Public;
      currentClassName = c->name.empty() ? UINT32_MAX : c->name.last().symbol;
      size_t mark = scratch.size();
      while (!at(P::RBrace) && !at(TokenKind::End))
        parseDeclaration(Scope::Class);
      c->members = makeSpan<Decl*>(mark);
      currentParent = savedParent;
      currentAccess = savedAccess;
      currentClassName = savedClassName;
      expect(P::RBrace, "expected '}' to close class");
      skipAttributes();
      pushDecl(c);
      // Declarators after the body: `struct { ... } x, *y;`
      if (!at(P::Semi))
      {
        parseDeclSpecifiers(flags);
        NamedType* selfType = node<NamedType>(start);
        selfType->name = c->name;
        for (;;)
        {
          Declarator d;
          d.type = selfType;
          parseDeclarator(d, scope);
          if (inRecovery)
            return true;
          VariableDecl* v = decl<VariableDecl>(start);
          v->type = d.type;
          v->name = d.name;
          v->flags = flags | d.flags;
          parseInitializer(v);
          pushDecl(v);
          if (!accept(P::Comma))
            break;
        }
      }
      expect(P::Semi, "expected ';' after class");
      return true;
    }

    bool parseEnumOrElaborated(Scope scope, uint32_t flags, TemplateHeader templ)
    {
      uint32_t start = pos;
      uint32_t save = pos;
      advance();
      EnumDecl* e = decl<EnumDecl>(start);
      e->flags = flags;
      e->templateParams = templ.params;
      if (accept(Kw::class_) || accept(Kw::struct_))
        e->isClass = true;
      skipAttributes();
      skipMacroSpecifier();
      if (atIdent() || at(P::ColonColon))
      {
        if (!parseQualifiedName(e->name, true, false))
        {
          restore(save);
          return false;
        }
      }
      if (accept(P::Colon))
      {
        e->underlying = parseType();
        if (!e->underlying)
        {
          error("expected underlying type");
          return true;
        }
      }
      if (at(P::Semi))
      {
        advance();
        pushDecl(e);
        return true;
      }
      if (!at(P::LBrace))
      {
        restore(save);
        return false;
      }
      advance();
      e->isDefinition = true;
      Decl* savedParent = currentParent;
      currentParent = e;
      std::vector<Decl*> enumerators;
      size_t mark = scratch.size();
      while (!at(P::RBrace) && !at(TokenKind::End))
      {
        skipAttributes();
        if (atIdent() && (at(P::Comma, 1) || at(P::RBrace, 1) || at(P::Assign, 1) || at(P::LBracket, 1)))
        {
          EnumeratorDecl* en = decl<EnumeratorDecl>(pos);
          en->name = tok().id;
          advance();
          skipAttributes();
          if (accept(P::Assign))
            en->value = parseAssignmentNoBrace();
          enumerators.push_back(en);
          if (!accept(P::Comma))
            break;
          continue;
        }
        if (atMacroName() && at(P::LParen, 1))
        {
          // Enumerators generated by a macro.
          MacroDecl* m = decl<MacroDecl>(pos);
          m->name = tok().id;
          advance();
          skipBalanced();
          m->endToken = pos;
          enumerators.push_back(m);
          accept(P::Comma);
          continue;
        }
        if (accept(P::Semi))
          continue;
        // Kov: member declarations inside an enum.
        parseDeclaration(Scope::Enum);
        if (inRecovery)
          break;
      }
      e->members = makeSpan<Decl*>(mark);
      e->enumerators = makeSpanOf(enumerators);
      currentParent = savedParent;
      expect(P::RBrace, "expected '}' to close enum");
      pushDecl(e);
      if (!at(P::Semi))
      {
        NamedType* selfType = node<NamedType>(start);
        selfType->name = e->name;
        for (;;)
        {
          Declarator d;
          d.type = selfType;
          parseDeclarator(d, scope);
          if (inRecovery)
            return true;
          VariableDecl* v = decl<VariableDecl>(start);
          v->type = d.type;
          v->name = d.name;
          v->flags = flags | d.flags;
          parseInitializer(v);
          pushDecl(v);
          if (!accept(P::Comma))
            break;
        }
      }
      expect(P::Semi, "expected ';' after enum");
      return true;
    }

    // --- statements ---

    Stmt* parseBlock()
    {
      uint32_t start = pos;
      BlockStmt* b = node<BlockStmt>(start);
      if (!expect(P::LBrace, "expected '{'"))
        return b;
      size_t mark = scratch.size();
      while (!at(P::RBrace) && !at(TokenKind::End))
      {
        Stmt* s = parseStatement();
        if (s)
          scratch.push_back(s);
      }
      b->stmts = makeSpan<Stmt*>(mark);
      expect(P::RBrace, "expected '}'");
      if (inRecovery)
        inRecovery = false; // The block end is a reliable recovery point.
      return b;
    }

    Stmt* parseStatement()
    {
      DepthGuard guard(*this);
      if (depth > 400)
      {
        error("nesting too deep");
        recover();
        return nullptr;
      }
      uint32_t start = pos;
      size_t mark = scratch.size();
      Stmt* s = parseStatementInner();
      if (inRecovery)
      {
        scratch.resize(mark);
        if (pos == start)
          advance();
        recover();
        ErrorStmt* e = node<ErrorStmt>(start);
        e->endToken = pos;
        return e;
      }
      return s;
    }

    Stmt* parseStatementInner()
    {
      skipAttributes();
      uint32_t start = pos;
      if (at(TokenKind::End))
        return nullptr;
      if (at(P::LBrace))
        return parseBlock();
      if (accept(P::Semi))
        return node<EmptyStmt>(start);
      if (at(TokenKind::Keyword))
      {
        switch (Kw(tok().id))
        {
          case Kw::if_:
          {
            advance();
            IfStmt* s = node<IfStmt>(start);
            if (accept(Kw::constexpr_))
              s->isConstexpr = true;
            if (atIdent() && spelling() == "consteval")
              advance();
            if (!expect(P::LParen, "expected '(' after if"))
              return s;
            parseCondition(s->init, s->cond);
            expect(P::RParen, "expected ')'");
            s->then = parseStatement();
            if (accept(Kw::else_))
              s->else_ = parseStatement();
            return s;
          }
          case Kw::while_:
          {
            advance();
            WhileStmt* s = node<WhileStmt>(start);
            expect(P::LParen, "expected '('");
            Stmt* dummy = nullptr;
            parseCondition(dummy, s->cond);
            expect(P::RParen, "expected ')'");
            s->body = parseStatement();
            return s;
          }
          case Kw::do_:
          {
            advance();
            DoStmt* s = node<DoStmt>(start);
            s->body = parseStatement();
            if (!accept(Kw::while_))
              error("expected 'while' after do body");
            expect(P::LParen, "expected '('");
            s->cond = parseExpression();
            expect(P::RParen, "expected ')'");
            expect(P::Semi, "expected ';'");
            return s;
          }
          case Kw::for_:
            return parseFor();
          case Kw::switch_:
          {
            advance();
            SwitchStmt* s = node<SwitchStmt>(start);
            expect(P::LParen, "expected '('");
            parseCondition(s->init, s->cond);
            expect(P::RParen, "expected ')'");
            s->body = parseStatement();
            return s;
          }
          case Kw::case_:
          {
            advance();
            CaseStmt* s = node<CaseStmt>(start);
            s->value = parseTernaryNoColon();
            if (accept(P::Ellipsis))
              s->rangeEnd = parseTernaryNoColon();
            expect(P::Colon, "expected ':' after case");
            return s;
          }
          case Kw::default_:
            advance();
            expect(P::Colon, "expected ':' after default");
            return node<DefaultStmt>(start);
          case Kw::return_:
          case Kw::co_return_:
          {
            advance();
            ReturnStmt* s = node<ReturnStmt>(start);
            if (!at(P::Semi))
              s->value = at(P::LBrace) ? parseBraceInit() : parseExpression();
            expect(P::Semi, "expected ';' after return");
            return s;
          }
          case Kw::break_:
            advance();
            expect(P::Semi, "expected ';'");
            return node<BreakStmt>(start);
          case Kw::continue_:
            advance();
            expect(P::Semi, "expected ';'");
            return node<ContinueStmt>(start);
          case Kw::goto_:
          {
            advance();
            GotoStmt* s = node<GotoStmt>(start);
            s->label = atIdent() ? tok().id : UINT32_MAX;
            advance();
            expect(P::Semi, "expected ';'");
            return s;
          }
          case Kw::try_:
          {
            advance();
            TryStmt* s = node<TryStmt>(start);
            s->body = parseBlock();
            std::vector<TryStmt::Handler> handlers;
            while (at(Kw::catch_))
            {
              advance();
              TryStmt::Handler h{};
              expect(P::LParen, "expected '(' after catch");
              if (!accept(P::Ellipsis))
                h.param = parseParameter();
              expect(P::RParen, "expected ')'");
              h.body = parseBlock();
              handlers.push_back(h);
            }
            s->handlers = makeSpanOf(handlers);
            return s;
          }
          case Kw::asm_:
            advance();
            if (at(P::LParen))
              skipBalanced();
            accept(P::Semi);
            return node<AsmStmt>(start);
          case Kw::namespace_:
          case Kw::using_:
          case Kw::static_assert_:
          case Kw::template_:
          case Kw::typedef_:
          case Kw::class_:
          case Kw::struct_:
          case Kw::union_:
          case Kw::enum_:
          case Kw::static_:
          case Kw::constexpr_:
          case Kw::const_:
          case Kw::volatile_:
          case Kw::extern_:
          case Kw::thread_local_:
          case Kw::register_:
          case Kw::auto_:
          case Kw::typename_:
          case Kw::inline_:
          case Kw::constinit_:
          case Kw::consteval_:
            return parseDeclStmt();
          default:
            if (isBuiltinTypeKeyword(Kw(tok().id)) && !at(P::LParen, 1) && !at(P::LBrace, 1) && !at(P::ColonColon, 1))
              return parseDeclStmt();
            break;
        }
      }
      if (atIdent() && (spelling() == "__asm" || spelling() == "__asm__"))
      {
        advance();
        if (at(P::LParen) || at(P::LBrace))
          skipBalanced();
        else
          while (!at(P::Semi) && !at(P::RBrace) && !at(TokenKind::End))
            advance();
        accept(P::Semi);
        return node<AsmStmt>(start);
      }
      if (atIdent(kwFallthrough) && at(P::Semi, 1))
      {
        advance();
        advance();
        return node<FallthroughStmt>(start);
      }
      // Label.
      if (atIdent() && at(P::Colon, 1) && !at(P::ColonColon, 1))
      {
        LabelStmt* s = node<LabelStmt>(start);
        s->label = tok().id;
        advance();
        advance();
        return s;
      }
      if (atMacroName() && at(P::LParen, 1))
      {
        uint32_t save = pos;
        size_t mark = scratch.size();
        size_t savedDiagnostics = out.diagnostics.size();
        uint32_t nodes = out.nodeCount;
        ExprStmt* s = node<ExprStmt>(start);
        s->expr = parseExpression();
        if (!inRecovery && accept(P::Semi))
          return s;
        if (!inRecovery && (at(P::RBrace) || atIdent() || at(TokenKind::Keyword)))
          return s; // Macro statement without ';', e.g. `WHEN_X(...)` on its own line.
        // Arguments that are not expressions (statements, types...): swallow the macro.
        inRecovery = false;
        out.diagnostics.resize(savedDiagnostics);
        scratch.resize(mark);
        out.nodeCount = nodes;
        restore(save);
        MacroStmt* m = node<MacroStmt>(start);
        m->name = tok().id;
        advance();
        skipBalanced();
        accept(P::Semi);
        return m;
      }
      if (looksLikeDeclaration())
        return parseDeclStmt();
      ExprStmt* s = node<ExprStmt>(start);
      s->expr = parseExpression();
      if (!at(P::Semi) && !inRecovery && s->expr->kind == NodeKind::CallExpr &&
          static_cast<CallExpr*>(s->expr)->callee->kind == NodeKind::NameExpr &&
          (at(P::LBrace) || at(P::RBrace) || atIdent() || at(TokenKind::Keyword)))
        return s; // Function-like macro expanding to a statement, used without ';'.
      expect(P::Semi, "expected ';' after expression");
      return s;
    }

    // Syntactic guess whether a statement starting with a name is a declaration.
    bool looksLikeDeclaration()
    {
      if (at(TokenKind::Keyword))
      {
        switch (Kw(tok().id))
        {
          case Kw::const_: case Kw::volatile_: case Kw::static_: case Kw::constexpr_: case Kw::auto_:
          case Kw::struct_: case Kw::class_: case Kw::union_: case Kw::enum_: case Kw::typename_:
          case Kw::extern_: case Kw::thread_local_: case Kw::register_: case Kw::typedef_: case Kw::using_:
          case Kw::inline_: case Kw::constinit_: case Kw::consteval_: case Kw::static_assert_:
            return true;
          case Kw::decltype_:
            break; // speculative path below
          default:
            return isBuiltinTypeKeyword(Kw(tok().id)) && !at(P::LParen, 1) && !at(P::LBrace, 1) && !at(P::ColonColon, 1);
        }
      }
      if (!atIdent() && !at(P::ColonColon) && !at(Kw::decltype_))
        return false;
      uint32_t save = pos;
      size_t mark = scratch.size();
      bool savedRecovery = inRecovery;
      size_t savedDiagnostics = out.diagnostics.size();
      uint32_t nodes = out.nodeCount;
      bool result = false;
      // Macro statement `MACRO(...);` is an expression (a call) unless followed by a declaration.
      if (skipMacroSpecifier())
        result = atIdent() || at(TokenKind::Keyword);
      if (!result)
      {
        Type* t = parseType();
        if (t)
        {
          if (atIdent())
            result = at(P::Semi, 1) || at(P::Assign, 1) || at(P::LParen, 1) || at(P::LBrace, 1) || at(P::Comma, 1) ||
                     at(P::LBracket, 1);
          if (atIdent() && !result)
          {
            // `T x` followed by anything else (e.g. a macro suffix) is still most likely a declaration.
            result = !at(P::ColonColon, 1) && !at(P::Less, 1) && !at(P::Dot, 1) && !at(P::Arrow, 1) && !at(P::LParen, 1);
            if (at(P::LParen, 1))
              result = true;
          }
          else if (at(P::Star) || at(P::Amp) || at(P::AndAnd))
          {
            // `T* x` / `T& x`: after the suffixes an identifier must follow.
            while (at(P::Star) || at(P::Amp) || at(P::AndAnd) || at(Kw::const_))
              advance();
            result = atIdent() && (at(P::Semi, 1) || at(P::Assign, 1) || at(P::LParen, 1) || at(P::LBrace, 1) ||
                                   at(P::Comma, 1) || at(P::LBracket, 1) || at(P::RParen, 1));
          }
          else if (at(P::LParen) && (at(P::Star, 1) || at(P::Amp, 1)) && atIdent(2) && (at(P::RParen, 3) || at(P::LBracket, 3)))
            result = true; // function pointer declarator
          else if (at(P::LBracket) && t->kind == NodeKind::AutoType)
            result = true;
        }
      }
      restore(save);
      scratch.resize(mark);
      inRecovery = savedRecovery;
      out.diagnostics.resize(savedDiagnostics);
      out.nodeCount = nodes;
      return result;
    }

    Stmt* parseDeclStmt()
    {
      uint32_t start = pos;
      DeclStmt* s = node<DeclStmt>(start);
      size_t mark = scratch.size();
      parseDeclarationInner(Scope::Local, {});
      s->decls = makeSpan<Decl*>(mark);
      return s;
    }

    // Condition of if/while/switch: either an expression or a declaration `T x = e`, optionally
    // preceded by an init-statement `stmt;`.
    void parseCondition(Stmt*& init, Node*& cond, bool allowInit = true)
    {
      uint32_t save = pos;
      if (looksLikeDeclaration())
      {
        uint32_t start = pos;
        size_t mark = scratch.size();
        bool savedMode = conditionMode;
        conditionMode = true;
        parseDeclarationInner(Scope::Local, {});
        conditionMode = savedMode;
        if (inRecovery)
        {
          // Not actually a declaration: fall back to an expression.
          inRecovery = false;
          out.diagnostics.pop_back();
          scratch.resize(mark);
          restore(save);
        }
        else
        {
          DeclStmt* d = node<DeclStmt>(start);
          d->decls = makeSpan<Decl*>(mark);
          // parseDeclarationInner consumed a ';' only if it was an init-statement.
          if (toks[pos - 1].is(P::Semi) && allowInit)
          {
            init = d;
            parseCondition(init, cond, false);
            if (!cond)
              cond = d;
            return;
          }
          cond = d;
          return;
        }
      }
      Expr* e = parseExpression();
      if (allowInit && accept(P::Semi))
      {
        ExprStmt* s = node<ExprStmt>(e ? e->token : pos);
        s->expr = e;
        init = s;
        Stmt* dummy = nullptr;
        parseCondition(dummy, cond, false);
        return;
      }
      cond = e;
    }

    Stmt* parseFor()
    {
      uint32_t start = pos;
      advance();
      if (!expect(P::LParen, "expected '(' after for"))
        return nullptr;
      // Range-for: `for (decl : range)`. Detect by scanning for ':' before ';' at depth 0.
      {
        int level = 0;
        bool isRange = false;
        for (uint32_t i = pos; i < toks.size(); ++i)
        {
          const Token& t = toks[i];
          if (t.is(P::LParen) || t.is(P::LBracket) || t.is(P::LBrace)) ++level;
          else if (t.is(P::RParen) || t.is(P::RBracket) || t.is(P::RBrace))
          {
            if (level == 0) break;
            --level;
          }
          else if (level == 0 && t.is(P::Semi)) break;
          else if (level == 0 && t.is(P::Colon)) { isRange = true; break; }
          else if (t.getKind() == TokenKind::End) break;
        }
        if (isRange)
        {
          RangeForStmt* s = node<RangeForStmt>(start);
          uint32_t declStart = pos;
          uint32_t flags = 0;
          parseDeclSpecifiers(flags);
          VariableDecl* v = decl<VariableDecl>(declStart);
          v->type = parseType();
          if (!v->type)
          {
            error("expected range variable type");
            return s;
          }
          Declarator d;
          d.type = v->type;
          parseDeclarator(d, Scope::Local);
          v->type = d.type;
          v->name = d.name;
          v->structuredBindings = d.structuredBindings;
          v->flags = flags;
          s->var = v;
          expect(P::Colon, "expected ':' in range for");
          s->range = at(P::LBrace) ? parseBraceInit() : parseExpression();
          expect(P::RParen, "expected ')'");
          s->body = parseStatement();
          return s;
        }
      }
      ForStmt* s = node<ForStmt>(start);
      if (accept(P::Semi))
        s->init = nullptr;
      else if (looksLikeDeclaration())
        s->init = parseDeclStmt();
      else
      {
        ExprStmt* e = node<ExprStmt>(pos);
        e->expr = parseExpression();
        expect(P::Semi, "expected ';' in for");
        s->init = e;
      }
      if (!at(P::Semi))
      {
        Stmt* dummy = nullptr;
        parseCondition(dummy, s->cond, false);
      }
      expect(P::Semi, "expected ';' in for");
      if (!at(P::RParen))
        s->step = parseExpression();
      expect(P::RParen, "expected ')' after for");
      s->body = parseStatement();
      return s;
    }

    // --- expressions ---

    // Precedence climbing. Levels (low to high):
    // comma < assignment/ternary < || < && < | < ^ < & < ==,!= < <,>,<=,>= < <=> < <<,>> < +,- < *,/,% < .*,->* < unary < postfix
    Expr* parseExpression()
    {
      DepthGuard guard(*this);
      if (depth > 400)
      {
        error("nesting too deep");
        return node<ErrorExpr>(pos);
      }
      Expr* e = parseAssignment();
      while (at(P::Comma))
      {
        uint32_t opTok = pos;
        advance();
        if (at(P::Ellipsis))
        {
          advance();
          FoldExpr* f = node<FoldExpr>(opTok);
          f->lhs = e;
          f->rhs = nullptr;
          f->op = uint32_t(P::Comma);
          e = f;
          continue;
        }
        BinaryExpr* b = node<BinaryExpr>(opTok);
        b->lhs = e;
        b->op = uint32_t(P::Comma);
        b->isKeyword = false;
        b->rhs = parseAssignment();
        e = b;
      }
      return e;
    }

    // Pushes comma-separated assignment expressions (or brace-inits) onto scratch until `close`.
    void parseExpressionList(P close)
    {
      if (at(close))
        return;
      for (;;)
      {
        Expr* e = at(P::LBrace) ? parseBraceInit() : parseAssignment();
        if (accept(P::Ellipsis))
        {
          UnaryExpr* u = node<UnaryExpr>(pos - 1);
          u->operand = e;
          u->op = uint32_t(P::Ellipsis);
          u->isKeyword = false;
          u->postfix = true;
          e = u;
        }
        scratch.push_back(e);
        if (!accept(P::Comma))
          break;
        if (at(close))
          break;
      }
    }

    Expr* parseBraceInit()
    {
      uint32_t start = pos;
      BraceInitExpr* b = node<BraceInitExpr>(start);
      expect(P::LBrace, "expected '{'");
      size_t mark = scratch.size();
      while (!at(P::RBrace) && !at(TokenKind::End))
      {
        // Designated initializer `.x = 1`.
        if (at(P::Dot) && atIdent(1))
        {
          advance();
          advance();
          if (!accept(P::Assign))
          {
            if (at(P::LBrace))
            {
              scratch.push_back(parseBraceInit());
              if (!accept(P::Comma))
                break;
              continue;
            }
          }
        }
        Expr* e = at(P::LBrace) ? parseBraceInit() : parseAssignment();
        if (accept(P::Ellipsis))
        {
        }
        scratch.push_back(e);
        if (!accept(P::Comma))
          break;
        if (inRecovery)
          break;
      }
      b->args = makeSpan<Expr*>(mark);
      expect(P::RBrace, "expected '}' to close initializer");
      return b;
    }

    static bool isAssignmentOp(P p)
    {
      switch (p)
      {
        case P::Assign: case P::PlusAssign: case P::MinusAssign: case P::StarAssign: case P::SlashAssign:
        case P::PercentAssign: case P::AmpAssign: case P::PipeAssign: case P::CaretAssign: case P::ShlAssign:
        case P::ShrAssign:
          return true;
        default:
          return false;
      }
    }

    Expr* parseAssignment() { return parseAssignmentImpl(true, true); }
    Expr* parseAssignmentNoBrace() { return parseAssignmentImpl(true, false); }
    Expr* parseTernaryNoColon() { return parseAssignmentImpl(false, true); }

    Expr* parseAssignmentImpl(bool allowTernary, bool allowBrace)
    {
      uint32_t start = pos;
      if (at(Kw::throw_))
      {
        advance();
        ThrowExpr* t = node<ThrowExpr>(start);
        t->operand = at(P::Semi) || at(P::RParen) || at(P::Comma) || at(P::Colon) ? nullptr : parseAssignment();
        return t;
      }
      if (at(Kw::co_yield_))
        advance();
      Expr* lhs = parseLogicalOr();
      if (at(TokenKind::Punctuator))
      {
        P p = P(tok().id);
        if (isAssignmentOp(p))
        {
          uint32_t opTok = pos;
          advance();
          BinaryExpr* b = node<BinaryExpr>(opTok);
          b->lhs = lhs;
          b->op = uint32_t(p);
          b->isKeyword = false;
          b->rhs = (at(P::LBrace) && allowBrace) ? parseBraceInit() : parseAssignmentImpl(true, allowBrace);
          return b;
        }
        if (p == P::Question && allowTernary)
        {
          uint32_t opTok = pos;
          advance();
          ConditionalExpr* c = node<ConditionalExpr>(opTok);
          c->cond = lhs;
          c->then = parseExpression();
          expect(P::Colon, "expected ':' in conditional");
          c->else_ = parseAssignmentImpl(true, allowBrace);
          return c;
        }
        if (p == P::Elvis)
        {
          uint32_t opTok = pos;
          advance();
          ConditionalExpr* c = node<ConditionalExpr>(opTok);
          c->cond = lhs;
          c->then = nullptr;
          c->else_ = parseAssignmentImpl(true, allowBrace);
          return c;
        }
      }
      return lhs;
    }

    // Expression inside template arguments: no top-level '>' so relational operators are off.
    bool noGreater = false;
    Expr* parseExpressionNoGreater(bool& sawLogical)
    {
      bool saved = noGreater;
      noGreater = true;
      uint32_t start = pos;
      Expr* e = parseAssignmentImpl(true, true);
      // Detect top-level && / || for the comparison heuristic.
      for (uint32_t i = start; i < pos; ++i)
        if (toks[i].is(P::AndAnd) || toks[i].is(P::OrOr))
          sawLogical = true;
      noGreater = saved;
      return e;
    }

    int binaryPrecedence(const Token& t, bool& isKeyword) const
    {
      isKeyword = false;
      if (t.getKind() == TokenKind::Keyword)
      {
        isKeyword = true;
        switch (Kw(t.id))
        {
          case Kw::or_: return 1;
          case Kw::and_: return 2;
          case Kw::bitor_: return 3;
          case Kw::xor_: return 4;
          case Kw::bitand_: return 5;
          case Kw::not_eq_: return 6;
          default: return -1;
        }
      }
      if (t.getKind() != TokenKind::Punctuator)
        return -1;
      switch (P(t.id))
      {
        case P::OrOr: return 1;
        case P::AndAnd: return 2;
        case P::Pipe: return 3;
        case P::Caret: return 4;
        case P::Amp: return 5;
        case P::EqEq: case P::NotEq: return 6;
        case P::Less: case P::LessEq: case P::GreaterEq: return 7;
        case P::Greater: return noGreater ? -1 : 7;
        case P::Spaceship: return 8;
        case P::Shl: return 9;
        case P::Shr: return noGreater ? -1 : 9;
        case P::Plus: case P::Minus: return 10;
        case P::Star: case P::Slash: case P::Percent: return 11;
        case P::DotStar: case P::ArrowStar: return 12;
        default: return -1;
      }
    }

    Expr* parseLogicalOr() { return parseBinary(1); }

    Expr* parseRequiresClause()
    {
      bool saved = inRequires;
      inRequires = true;
      Expr* e = parseLogicalOr();
      inRequires = saved;
      return e;
    }

    Expr* parseBinary(int minPrec)
    {
      Expr* lhs = parseUnary();
      for (;;)
      {
        bool isKeyword;
        int prec = binaryPrecedence(tok(), isKeyword);
        if (prec < minPrec)
          return lhs;
        if (noGreater && (at(P::GreaterEq) || at(P::ShrAssign)))
          return lhs;
        uint32_t opTok = pos;
        uint32_t op = tok().id;
        advance();
        // Fold expression `pack op ...`
        if (at(P::Ellipsis))
        {
          advance();
          FoldExpr* f = node<FoldExpr>(opTok);
          f->lhs = lhs;
          f->op = op;
          f->rhs = nullptr;
          if (!at(P::RParen))
          {
            // ( pack op ... op init )
            bool k;
            if (binaryPrecedence(tok(), k) >= 0)
            {
              advance();
              f->rhs = parseBinary(prec + 1);
            }
          }
          lhs = f;
          continue;
        }
        Expr* rhs = parseBinary(prec + 1);
        BinaryExpr* b = node<BinaryExpr>(opTok);
        b->lhs = lhs;
        b->rhs = rhs;
        b->op = op;
        b->isKeyword = isKeyword;
        lhs = b;
      }
    }

    Expr* parseUnary()
    {
      DepthGuard guard(*this);
      if (depth > 400)
      {
        error("nesting too deep");
        return node<ErrorExpr>(pos);
      }
      uint32_t start = pos;
      if (at(TokenKind::Punctuator))
      {
        P p = P(tok().id);
        switch (p)
        {
          case P::PlusPlus: case P::MinusMinus: case P::Plus: case P::Minus: case P::Bang: case P::Tilde:
          case P::Star: case P::Amp: case P::AndAnd:
          {
            // `~Foo()` could be a destructor name in `obj.~Foo()` but that goes through member parsing.
            advance();
            UnaryExpr* u = node<UnaryExpr>(start);
            u->op = uint32_t(p);
            u->isKeyword = false;
            u->postfix = false;
            u->operand = parseUnary();
            return u;
          }
          case P::Ellipsis:
          {
            // Fold expression `( ... op pack )`.
            advance();
            bool k;
            if (binaryPrecedence(tok(), k) >= 0)
            {
              FoldExpr* f = node<FoldExpr>(start);
              f->op = tok().id;
              advance();
              f->lhs = nullptr;
              f->rhs = parseBinary(1);
              return f;
            }
            error("unexpected '...'");
            return node<ErrorExpr>(start);
          }
          case P::ColonColon:
            if (at(Kw::new_, 1) || at(Kw::delete_, 1))
            {
              advance();
              Expr* e = parseUnary();
              if (e->kind == NodeKind::NewExpr) static_cast<NewExpr*>(e)->global = true;
              else if (e->kind == NodeKind::DeleteExpr) static_cast<DeleteExpr*>(e)->global = true;
              return e;
            }
            break;
          default:
            break;
        }
      }
      if (at(TokenKind::Keyword))
      {
        switch (Kw(tok().id))
        {
          case Kw::not_: case Kw::compl_:
          {
            advance();
            UnaryExpr* u = node<UnaryExpr>(start);
            u->op = uint32_t(Kw(toks[start].id));
            u->isKeyword = true;
            u->postfix = false;
            u->operand = parseUnary();
            return u;
          }
          case Kw::sizeof_: case Kw::alignof_:
          {
            SizeofExpr* s = node<SizeofExpr>(start);
            s->sizeofKind = Kw(tok().id);
            advance();
            if (accept(P::Ellipsis))
              s->isPack = true;
            if (at(P::LParen))
            {
              uint32_t save = pos;
              size_t mark = scratch.size();
              advance();
              Type* t = parseType();
              if (t && at(P::RParen))
              {
                advance();
                s->type = t;
                return s;
              }
              restore(save);
              scratch.resize(mark);
            }
            s->operand = parseUnary();
            return s;
          }
          case Kw::noexcept_:
            if (at(P::LParen, 1))
            {
              advance();
              skipBalanced();
              LiteralExpr* l = node<LiteralExpr>(start);
              l->literalKind = TokenKind::Keyword;
              l->keyword = Kw::true_;
              return l;
            }
            break;
          case Kw::new_:
            return parseNew(start);
          case Kw::delete_:
          {
            advance();
            DeleteExpr* d = node<DeleteExpr>(start);
            d->global = false;
            d->isArray = false;
            if (at(P::LBracket) && at(P::RBracket, 1))
            {
              advance();
              advance();
              d->isArray = true;
            }
            d->operand = parseUnary();
            return d;
          }
          case Kw::co_await_:
            advance();
            return parseUnary();
          case Kw::static_cast_: case Kw::dynamic_cast_: case Kw::reinterpret_cast_: case Kw::const_cast_:
          {
            NamedCastExpr* c = node<NamedCastExpr>(start);
            c->castKind = Kw(tok().id);
            advance();
            expect(P::Less, "expected '<' after cast");
            c->type = parseType();
            if (!c->type)
              error("expected type in cast");
            if (!acceptTemplateClose())
              error("expected '>' after cast type");
            expect(P::LParen, "expected '(' in cast");
            c->operand = parseExpression();
            expect(P::RParen, "expected ')' in cast");
            return parsePostfix(c);
          }
          case Kw::requires_:
          {
            advance();
            if (at(P::LParen))
              skipBalanced();
            if (at(P::LBrace))
              skipBalanced();
            return node<RequiresExpr>(start);
          }
          default:
            break;
        }
      }
      // C-style cast `(Type)expr`.
      if (at(P::LParen))
      {
        uint32_t save = pos;
        size_t mark = scratch.size();
        uint32_t nodes = out.nodeCount;
        advance();
        Type* t = parseType();
        if (t && at(P::RParen))
        {
          advance();
          if (looksLikeCastOperand(t))
          {
            CastExpr* c = node<CastExpr>(start);
            c->type = t;
            c->operand = parseUnary();
            return c;
          }
        }
        restore(save);
        scratch.resize(mark);
        out.nodeCount = nodes;
      }
      return parsePostfix(parsePrimary());
    }

    bool looksLikeCastOperand(Type* t) const
    {
      // After `(T)`: a cast if T is obviously a type (builtin, pointer, const...) or the next
      // token cannot continue an expression that `(T)` started.
      bool obviousType = t->kind != NodeKind::NamedType;
      if (obviousType)
        return atIdent() || at(TokenKind::Number) || at(TokenKind::String) || at(TokenKind::Char) || at(P::LParen) ||
               at(P::Minus) || at(P::Plus) || at(P::Bang) || at(P::Tilde) || at(P::Star) || at(P::Amp) ||
               at(P::PlusPlus) || at(P::MinusMinus) || at(TokenKind::Keyword) || at(P::ColonColon) || at(P::LBracket);
      return atIdent() || at(TokenKind::Number) || at(TokenKind::String) || at(TokenKind::Char) || at(P::Bang) ||
             at(P::Tilde) || at(Kw::this_) || at(Kw::sizeof_) || at(Kw::new_) || at(Kw::true_) || at(Kw::false_) ||
             at(Kw::nullptr_) || at(P::ColonColon) || at(Kw::static_cast_) || at(Kw::reinterpret_cast_) ||
             at(Kw::const_cast_) || at(Kw::dynamic_cast_);
    }

    Expr* parseNew(uint32_t start)
    {
      advance();
      NewExpr* n = node<NewExpr>(start);
      size_t mark = scratch.size();
      if (at(P::LParen))
      {
        // Placement or parenthesised type.
        uint32_t save = pos;
        uint32_t nodes = out.nodeCount;
        advance();
        Type* t = parseType();
        if (t && at(P::RParen) && !at(P::LParen, 1) && !at(P::LBrace, 1) && !atIdent(1))
        {
          // new (T) is rare; treat as placement unless clearly a type.
        }
        restore(save);
        out.nodeCount = nodes;
        scratch.resize(mark);
        advance();
        parseExpressionList(P::RParen);
        n->placement = makeSpan<Expr*>(mark);
        expect(P::RParen, "expected ')' after placement arguments");
      }
      if (at(P::LParen))
      {
        advance();
        n->type = parseType();
        expect(P::RParen, "expected ')' after new type");
      }
      else
        n->type = parseType();
      if (!n->type)
      {
        error("expected type after new");
        return n;
      }
      while (at(P::LBracket))
        n->type = parseArraySuffix(n->type);
      mark = scratch.size();
      if (accept(P::LParen))
      {
        parseExpressionList(P::RParen);
        n->args = makeSpan<Expr*>(mark);
        expect(P::RParen, "expected ')' after new arguments");
      }
      else if (at(P::LBrace))
      {
        scratch.push_back(parseBraceInit());
        n->args = makeSpan<Expr*>(mark);
        n->braced = true;
      }
      return n;
    }

    Expr* parsePrimary()
    {
      uint32_t start = pos;
      switch (tok().getKind())
      {
        case TokenKind::Number:
        case TokenKind::Char:
        {
          LiteralExpr* l = node<LiteralExpr>(start);
          l->literalKind = tok().getKind();
          l->keyword = Kw::Count;
          advance();
          return l;
        }
        case TokenKind::String:
        {
          LiteralExpr* l = node<LiteralExpr>(start);
          l->literalKind = TokenKind::String;
          l->keyword = Kw::Count;
          while (at(TokenKind::String) || (atMacroName() && !at(P::LParen, 1) && !at(P::ColonColon, 1)))
            advance();
          return l;
        }
        case TokenKind::Keyword:
          switch (Kw(tok().id))
          {
            case Kw::true_: case Kw::false_: case Kw::nullptr_:
            {
              LiteralExpr* l = node<LiteralExpr>(start);
              l->literalKind = TokenKind::Keyword;
              l->keyword = Kw(tok().id);
              advance();
              return l;
            }
            case Kw::this_:
              advance();
              return node<ThisExpr>(start);
            case Kw::typeid_:
            {
              advance();
              TypeidExpr* t = node<TypeidExpr>(start);
              expect(P::LParen, "expected '('");
              uint32_t save = pos;
              Type* ty = parseType();
              if (ty && at(P::RParen))
                t->operand = ty;
              else
              {
                restore(save);
                t->operand = parseExpression();
              }
              expect(P::RParen, "expected ')'");
              return t;
            }
            case Kw::typename_:
            case Kw::decltype_:
            case Kw::struct_:
            case Kw::class_:
            case Kw::union_:
            case Kw::enum_:
              return parseTypeAsExpr(start);
            default:
              if (isBuiltinTypeKeyword(Kw(tok().id)))
                return parseTypeAsExpr(start);
              error("unexpected keyword in expression");
              return node<ErrorExpr>(start);
          }
        case TokenKind::Identifier:
          return parseNameExpr(start);
        case TokenKind::Punctuator:
          if (at(P::LParen))
          {
            advance();
            ParenExpr* p = node<ParenExpr>(start);
            p->inner = parseExpression();
            expect(P::RParen, "expected ')'");
            return p;
          }
          if (at(P::LBracket))
            return parseLambda(start);
          if (at(P::LBrace))
            return parseBraceInit();
          if (at(P::ColonColon))
            return parseNameExpr(start);
          break;
        default:
          break;
      }
      error("expected expression");
      return node<ErrorExpr>(start);
    }

    // `int(x)`, `typename T::type{...}`, `decltype(x)::f()`, `Foo<T>::bar` where the name is a type-ish keyword.
    Expr* parseTypeAsExpr(uint32_t start)
    {
      Type* t = parseType();
      if (!t)
      {
        error("expected type");
        return node<ErrorExpr>(start);
      }
      if (at(P::LParen) || at(P::LBrace))
      {
        FunctionalCastExpr* c = node<FunctionalCastExpr>(start);
        c->type = t;
        c->braced = at(P::LBrace);
        size_t mark = scratch.size();
        if (c->braced)
          scratch.push_back(parseBraceInit());
        else
        {
          advance();
          parseExpressionList(P::RParen);
          expect(P::RParen, "expected ')'");
        }
        c->args = makeSpan<Expr*>(mark);
        return c;
      }
      if (at(P::ColonColon))
      {
        // decltype(x)::member
        advance();
        NameExpr* n = node<NameExpr>(start);
        parseQualifiedName(n->name, false);
        return n;
      }
      // A type where an expression was expected (e.g. in a macro argument). Represent as a name.
      NameExpr* n = node<NameExpr>(start);
      if (t->kind == NodeKind::NamedType)
        n->name = static_cast<NamedType*>(t)->name;
      return n;
    }

    Expr* parseNameExpr(uint32_t start)
    {
      NameExpr* n = node<NameExpr>(start);
      if (!parseQualifiedName(n->name, false))
      {
        error("expected name");
        return node<ErrorExpr>(start);
      }
      if (at(TokenKind::String) && n->name.parts.size() == 1 && isAllCaps(spelling(toks[start])))
      {
        LiteralExpr* l = node<LiteralExpr>(start);
        l->literalKind = TokenKind::String;
        l->keyword = Kw::Count;
        while (at(TokenKind::String) || (atMacroName() && !at(P::LParen, 1) && !at(P::ColonColon, 1)))
          advance();
        return l;
      }
      // `Name{...}` is a braced functional cast / aggregate construction.
      if (at(P::LBrace) && !noGreater)
      {
        // Distinguish from a block starting after an expression statement: only at expression
        // positions where '{' cannot start a statement. The caller contexts (arguments,
        // initializers, return) are the common ones; `Foo{}` as a statement start is rare.
        FunctionalCastExpr* c = node<FunctionalCastExpr>(start);
        NamedType* t = node<NamedType>(start);
        t->name = n->name;
        c->type = t;
        c->braced = true;
        size_t mark = scratch.size();
        scratch.push_back(parseBraceInit());
        c->args = makeSpan<Expr*>(mark);
        return c;
      }
      return n;
    }

    Expr* parseLambda(uint32_t start)
    {
      LambdaExpr* l = node<LambdaExpr>(start);
      skipBalanced(); // captures
      if (at(P::Less))
      {
        advance();
        size_t mark = scratch.size();
        while (!at(P::Greater) && !at(TokenKind::End))
        {
          Decl* p = parseTemplateParam();
          if (!p)
            break;
          scratch.push_back(p);
          if (!accept(P::Comma))
            break;
        }
        l->templateParams = makeSpan<Decl*>(mark);
        acceptTemplateClose();
      }
      if (at(P::LParen))
      {
        Declarator d;
        parseFunctionSuffix(d);
        l->params = d.params;
        l->returnType = d.trailingReturn;
      }
      for (;;)
      {
        if (accept(Kw::mutable_) || accept(Kw::constexpr_) || accept(Kw::consteval_))
          continue;
        if (at(Kw::noexcept_))
        {
          advance();
          if (at(P::LParen))
            skipBalanced();
          continue;
        }
        if (at(P::Arrow))
        {
          advance();
          l->returnType = parseType();
          continue;
        }
        if (at(Kw::requires_))
        {
          advance();
          parseRequiresClause();
          continue;
        }
        skipAttributes();
        break;
      }
      if (!at(P::LBrace))
      {
        error("expected lambda body");
        return l;
      }
      l->body = parseBlock();
      return l;
    }

    Expr* parsePostfix(Expr* e)
    {
      for (;;)
      {
        uint32_t start = pos;
        if (at(P::LParen))
        {
          advance();
          CallExpr* c = node<CallExpr>(start);
          c->callee = e;
          size_t mark = scratch.size();
          parseExpressionList(P::RParen);
          c->args = makeSpan<Expr*>(mark);
          expect(P::RParen, "expected ')' after arguments");
          e = c;
        }
        else if (at(P::LBracket) && !at(P::LBracket, 1))
        {
          advance();
          IndexExpr* i = node<IndexExpr>(start);
          i->base = e;
          size_t mark = scratch.size();
          parseExpressionList(P::RBracket);
          i->args = makeSpan<Expr*>(mark);
          expect(P::RBracket, "expected ']'");
          e = i;
        }
        else if (at(P::Dot) || at(P::Arrow) || at(P::SafeArrow))
        {
          P op = P(tok().id);
          advance();
          MemberExpr* m = node<MemberExpr>(start);
          m->base = e;
          m->op = op;
          accept(Kw::template_);
          if (!parseQualifiedName(m->member, false))
          {
            error("expected member name");
            return m;
          }
          e = m;
        }
        else if (at(P::PlusPlus) || at(P::MinusMinus))
        {
          UnaryExpr* u = node<UnaryExpr>(start);
          u->operand = e;
          u->op = tok().id;
          u->isKeyword = false;
          u->postfix = true;
          advance();
          e = u;
        }
        else if (at(P::LBrace) && e->kind == NodeKind::NameExpr && false)
        {
        }
        else
          return e;
      }
    }
  };

  template<> constexpr NodeKind Parser::nodeKindOf<NamedType>() { return NodeKind::NamedType; }
  template<> constexpr NodeKind Parser::nodeKindOf<PointerType>() { return NodeKind::PointerType; }
  template<> constexpr NodeKind Parser::nodeKindOf<LRefType>() { return NodeKind::LRefType; }
  template<> constexpr NodeKind Parser::nodeKindOf<RRefType>() { return NodeKind::RRefType; }
  template<> constexpr NodeKind Parser::nodeKindOf<ConstType>() { return NodeKind::ConstType; }
  template<> constexpr NodeKind Parser::nodeKindOf<ArrayType>() { return NodeKind::ArrayType; }
  template<> constexpr NodeKind Parser::nodeKindOf<FunctionType>() { return NodeKind::FunctionType; }
  template<> constexpr NodeKind Parser::nodeKindOf<DecltypeType>() { return NodeKind::DecltypeType; }
  template<> constexpr NodeKind Parser::nodeKindOf<AutoType>() { return NodeKind::AutoType; }
  template<> constexpr NodeKind Parser::nodeKindOf<BuiltinType>() { return NodeKind::BuiltinType; }
  template<> constexpr NodeKind Parser::nodeKindOf<MemberPointerType>() { return NodeKind::MemberPointerType; }
  template<> constexpr NodeKind Parser::nodeKindOf<PackType>() { return NodeKind::PackType; }
  template<> constexpr NodeKind Parser::nodeKindOf<NamespaceDecl>() { return NodeKind::NamespaceDecl; }
  template<> constexpr NodeKind Parser::nodeKindOf<ClassDecl>() { return NodeKind::ClassDecl; }
  template<> constexpr NodeKind Parser::nodeKindOf<EnumDecl>() { return NodeKind::EnumDecl; }
  template<> constexpr NodeKind Parser::nodeKindOf<EnumeratorDecl>() { return NodeKind::EnumeratorDecl; }
  template<> constexpr NodeKind Parser::nodeKindOf<FunctionDecl>() { return NodeKind::FunctionDecl; }
  template<> constexpr NodeKind Parser::nodeKindOf<ParamDecl>() { return NodeKind::ParamDecl; }
  template<> constexpr NodeKind Parser::nodeKindOf<VariableDecl>() { return NodeKind::VariableDecl; }
  template<> constexpr NodeKind Parser::nodeKindOf<UsingAliasDecl>() { return NodeKind::UsingAliasDecl; }
  template<> constexpr NodeKind Parser::nodeKindOf<UsingDirectiveDecl>() { return NodeKind::UsingDirectiveDecl; }
  template<> constexpr NodeKind Parser::nodeKindOf<UsingNameDecl>() { return NodeKind::UsingNameDecl; }
  template<> constexpr NodeKind Parser::nodeKindOf<TemplateParamDecl>() { return NodeKind::TemplateParamDecl; }
  template<> constexpr NodeKind Parser::nodeKindOf<StaticAssertDecl>() { return NodeKind::StaticAssertDecl; }
  template<> constexpr NodeKind Parser::nodeKindOf<MacroDecl>() { return NodeKind::MacroDecl; }
  template<> constexpr NodeKind Parser::nodeKindOf<ErrorDecl>() { return NodeKind::ErrorDecl; }
  template<> constexpr NodeKind Parser::nodeKindOf<EmptyDecl>() { return NodeKind::EmptyDecl; }
  template<> constexpr NodeKind Parser::nodeKindOf<AsmDecl>() { return NodeKind::AsmDecl; }
  template<> constexpr NodeKind Parser::nodeKindOf<FriendDecl>() { return NodeKind::FriendDecl; }
  template<> constexpr NodeKind Parser::nodeKindOf<BlockStmt>() { return NodeKind::BlockStmt; }
  template<> constexpr NodeKind Parser::nodeKindOf<ExprStmt>() { return NodeKind::ExprStmt; }
  template<> constexpr NodeKind Parser::nodeKindOf<DeclStmt>() { return NodeKind::DeclStmt; }
  template<> constexpr NodeKind Parser::nodeKindOf<IfStmt>() { return NodeKind::IfStmt; }
  template<> constexpr NodeKind Parser::nodeKindOf<ForStmt>() { return NodeKind::ForStmt; }
  template<> constexpr NodeKind Parser::nodeKindOf<RangeForStmt>() { return NodeKind::RangeForStmt; }
  template<> constexpr NodeKind Parser::nodeKindOf<WhileStmt>() { return NodeKind::WhileStmt; }
  template<> constexpr NodeKind Parser::nodeKindOf<DoStmt>() { return NodeKind::DoStmt; }
  template<> constexpr NodeKind Parser::nodeKindOf<SwitchStmt>() { return NodeKind::SwitchStmt; }
  template<> constexpr NodeKind Parser::nodeKindOf<CaseStmt>() { return NodeKind::CaseStmt; }
  template<> constexpr NodeKind Parser::nodeKindOf<DefaultStmt>() { return NodeKind::DefaultStmt; }
  template<> constexpr NodeKind Parser::nodeKindOf<ReturnStmt>() { return NodeKind::ReturnStmt; }
  template<> constexpr NodeKind Parser::nodeKindOf<BreakStmt>() { return NodeKind::BreakStmt; }
  template<> constexpr NodeKind Parser::nodeKindOf<ContinueStmt>() { return NodeKind::ContinueStmt; }
  template<> constexpr NodeKind Parser::nodeKindOf<GotoStmt>() { return NodeKind::GotoStmt; }
  template<> constexpr NodeKind Parser::nodeKindOf<LabelStmt>() { return NodeKind::LabelStmt; }
  template<> constexpr NodeKind Parser::nodeKindOf<TryStmt>() { return NodeKind::TryStmt; }
  template<> constexpr NodeKind Parser::nodeKindOf<EmptyStmt>() { return NodeKind::EmptyStmt; }
  template<> constexpr NodeKind Parser::nodeKindOf<ErrorStmt>() { return NodeKind::ErrorStmt; }
  template<> constexpr NodeKind Parser::nodeKindOf<FallthroughStmt>() { return NodeKind::FallthroughStmt; }
  template<> constexpr NodeKind Parser::nodeKindOf<AsmStmt>() { return NodeKind::AsmStmt; }
  template<> constexpr NodeKind Parser::nodeKindOf<MacroStmt>() { return NodeKind::MacroStmt; }
  template<> constexpr NodeKind Parser::nodeKindOf<NameExpr>() { return NodeKind::NameExpr; }
  template<> constexpr NodeKind Parser::nodeKindOf<LiteralExpr>() { return NodeKind::LiteralExpr; }
  template<> constexpr NodeKind Parser::nodeKindOf<ThisExpr>() { return NodeKind::ThisExpr; }
  template<> constexpr NodeKind Parser::nodeKindOf<CallExpr>() { return NodeKind::CallExpr; }
  template<> constexpr NodeKind Parser::nodeKindOf<IndexExpr>() { return NodeKind::IndexExpr; }
  template<> constexpr NodeKind Parser::nodeKindOf<MemberExpr>() { return NodeKind::MemberExpr; }
  template<> constexpr NodeKind Parser::nodeKindOf<UnaryExpr>() { return NodeKind::UnaryExpr; }
  template<> constexpr NodeKind Parser::nodeKindOf<BinaryExpr>() { return NodeKind::BinaryExpr; }
  template<> constexpr NodeKind Parser::nodeKindOf<ConditionalExpr>() { return NodeKind::ConditionalExpr; }
  template<> constexpr NodeKind Parser::nodeKindOf<CastExpr>() { return NodeKind::CastExpr; }
  template<> constexpr NodeKind Parser::nodeKindOf<NamedCastExpr>() { return NodeKind::NamedCastExpr; }
  template<> constexpr NodeKind Parser::nodeKindOf<SizeofExpr>() { return NodeKind::SizeofExpr; }
  template<> constexpr NodeKind Parser::nodeKindOf<NewExpr>() { return NodeKind::NewExpr; }
  template<> constexpr NodeKind Parser::nodeKindOf<DeleteExpr>() { return NodeKind::DeleteExpr; }
  template<> constexpr NodeKind Parser::nodeKindOf<LambdaExpr>() { return NodeKind::LambdaExpr; }
  template<> constexpr NodeKind Parser::nodeKindOf<BraceInitExpr>() { return NodeKind::BraceInitExpr; }
  template<> constexpr NodeKind Parser::nodeKindOf<ThrowExpr>() { return NodeKind::ThrowExpr; }
  template<> constexpr NodeKind Parser::nodeKindOf<FunctionalCastExpr>() { return NodeKind::FunctionalCastExpr; }
  template<> constexpr NodeKind Parser::nodeKindOf<TypeidExpr>() { return NodeKind::TypeidExpr; }
  template<> constexpr NodeKind Parser::nodeKindOf<FoldExpr>() { return NodeKind::FoldExpr; }
  template<> constexpr NodeKind Parser::nodeKindOf<RequiresExpr>() { return NodeKind::RequiresExpr; }
  template<> constexpr NodeKind Parser::nodeKindOf<ErrorExpr>() { return NodeKind::ErrorExpr; }
  template<> constexpr NodeKind Parser::nodeKindOf<ParenExpr>() { return NodeKind::ParenExpr; }
}

const char* nodeKindName(NodeKind kind)
{
  switch (kind)
  {
#define X(name) case NodeKind::name: return #name;
    KOV_NODE_KINDS(X)
#undef X
  }
  return "?";
}

namespace
{
  // Removes preprocessor directives from the token stream, keeping one branch of every
  // conditional: the first branch, except for `#if 0` where the else-branch is kept. This is a
  // stand-in for real configuration selection, good enough to parse a C++ code base as a corpus.
  void filterDirectives(std::vector<Token>& tokens, std::string_view text)
  {
    bool any = false;
    for (const Token& t : tokens)
      if (t.getKind() == TokenKind::Directive)
      {
        any = true;
        break;
      }
    if (!any)
      return;
    auto directiveName = [&](const Token& t, bool& isIfZero)
    {
      std::string_view s = text.substr(t.offset, t.length);
      size_t i = 1;
      while (i < s.size() && (s[i] == ' ' || s[i] == '\t'))
        ++i;
      size_t start = i;
      while (i < s.size() && ((s[i] >= 'a' && s[i] <= 'z') || s[i] == '_'))
        ++i;
      std::string_view name = s.substr(start, i - start);
      isIfZero = false;
      if (name == "if")
      {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t'))
          ++i;
        isIfZero = i < s.size() && s[i] == '0' && (i + 1 == s.size() || !(s[i + 1] >= '0' && s[i + 1] <= '9'));
      }
      return name;
    };
    std::vector<Token> result;
    result.reserve(tokens.size());
    // Skips tokens until the matching #else/#elif/#endif (stopAtElse) or #endif of the current level.
    auto skipBranch = [&](size_t& i, bool stopAtElse) -> std::string_view
    {
      int level = 0;
      for (; i < tokens.size(); ++i)
      {
        const Token& t = tokens[i];
        if (t.getKind() != TokenKind::Directive)
          continue;
        bool zero;
        std::string_view name = directiveName(t, zero);
        if (name == "if" || name == "ifdef" || name == "ifndef")
          ++level;
        else if (name == "endif")
        {
          if (level == 0)
            return name;
          --level;
        }
        else if (level == 0 && stopAtElse && (name == "else" || name == "elif"))
          return name;
      }
      return {};
    };
    for (size_t i = 0; i < tokens.size(); ++i)
    {
      const Token& t = tokens[i];
      if (t.getKind() != TokenKind::Directive)
      {
        result.push_back(t);
        continue;
      }
      bool zero;
      std::string_view name = directiveName(t, zero);
      if (zero)
      {
        // #if 0: skip to the else branch and take it; a following #elif is treated as taken.
        ++i;
        skipBranch(i, true);
      }
      else if (name == "else" || name == "elif")
      {
        // The first branch was taken; skip the rest up to #endif.
        ++i;
        skipBranch(i, false);
      }
    }
    tokens.swap(result);
  }
}

void parseFile(std::vector<Token>& tokens, std::string_view text, SymbolTable& symbols, ParsedFile& out)
{
  filterDirectives(tokens, text);
  Parser parser(tokens, text, symbols, out);
  parser.parseTranslationUnit();
}
