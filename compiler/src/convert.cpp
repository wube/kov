#include "convert.hpp"
#include "parser.hpp"
#include <algorithm>
#include <cctype>
#include <cstring>
#include <functional>
#include <map>
#include <set>

namespace
{
  struct Edit
  {
    uint32_t offset;
    uint32_t length; // 0: an insertion
    std::string text;
  };

  struct Counts
  {
    std::map<std::string, int> values;
    void add(const std::string& what, int n = 1) { values[what] += n; }
  };

  // Default arguments of a function's parameters, by index, as source text: carried from a
  // header declaration to the definition that replaces it in the converted file.
  using CarriedDefaults = std::map<FunctionDecl*, std::vector<std::pair<size_t, std::string>>>;

  // The index of the `,` or `)` that ends the parameter (or default argument) starting at token,
  // skipping nested brackets; UINT32_MAX when a `;` or the end comes first.
  uint32_t parameterEndIn(const std::vector<Token>& toks, uint32_t token)
  {
    int depth = 0;
    for (uint32_t i = token; i < toks.size(); ++i)
    {
      const Token& t = toks[i];
      if (t.is(P::LParen) || t.is(P::LBracket) || t.is(P::LBrace) || t.is(P::Less))
        ++depth;
      else if (t.is(P::RParen) || t.is(P::RBracket) || t.is(P::RBrace) || t.is(P::Greater))
      {
        if (depth == 0)
          return t.is(P::RParen) ? i : UINT32_MAX;
        --depth;
      }
      else if (t.is(P::Shr))
        depth -= 2;
      else if (t.is(P::Comma) && depth == 0)
        return i;
      else if (t.is(P::Semi) || t.getKind() == TokenKind::End)
        return UINT32_MAX;
    }
    return UINT32_MAX;
  }

  // Preprocessor symbols the converter treats as defined (Windows, MSVC, debug) and as undefined.
  const std::set<std::string> definedSymbols = {"WIN32", "_WIN32", "_WIN64", "_MSC_VER", "DEBUG", "__cplusplus", "FACTORIO_PACKAGE"};
  const std::set<std::string> undefinedSymbols = {"LINUX", "MAC_OSX", "NINTENDO_SWITCH", "NINTENDO_SWITCH1", "NINTENDO_SWITCH2", "__linux__",
                                                  "__APPLE__", "__GNUC__", "__clang__", "NDEBUG", "HEADLESS", "FACTORIO_USE_METAL", "DEMO"};

  // Known object-like and function-like macros of the corpus that have a direct Kov spelling.
  const std::map<std::string, std::string> knownMacros = {
    {"FORMAT_ATTR", ""}, {"TAG_FORMAT_ATTR", ""}, {"VCALL", ""}, {"NO_INLINE", "[[noinline]]"}, {"ALWAYS_INLINE", "[[forceinline]]"},
    {"DEV_TOOL_VARIABLE", ""}, {"BEGIN_SUPPRESS_CONVERSIONS", ""}, {"END_SUPPRESS_CONVERSIONS", ""}, {"BEGIN_FORCE_OPTIMIZE", ""},
    {"END_FORCE_OPTIMIZE", ""}, {"SUPPRESS_UBSAN", ""}, {"PADDING_WARNING", ""}, {"errno", "(*_errno())"}};
  const std::map<std::string, std::string> knownFunctionMacros = {{"FORMAT_STRING", "$1"}, {"WHEN_ASSERT", "$1"}, {"FOR_DEVELOPER", "$1"}};

  // std:: names and their Kov library spelling. Names not here keep their name and are counted.
  // C library functions become `::name`: inside a namespace with its own `sqrt`, `std::sqrt` meant the C one.
  const std::map<std::string, std::string> stdNames = {
    {"string", "String"}, {"string_view", "StringView"}, {"wstring", "WString"}, {"vector", "Vector"}, {"pair", "Pair"},
    {"optional", "Optional"}, {"map", "Map"}, {"set", "Set"}, {"unordered_map", "HashMap"}, {"unordered_set", "HashSet"},
    {"deque", "Deque"}, {"list", "List"}, {"array", "Array"}, {"span", "Span"}, {"unique_ptr", "UniquePtr"}, {"queue", "Queue"},
    {"replace", "replace"}, {"advance", "advance"}, {"make_optional", "makeOptional"}, {"clock", "::clock"}, {"abort", "::abort"},
    {"shared_ptr", "SharedPtr"}, {"weak_ptr", "WeakPtr"}, {"function", "Function"}, {"tuple", "Tuple"}, {"variant", "Variant"},
    {"initializer_list", "InitializerList"}, {"numeric_limits", "Limits"}, {"runtime_error", "RuntimeError"},
    {"logic_error", "LogicError"}, {"exception", "Exception"}, {"out_of_range", "OutOfRange"}, {"invalid_argument", "InvalidArgument"},
    {"move", "move"}, {"forward", "forward"}, {"swap", "swap"}, {"min", "min"}, {"max", "max"}, {"clamp", "clamp"},
    {"sort", "sort"}, {"stable_sort", "stableSort"}, {"find", "find"}, {"find_if", "findIf"}, {"remove_if", "removeIf"},
    {"remove", "remove"}, {"transform", "transform"}, {"count_if", "countIf"}, {"all_of", "allOf"}, {"any_of", "anyOf"},
    {"none_of", "noneOf"}, {"lower_bound", "lowerBound"}, {"upper_bound", "upperBound"}, {"reverse", "reverse"},
    {"fill", "fill"}, {"copy", "copy"}, {"accumulate", "accumulate"}, {"begin", "begin"}, {"end", "end"}, {"size", "size"},
    {"isnan", "::isnan"}, {"isinf", "::isinf"}, {"abs", "::abs"}, {"floor", "::floor"}, {"ceil", "::ceil"}, {"round", "::round"},
    {"sqrt", "::sqrt"}, {"pow", "::pow"}, {"tolower", "::tolower"}, {"toupper", "::toupper"}, {"size_t", "size_t"},
    {"nullptr_t", "nullptr_t"}, {"to_string", "toString"}, {"make_unique", "makeUnique"}, {"make_shared", "makeShared"},
    {"make_pair", "makePair"}, {"make_tuple", "makeTuple"}, {"get", "get"}, {"declval", "declval"}, {"memcpy", "::memcpy"},
    {"memset", "::memset"}, {"strlen", "::strlen"}, {"is_same_v", "isSame"}, {"is_array_v", "isArray"}, {"is_signed_v", "isSigned"},
    {"is_unsigned_v", "isUnsigned"}, {"is_pointer_v", "isPointer"}, {"is_enum_v", "isEnum"}, {"is_integral_v", "isIntegral"},
    {"is_floating_point_v", "isFloatingPoint"}, {"is_base_of_v", "isBaseOf"}, {"is_trivially_copyable_v", "isTriviallyCopyable"},
    {"remove_reference_t", "RemoveReference"}, {"remove_const_t", "RemoveConst"}, {"remove_cv_t", "RemoveCv"},
    {"remove_extent_t", "RemoveExtent"}, {"remove_pointer_t", "RemovePointer"}, {"decay_t", "Decay"}, {"underlying_type_t", "UnderlyingType"},
    {"conditional_t", "Conditional"}, {"enable_if_t", "EnableIf"}, {"chrono", "Chrono"}, {"atomic", "Atomic"}, {"mutex", "Mutex"},
    {"lock_guard", "LockGuard"}, {"unique_lock", "UniqueLock"}, {"thread", "Thread"}, {"condition_variable", "ConditionVariable"},
    {"exchange", "exchange"}, {"hash", "Hash"}, {"less", "Less"}, {"equal_to", "EqualTo"}, {"bind", "bind"}, {"ref", "ref"},
    {"cref", "cref"}, {"reference_wrapper", "ReferenceWrapper"}, {"is_const_v", "isConst"}, {"is_reference_v", "isReference"},
    {"numeric_limits", "Limits"}, {"strtoll", "::strtoll"}, {"strtoull", "::strtoull"}, {"strtod", "::strtod"}, {"strtof", "::strtof"},
    {"ostream", "OutStream"}, {"istream", "InStream"}, {"stringstream", "StringStream"}, {"ostringstream", "StringStream"},
    {"nullopt", "nullopt"}, {"count", "count"}, {"distance", "distance"}, {"max_element", "maxElement"}, {"min_element", "minElement"},
    {"nextafter", "::nextafter"}, {"rbegin", "rbegin"}, {"rend", "rend"}, {"unique", "unique"}, {"is_class_v", "isClass"},
    {"is_convertible_v", "isConvertible"}, {"is_null_pointer_v", "isNullPointer"}, {"back_inserter", "backInserter"},
    {"is_void_v", "isVoid"}, {"is_function_v", "isFunction"}, {"invoke", "invoke"}, {"apply", "apply"}, {"tie", "tie"},
    {"ignore", "ignore"}, {"index_sequence", "IndexSequence"}, {"make_index_sequence", "makeIndexSequence"}, {"byte", "u8"},
    {"int8_t", "int8_t"}, {"int16_t", "int16_t"}, {"int32_t", "int32_t"}, {"int64_t", "int64_t"}, {"uint8_t", "uint8_t"},
    {"uint16_t", "uint16_t"}, {"uint32_t", "uint32_t"}, {"uint64_t", "uint64_t"}, {"intptr_t", "intptr_t"}, {"uintptr_t", "uintptr_t"},
    {"ptrdiff_t", "ptrdiff_t"}, {"fabs", "::fabs"}, {"fmod", "::fmod"}, {"log", "::log"}, {"exp", "::exp"}, {"sin", "::sin"}, {"cos", "::cos"},
    {"atan2", "::atan2"}, {"isfinite", "::isfinite"}, {"signbit", "::signbit"}, {"trunc", "::trunc"}, {"lround", "::lround"}};

  bool isIdentChar(char c) { return isalnum((unsigned char)c) || c == '_'; }

  // --- hand-written overrides ---

  // What an override file defines, by qualified name: those definitions of the converted file
  // are dropped and the override file's text is appended.
  struct OverrideKeys
  {
    std::set<std::string> functions;           // "ns::name/arity", or "ns::name/*" for every overload
    std::map<std::string, int> functionCounts; // definitions per qualified name
    std::set<std::string> classes;             // "ns::Class" (its out-of-line members go too)
    std::set<std::string> variables;           // "ns::name"
  };

  std::string nameText(const QualifiedName& name, const SymbolTable& symbols)
  {
    std::string s;
    for (size_t i = 0; i < name.parts.size(); ++i)
    {
      const NamePart& p = name.parts[i];
      if (i)
        s += "::";
      if (p.special == 1)
        s += "~";
      if (p.special == 2)
        s += "operator#" + std::to_string(p.opToken);
      else if (p.special == 3)
        s += "operator-conversion";
      else if (p.symbol != UINT32_MAX)
        s += std::string(symbols.name(p.symbol));
    }
    return s;
  }

  bool isFunctionDefinition(FunctionDecl* f) { return f->body != nullptr || (f->flags & (DF_Default | DF_Delete)); }

  void collectOverrideKeys(Span<Decl*> decls, const std::string& prefix, const SymbolTable& symbols, OverrideKeys& out)
  {
    for (Decl* d : decls)
      switch (d->kind)
      {
        case NodeKind::NamespaceDecl:
        {
          auto* n = static_cast<NamespaceDecl*>(d);
          std::string name = !n->nested.empty() ? nameText(n->nested, symbols) : n->name == UINT32_MAX ? std::string() : std::string(symbols.name(n->name));
          collectOverrideKeys(n->decls, prefix + name + "::", symbols, out);
          break;
        }
        case NodeKind::FunctionDecl:
        {
          auto* f = static_cast<FunctionDecl*>(d);
          if (isFunctionDefinition(f))
          {
            out.functions.insert(prefix + nameText(f->name, symbols) + "/" + std::to_string(f->params.size()));
            ++out.functionCounts[prefix + nameText(f->name, symbols)];
          }
          break;
        }
        case NodeKind::ClassDecl:
        {
          auto* c = static_cast<ClassDecl*>(d);
          if (c->isDefinition && !c->name.empty())
            out.classes.insert(prefix + nameText(c->name, symbols));
          break;
        }
        case NodeKind::VariableDecl: out.variables.insert(prefix + nameText(static_cast<VariableDecl*>(d)->name, symbols)); break;
        default: break;
      }
  }

  struct OverrideFile
  {
    std::string text;
    OverrideKeys keys;
  };

  struct FileConverter
  {
    const ConvertInput& in;
    GlobalScope& global;
    const SymbolTable& symbols;
    const OverrideKeys* overrides = nullptr; // definitions a hand-written file replaces
    const CarriedDefaults* carriedDefaults = nullptr;
    std::set<std::string> replacedFunctions;  // qualified names whose definitions were found and removed
    std::vector<Edit> edits;
    Counts counts;
    bool importedCommon = false;    // stdlib.h, string.h and stdio.h added next to the first C import
    std::vector<std::string> todos; // "line: text"
    std::string_view text;
    const std::vector<Token>& toks;

    FileConverter(const ConvertInput& in, GlobalScope& global, const SymbolTable& symbols)
      : in(in), global(global), symbols(symbols), text(in.text), toks(*in.tokens)
    {
    }

    uint32_t lineOf(uint32_t offset) const
    {
      uint32_t line = 1;
      for (uint32_t i = 0; i < offset && i < text.size(); ++i)
        if (text[i] == '\n')
          ++line;
      return line;
    }

    void todo(uint32_t offset, const std::string& what)
    {
      todos.push_back(std::to_string(lineOf(offset)) + ": " + what);
      counts.add("todo");
    }

    void remove(uint32_t offset, uint32_t length) { edits.push_back({offset, length, ""}); }
    void insert(uint32_t offset, const std::string& s) { edits.push_back({offset, 0, s}); }
    void replace(uint32_t offset, uint32_t length, const std::string& s) { edits.push_back({offset, length, s}); }

    std::string_view spelling(uint32_t token) const
    {
      const Token& t = toks[token];
      return text.substr(t.offset, t.length);
    }
    uint32_t endOffset(uint32_t token) const { return toks[token].offset + toks[token].length; }

    // --- preprocessor lines ---

    struct Lines
    {
      std::vector<std::pair<uint32_t, uint32_t>> lines; // offset, length (without the newline)
    };

    // Evaluates an #if expression over the known symbols: defined(X), X, !, &&, ||, numbers, ==.
    // `known` becomes false when a symbol outside the two lists appears.
    struct IfEval
    {
      std::string s;
      size_t p = 0;
      bool known = true;
      void skipSpace()
      {
        while (p < s.size() && isspace((unsigned char)s[p]))
          ++p;
      }
      long long primary()
      {
        skipSpace();
        if (p < s.size() && s[p] == '!')
        {
          ++p;
          return !primary();
        }
        if (p < s.size() && s[p] == '(')
        {
          ++p;
          long long v = orExpr();
          skipSpace();
          if (p < s.size() && s[p] == ')')
            ++p;
          return v;
        }
        if (p < s.size() && isdigit((unsigned char)s[p]))
        {
          long long v = strtoll(s.c_str() + p, nullptr, 0);
          while (p < s.size() && isIdentChar(s[p]))
            ++p;
          return v;
        }
        size_t start = p;
        while (p < s.size() && isIdentChar(s[p]))
          ++p;
        std::string name = s.substr(start, p - start);
        if (name == "defined")
        {
          skipSpace();
          bool paren = p < s.size() && s[p] == '(';
          if (paren)
            ++p;
          skipSpace();
          size_t ns = p;
          while (p < s.size() && isIdentChar(s[p]))
            ++p;
          std::string sym = s.substr(ns, p - ns);
          skipSpace();
          if (paren && p < s.size() && s[p] == ')')
            ++p;
          return symbol(sym);
        }
        if (name.empty())
        {
          ++p;
          return 0;
        }
        return symbol(name);
      }
      long long symbol(const std::string& name)
      {
        if (definedSymbols.count(name))
          return 1;
        if (undefinedSymbols.count(name))
          return 0;
        known = false;
        return 0;
      }
      long long cmpExpr()
      {
        long long l = primary();
        skipSpace();
        if (s.compare(p, 2, "==") == 0)
        {
          p += 2;
          return l == primary();
        }
        if (s.compare(p, 2, "!=") == 0)
        {
          p += 2;
          return l != primary();
        }
        if (s.compare(p, 2, ">=") == 0)
        {
          p += 2;
          return l >= primary();
        }
        if (s.compare(p, 2, "<=") == 0)
        {
          p += 2;
          return l <= primary();
        }
        if (p < s.size() && s[p] == '>')
        {
          ++p;
          return l > primary();
        }
        if (p < s.size() && s[p] == '<')
        {
          ++p;
          return l < primary();
        }
        return l;
      }
      long long andExpr()
      {
        long long l = cmpExpr();
        for (;;)
        {
          skipSpace();
          if (s.compare(p, 2, "&&") != 0)
            return l;
          p += 2;
          long long r = cmpExpr();
          l = l && r;
        }
      }
      long long orExpr()
      {
        long long l = andExpr();
        for (;;)
        {
          skipSpace();
          if (s.compare(p, 2, "||") != 0)
            return l;
          p += 2;
          long long r = andExpr();
          l = l || r;
        }
      }
    };

    // Turns `#define NAME body` into a constant where the body is one, else a TODO comment.
    std::string macroToKov(const std::string& line, uint32_t offset)
    {
      size_t p = line.find("define") + 6;
      while (p < line.size() && isspace((unsigned char)line[p]))
        ++p;
      size_t nameStart = p;
      while (p < line.size() && isIdentChar(line[p]))
        ++p;
      std::string name = line.substr(nameStart, p - nameStart);
      bool functionLike = p < line.size() && line[p] == '(';
      std::string body = line.substr(p);
      size_t comment = body.find("//");
      std::string trailing;
      if (comment != std::string::npos)
      {
        trailing = " " + body.substr(comment);
        body = body.substr(0, comment);
      }
      while (!body.empty() && isspace((unsigned char)body.back()))
        body.pop_back();
      while (!body.empty() && isspace((unsigned char)body.front()))
        body.erase(body.begin());
      if (knownMacros.count(name) || knownFunctionMacros.count(name))
      {
        counts.add("macro known");
        return ""; // its uses are rewritten
      }
      if (!functionLike && !body.empty())
      {
        bool constant = true;
        int quotes = 0;
        for (char c : body)
        {
          if (c == '"')
            ++quotes;
          else if (!(isIdentChar(c) || strchr(" .+-*/()<>|&~!^,'\\xX", c)))
            constant = false;
        }
        if (constant && (quotes == 0 || quotes == 2))
        {
          counts.add("macro to constant");
          std::string indent = line.substr(0, line.find('#'));
          return indent + "const auto " + name + " = " + body + ";" + trailing;
        }
      }
      todo(offset, "macro " + name + (functionLike ? "(...)" : "") + " kept as a comment");
      counts.add("macro todo");
      std::string commented = "// TODO(kov) macro: ";
      for (char c : line)
      {
        commented += c;
        if (c == '\n')
          commented += "// ";
      }
      return commented;
    }

    void convertPreprocessor()
    {
      // Logical lines with continuations joined.
      std::vector<std::pair<uint32_t, uint32_t>> lines;
      size_t pos = 0;
      while (pos < text.size())
      {
        size_t end = text.find('\n', pos);
        if (end == std::string::npos)
          end = text.size();
        size_t start = pos;
        while (end < text.size())
        {
          size_t last = end;
          while (last > start && (text[last - 1] == '\r' || text[last - 1] == ' '))
            --last;
          if (last > start && text[last - 1] == '\\')
          {
            end = text.find('\n', end + 1);
            if (end == std::string::npos)
              end = text.size();
            continue;
          }
          break;
        }
        lines.push_back({uint32_t(start), uint32_t(end - start)});
        pos = end + 1;
      }
      struct Level
      {
        bool parentActive;
        bool taken;  // some branch was taken
        bool active; // this branch is active
        bool known;
      };
      std::vector<Level> stack;
      bool active = true;
      for (auto [offset, length] : lines)
      {
        std::string line(text.substr(offset, length));
        size_t p = 0;
        while (p < line.size() && isspace((unsigned char)line[p]))
          ++p;
        bool directive = p < line.size() && line[p] == '#';
        uint32_t removeLength = length + (offset + length < text.size() ? 1 : 0); // with its newline
        if (!directive)
        {
          if (!active)
          {
            remove(offset, removeLength);
            counts.add("lines dropped by #if");
          }
          continue;
        }
        ++p;
        while (p < line.size() && isspace((unsigned char)line[p]))
          ++p;
        size_t ws = p;
        while (p < line.size() && isalpha((unsigned char)line[p]))
          ++p;
        std::string word = line.substr(ws, p - ws);
        std::string rest = line.substr(p);
        size_t c = rest.find("//");
        if (c != std::string::npos)
          rest = rest.substr(0, c);
        for (char& ch : rest)
          if (ch == '\\' || ch == '\r')
            ch = ' ';
        auto evaluate = [&](const std::string& expr, bool& known)
        {
          IfEval e{expr};
          bool v = e.orExpr() != 0;
          known = e.known;
          return v;
        };
        if (word == "if" || word == "ifdef" || word == "ifndef")
        {
          bool known = true, value = false;
          if (word == "if")
            value = evaluate(rest, known);
          else
          {
            IfEval e{""};
            std::string sym;
            for (char ch : rest)
              if (isIdentChar(ch))
                sym += ch;
              else if (!sym.empty())
                break;
            value = e.symbol(sym) != 0;
            known = e.known;
            if (word == "ifndef")
              value = !value;
          }
          if (!known)
          {
            todo(offset, "#" + word + rest + " uses an unknown symbol; the first branch was kept");
            value = true;
          }
          stack.push_back({active, value, active && value, known});
          active = active && value;
          counts.add("#if");
        }
        else if (word == "elif" || word == "else")
        {
          if (stack.empty())
            continue;
          Level& l = stack.back();
          bool value = word == "else";
          if (word == "elif")
          {
            bool known = true;
            value = evaluate(rest, known);
            if (!known)
              value = false;
          }
          l.active = l.parentActive && !l.taken && value;
          if (l.active)
            l.taken = true;
          active = l.active;
        }
        else if (word == "endif")
        {
          if (!stack.empty())
          {
            active = stack.back().parentActive;
            stack.pop_back();
          }
        }
        else if (!active)
        {
          // a directive inside a dropped branch
        }
        else if (word == "include")
        {
          // C headers become imports; C++ and project headers go (every project symbol is visible).
          static const std::set<std::string> cHeaders = {"windows.h", "stdio.h", "stdlib.h", "string.h", "math.h", "ctype.h", "errno.h",
                                                         "stdarg.h", "stdint.h", "stddef.h", "time.h", "io.h", "fcntl.h", "direct.h",
                                                         "process.h", "signal.h", "locale.h", "wchar.h", "float.h", "limits.h", "assert.h",
                                                         "conio.h", "intrin.h", "malloc.h", "memory.h", "wctype.h", "inttypes.h",
                                                         "shellapi.h", "shlobj.h", "winsock2.h", "ws2tcpip.h", "psapi.h", "dbghelp.h"};
          size_t open = rest.find_first_of("<\"");
          size_t close = rest.find_first_of(">\"", open + 1);
          std::string header = open != std::string::npos && close != std::string::npos ? rest.substr(open + 1, close - open - 1) : "";
          std::string base = header.substr(header.find_last_of('/') + 1);
          static const std::map<std::string, std::string> cxxToC = {{"cmath", "math.h"}, {"cstdio", "stdio.h"}, {"cstring", "string.h"},
                                                                    {"cstdlib", "stdlib.h"}, {"cctype", "ctype.h"}, {"cerrno", "errno.h"},
                                                                    {"cstdarg", "stdarg.h"}, {"ctime", "time.h"}, {"cwchar", "wchar.h"},
                                                                    {"cfloat", "float.h"}, {"climits", "limits.h"}, {"cstdint", "stdint.h"},
                                                                    {"cstddef", "stddef.h"}, {"cassert", "assert.h"}, {"csignal", "signal.h"},
                                                                    {"clocale", "locale.h"}, {"cwctype", "wctype.h"}, {"cinttypes", "inttypes.h"}};
          auto cxx = cxxToC.find(base);
          if (cxx != cxxToC.end())
            base = cxx->second;
          // C libraries the game uses, by their include path prefix.
          static const char* cLibraries[] = {"SDL3/", "SDL2/", "SDL", "lua", "zlib", "zconf", "curl/", "png", "jpeg", "zstd", "opus", "ogg",
                                             "vorbis", "minizip", "stb_image", "fontstash"};
          bool cLibrary = false;
          for (const char* prefix : cLibraries)
            if (header.rfind(prefix, 0) == 0)
              cLibrary = true;
          if (cHeaders.count(base) || cLibrary)
          {
            std::string imports = "extern \"C\" import \"" + (cLibrary ? header : base) + "\";";
            if (!importedCommon)
            {
              // The C++ standard headers brought these in transitively; converted code expects them.
              importedCommon = true;
              for (const char* common : {"stdlib.h", "string.h", "stdio.h"})
                if (base != common)
                  imports += std::string("\nextern \"C\" import \"") + common + "\";";
            }
            replace(offset, length, imports);
            counts.add("includes to imports");
            continue;
          }
          counts.add("includes removed");
        }
        else if (word == "pragma" || word == "undef" || word == "error" || word == "warning")
          counts.add("#" + word + " removed");
        else if (word == "define")
        {
          std::string replacement = macroToKov(line, offset);
          if (!replacement.empty())
          {
            replace(offset, length, replacement);
            continue;
          }
        }
        remove(offset, removeLength);
      }
    }

    // --- token-level rewrites ---

    void convertTokens()
    {
      for (uint32_t i = 0; i + 2 < toks.size(); ++i)
      {
        const Token& t = toks[i];
        if (t.getKind() == TokenKind::Identifier && spelling(i) == "std" && toks[i + 1].is(P::ColonColon) &&
            toks[i + 2].getKind() == TokenKind::Identifier)
        {
          std::string name(spelling(i + 2));
          auto it = stdNames.find(name);
          std::string to = it == stdNames.end() ? name : it->second;
          if (it == stdNames.end())
            counts.add("std::" + name + " (no mapping)");
          else
            counts.add("std renamed");
          replace(t.offset, endOffset(i + 2) - t.offset, to);
          i += 2;
          continue;
        }
        if (t.is(Kw::typename_) && toks[i + 1].getKind() == TokenKind::Identifier && toks[i + 2].is(P::ColonColon))
        {
          remove(t.offset, toks[i + 1].offset - t.offset); // `typename T::x` -> `T::x`
          counts.add("typename dropped");
          continue;
        }
        if (t.is(Kw::template_) && i > 0 && (toks[i - 1].is(P::Dot) || toks[i - 1].is(P::Arrow) || toks[i - 1].is(P::ColonColon)))
        {
          remove(t.offset, toks[i + 1].offset - t.offset); // `x.template f<T>()` -> `x.f<T>()`
          counts.add("template keyword dropped");
          continue;
        }
        if (t.getKind() == TokenKind::Identifier && toks[i + 1].is(P::LParen))
        {
          // Library functions Factorio wraps that the C runtime provides as well.
          static const std::map<std::string_view, std::string> renames = {
            {"trio_vsnprintf", "vsnprintf"}, {"trio_snprintf", "snprintf"}, {"trio_sprintf", "sprintf"}, {"trio_vsprintf", "vsprintf"}};
          auto rn = renames.find(spelling(i));
          if (rn != renames.end())
          {
            replace(t.offset, t.length, rn->second);
            counts.add("function renamed");
            continue;
          }
        }
        if (t.getKind() == TokenKind::Identifier && spelling(i) == "length" && i > 0 && (toks[i - 1].is(P::Dot) || toks[i - 1].is(P::Arrow)) &&
            toks[i + 1].is(P::LParen) && toks[i + 2].is(P::RParen))
        {
          replace(t.offset, t.length, "size"); // `.length()` on strings: the library spells it size()
          counts.add("length() to size()");
          continue;
        }
        if (t.getKind() == TokenKind::Identifier)
        {
          std::string name(spelling(i));
          auto km = knownMacros.find(name);
          if (km != knownMacros.end())
          {
            replace(t.offset, t.length, km->second);
            counts.add("macro use rewritten");
            continue;
          }
          auto kf = knownFunctionMacros.find(name);
          if (kf != knownFunctionMacros.end() && toks[i + 1].is(P::LParen))
          {
            // `MACRO(arg)` -> arg (the only pattern in the table).
            int depth = 0;
            uint32_t j = i + 1;
            for (; j < toks.size(); ++j)
            {
              if (toks[j].is(P::LParen))
                ++depth;
              else if (toks[j].is(P::RParen) && --depth == 0)
                break;
            }
            if (j < toks.size())
            {
              remove(t.offset, toks[i + 2].offset - t.offset);
              remove(toks[j].offset, toks[j].length);
              counts.add("macro use rewritten");
            }
          }
        }
      }
    }

    // --- declaration-level rewrites ---

    static bool isDefinition(FunctionDecl* f) { return f->body != nullptr || (f->flags & (DF_Default | DF_Delete)); }

    // Whether a hand-written override replaces this namespace-scope definition.
    bool overridden(Decl* d, const std::string& prefix)
    {
      if (!overrides)
        return false;
      if (d->kind == NodeKind::FunctionDecl)
      {
        auto* f = static_cast<FunctionDecl*>(d);
        if (!isFunctionDefinition(f))
          return false;
        std::string q = prefix + nameText(f->name, symbols);
        std::string resolved = global.qualifiedName(f); // the namespace the resolver put it in
        size_t cut = resolved.rfind("::");
        std::string alt = (cut == std::string::npos ? std::string() : resolved.substr(0, cut + 2)) + nameText(f->name, symbols);
        for (const std::string& key : {q, alt})
          if (overrides->functions.count(key + "/" + std::to_string(f->params.size())) || overrides->functions.count(key + "/*"))
          {
            replacedFunctions.insert(key);
            return true;
          }
        for (const std::string& c : overrides->classes)
          if (q.size() > c.size() + 2 && q.compare(0, c.size(), c) == 0 && q.compare(c.size(), 2, "::") == 0)
            return true; // a member of a replaced class, defined out of line
        return false;
      }
      if (d->kind == NodeKind::ClassDecl)
      {
        auto* c = static_cast<ClassDecl*>(d);
        return c->isDefinition && !c->name.empty() && overrides->classes.count(prefix + nameText(c->name, symbols));
      }
      if (d->kind == NodeKind::VariableDecl)
        return overrides->variables.count(prefix + nameText(static_cast<VariableDecl*>(d)->name, symbols)) != 0;
      return false;
    }

    void convertDecls(Span<Decl*> decls, bool namespaceScope, const std::string& prefix = "")
    {
      for (Decl* d : decls)
      {
        if (namespaceScope && overridden(d, prefix))
        {
          removeDefinition(d);
          counts.add("replaced by the override file");
          continue;
        }
        if ((d->flags & DF_Friend) && (d->kind == NodeKind::ClassDecl || d->kind == NodeKind::FunctionDecl))
        {
          removeDecl(d); // no friends in Kov; `friend class X;` would read as a nested class
          counts.add("friend declarations removed");
          continue;
        }
        switch (d->kind)
        {
          case NodeKind::NamespaceDecl:
          {
            auto* n = static_cast<NamespaceDecl*>(d);
            std::string name = !n->nested.empty() ? nameText(n->nested, symbols) : n->name == UINT32_MAX ? std::string() : std::string(symbols.name(n->name));
            convertDecls(n->decls, true, prefix + name + "::");
            break;
          }
          case NodeKind::ClassDecl:
          {
            auto* c = static_cast<ClassDecl*>(d);
            if (!c->isDefinition)
            {
              if (namespaceScope)
              {
                removeDecl(d);
                counts.add("forward declarations removed");
              }
              break;
            }
            convertDecls(c->members, false, prefix + nameText(c->name, symbols) + "::");
            break;
          }
          case NodeKind::EnumDecl:
          {
            auto* e = static_cast<EnumDecl*>(d);
            if (!e->isDefinition && namespaceScope)
            {
              removeDecl(d);
              counts.add("forward declarations removed");
            }
            break;
          }
          case NodeKind::FunctionDecl:
          {
            auto* f = static_cast<FunctionDecl*>(d);
            if (f->templateParams.empty() && !f->body && !f->name.empty() && f->name.last().hasTemplateArgs && !(f->flags & DF_Specialization))
            {
              removeDecl(d); // an explicit instantiation: templates compile from source in Kov
              counts.add("explicit instantiations removed");
              break;
            }
            if (!isDefinition(f) && namespaceScope && !(f->flags & DF_ExternC))
            {
              Decl* def = global.definitionOf(f);
              if (def && def != f && def->kind == NodeKind::FunctionDecl && static_cast<FunctionDecl*>(def)->body)
              {
                removeDecl(d);
                counts.add("forward declarations removed");
                break;
              }
            }
            if (f->body && carriedDefaults)
              if (auto cd = carriedDefaults->find(f); cd != carriedDefaults->end())
                for (auto& [index, value] : cd->second)
                  if (index < f->params.size())
                    if (uint32_t end = parameterEndIn(toks, f->params[index]->token); end != UINT32_MAX)
                    {
                      insert(toks[end].offset, " = " + value);
                      counts.add("default arguments carried from declarations");
                    }
            if ((f->flags & DF_Constructor) && !(f->flags & (DF_Explicit | DF_Implicit)) && convertingConstructor(f))
            {
              insert(toks[f->token].offset, "implicit ");
              counts.add("implicit added");
            }
            if ((f->flags & DF_Conversion) && !(f->flags & (DF_Explicit | DF_Implicit)))
            {
              insert(toks[f->token].offset, "implicit ");
              counts.add("implicit added");
            }
            if (f->body)
            {
              Decl* owner = global.owningClass(f);
              walkStmt(f->body, owner);
            }
            for (const FunctionDecl::CtorInit& init : f->ctorInits)
              for (Expr* a : init.args)
                walkExpr(a, global.owningClass(f));
            break;
          }
          case NodeKind::VariableDecl:
          {
            auto* v = static_cast<VariableDecl*>(d);
            walkExpr(v->init, nullptr);
            break;
          }
          default: break;
        }
      }
    }

    static bool convertingConstructor(FunctionDecl* f)
    {
      if (f->params.empty())
        return false;
      auto* first = static_cast<ParamDecl*>(f->params[0]);
      if (first->isPack)
        return false;
      for (size_t i = 1; i < f->params.size(); ++i)
        if (!static_cast<ParamDecl*>(f->params[i])->defaultValue)
          return false;
      // Copy and move constructors are not conversions: `T(const T&)` has the class's own name.
      Type* t = first->type;
      while (t && (t->kind == NodeKind::LRefType || t->kind == NodeKind::RRefType || t->kind == NodeKind::ConstType))
        t = t->kind == NodeKind::LRefType ? static_cast<LRefType*>(t)->referenced
          : t->kind == NodeKind::RRefType ? static_cast<RRefType*>(t)->referenced
                                          : static_cast<ConstType*>(t)->inner;
      if (t && t->kind == NodeKind::NamedType && !f->name.empty() && f->name.parts.size() >= 1)
      {
        const QualifiedName& n = static_cast<NamedType*>(t)->name;
        if (!n.empty() && n.last().symbol == f->name.last().symbol)
          return false;
      }
      return true;
    }

    // Removes a whole definition (a function with its body, a class, a global): from the end of
    // whatever declaration precedes it, so that a return type or `template<...>` on an earlier
    // line goes with it.
    void removeDefinition(Decl* d)
    {
      uint32_t start = toks[d->token].offset;
      while (start > 0 && text[start - 1] != ';' && text[start - 1] != '{' && text[start - 1] != '}')
        --start;
      while (start < text.size() && (text[start] == '\n' || text[start] == '\r'))
        ++start;
      uint32_t end = d->endToken > d->token && d->endToken <= toks.size() ? endOffset(d->endToken - 1) : endOffset(d->token);
      // The rest of the line too when nothing else is on it (a `}` closing a namespace stays).
      uint32_t lineEnd = end;
      while (lineEnd < text.size() && (text[lineEnd] == ' ' || text[lineEnd] == '\t' || text[lineEnd] == '\r' || text[lineEnd] == ';'))
        ++lineEnd;
      if (lineEnd < text.size() && text[lineEnd] == '\n')
        end = lineEnd + 1;
      remove(start, end - start);
    }

    void removeDecl(Decl* d)
    {
      // The declaration's first token is its name; the type and specifiers before it on the same
      // line belong to it as well (forward declarations are one line).
      uint32_t start = toks[d->token].offset;
      while (start > 0 && text[start - 1] != '\n' && text[start - 1] != ';' && text[start - 1] != '{' && text[start - 1] != '}')
        --start;
      uint32_t end = d->endToken > d->token && d->endToken <= toks.size() ? endOffset(d->endToken - 1) : start;
      // Take the rest of the line too when nothing else is on it.
      uint32_t lineEnd = end;
      while (lineEnd < text.size() && (text[lineEnd] == ' ' || text[lineEnd] == '\t' || text[lineEnd] == '\r'))
        ++lineEnd;
      if (lineEnd < text.size() && text[lineEnd] == '\n')
        end = lineEnd + 1;
      remove(start, end - start);
    }

    // --- bodies: this-> and fallthrough ---

    // Non-static data members and methods of a class and its bases, by name.
    std::map<Decl*, std::set<uint32_t>> memberNameCache;
    const std::set<uint32_t>& memberNames(Decl* cls)
    {
      auto it = memberNameCache.find(cls);
      if (it != memberNameCache.end())
        return it->second;
      std::set<uint32_t>& names = memberNameCache[cls];
      std::function<void(Decl*, int)> collect = [&](Decl* d, int depth)
      {
        Decl* def = global.definitionOf(d);
        if (!def || def->kind != NodeKind::ClassDecl || depth > 8)
          return;
        auto* c = static_cast<ClassDecl*>(def);
        for (Decl* m : c->members)
        {
          if (m->flags & DF_Static)
            continue;
          if (m->kind == NodeKind::VariableDecl && static_cast<VariableDecl*>(m)->name.parts.size() == 1)
            names.insert(static_cast<VariableDecl*>(m)->name.parts[0].symbol);
          else if (m->kind == NodeKind::FunctionDecl && static_cast<FunctionDecl*>(m)->name.parts.size() == 1 &&
                   static_cast<FunctionDecl*>(m)->name.parts[0].special == 0)
            names.insert(static_cast<FunctionDecl*>(m)->name.parts[0].symbol);
        }
        for (const ClassDecl::Base& b : c->bases)
          if (b.type && b.type->kind == NodeKind::NamedType)
          {
            const Ty* bt = global.typeOfAst(b.type, c);
            if (bt && bt->decl)
              collect(bt->decl, depth + 1);
          }
      };
      collect(cls, 0);
      return names;
    }

    static bool terminates(Stmt* s)
    {
      if (!s)
        return false;
      switch (s->kind)
      {
        case NodeKind::ReturnStmt: case NodeKind::BreakStmt: case NodeKind::ContinueStmt: case NodeKind::GotoStmt:
        case NodeKind::FallthroughStmt:
          return true;
        case NodeKind::ExprStmt:
        {
          Expr* e = static_cast<ExprStmt*>(s)->expr;
          return e && e->kind == NodeKind::ThrowExpr;
        }
        case NodeKind::BlockStmt:
        {
          auto* b = static_cast<BlockStmt*>(s);
          return !b->stmts.empty() && terminates(b->stmts[b->stmts.size() - 1]);
        }
        case NodeKind::IfStmt:
        {
          auto* i = static_cast<IfStmt*>(s);
          return i->else_ && terminates(i->then) && terminates(i->else_);
        }
        default: return false;
      }
    }

    void walkSwitchBody(Stmt* body, Decl* owner)
    {
      if (!body || body->kind != NodeKind::BlockStmt)
      {
        walkStmt(body, owner);
        return;
      }
      auto* b = static_cast<BlockStmt*>(body);
      Stmt* last = nullptr; // last statement since the previous label
      for (Stmt* s : b->stmts)
      {
        if (s->kind == NodeKind::CaseStmt || s->kind == NodeKind::DefaultStmt)
        {
          if (last && !terminates(last))
          {
            // Indent like the label.
            uint32_t at = toks[s->token].offset;
            uint32_t lineStart = at;
            while (lineStart > 0 && text[lineStart - 1] != '\n')
              --lineStart;
            insert(at, "fallthrough;\n" + std::string(text.substr(lineStart, at - lineStart)));
            counts.add("fallthrough added");
          }
          last = nullptr;
          if (s->kind == NodeKind::CaseStmt)
            walkExpr(static_cast<CaseStmt*>(s)->value, owner);
          continue;
        }
        walkStmt(s, owner);
        last = s;
      }
    }

    void walkStmt(Stmt* s, Decl* owner)
    {
      if (!s)
        return;
      switch (s->kind)
      {
        case NodeKind::BlockStmt:
          for (Stmt* inner : static_cast<BlockStmt*>(s)->stmts)
            walkStmt(inner, owner);
          break;
        case NodeKind::ExprStmt: walkExpr(static_cast<ExprStmt*>(s)->expr, owner); break;
        case NodeKind::DeclStmt:
          for (Decl* d : static_cast<DeclStmt*>(s)->decls)
            if (d->kind == NodeKind::VariableDecl)
              walkExpr(static_cast<VariableDecl*>(d)->init, owner);
          break;
        case NodeKind::IfStmt:
        {
          auto* i = static_cast<IfStmt*>(s);
          walkStmt(i->init, owner);
          walkNode(i->cond, owner);
          walkStmt(i->then, owner);
          walkStmt(i->else_, owner);
          break;
        }
        case NodeKind::AssertStmt: walkExpr(static_cast<AssertStmt*>(s)->cond, owner); break;
        case NodeKind::ForStmt:
        {
          auto* f = static_cast<ForStmt*>(s);
          walkStmt(f->init, owner);
          walkNode(f->cond, owner);
          walkExpr(f->step, owner);
          walkStmt(f->body, owner);
          break;
        }
        case NodeKind::RangeForStmt:
        {
          auto* f = static_cast<RangeForStmt*>(s);
          walkExpr(f->range, owner);
          walkStmt(f->body, owner);
          break;
        }
        case NodeKind::WhileStmt:
          walkNode(static_cast<WhileStmt*>(s)->cond, owner);
          walkStmt(static_cast<WhileStmt*>(s)->body, owner);
          break;
        case NodeKind::DoStmt:
          walkStmt(static_cast<DoStmt*>(s)->body, owner);
          walkExpr(static_cast<DoStmt*>(s)->cond, owner);
          break;
        case NodeKind::SwitchStmt:
        {
          auto* sw = static_cast<SwitchStmt*>(s);
          walkStmt(sw->init, owner);
          walkNode(sw->cond, owner);
          walkSwitchBody(sw->body, owner);
          break;
        }
        case NodeKind::CaseStmt: walkExpr(static_cast<CaseStmt*>(s)->value, owner); break;
        case NodeKind::ReturnStmt: walkExpr(static_cast<ReturnStmt*>(s)->value, owner); break;
        case NodeKind::TryStmt:
        {
          auto* t = static_cast<TryStmt*>(s);
          walkStmt(t->body, owner);
          for (const TryStmt::Handler& h : t->handlers)
            walkStmt(h.body, owner);
          break;
        }
        default: break;
      }
    }

    void walkNode(Node* n, Decl* owner)
    {
      if (!n)
        return;
      if (n->kind == NodeKind::DeclStmt || n->kind == NodeKind::VariableDecl)
      {
        if (n->kind == NodeKind::VariableDecl)
          walkExpr(static_cast<VariableDecl*>(n)->init, owner);
        else
          walkStmt(static_cast<Stmt*>(n), owner);
        return;
      }
      walkExpr(static_cast<Expr*>(n), owner);
    }

    void walkExprs(Span<Expr*> es, Decl* owner)
    {
      for (Expr* e : es)
        walkExpr(e, owner);
    }

    void walkExpr(Expr* e, Decl* owner)
    {
      if (!e)
        return;
      switch (e->kind)
      {
        case NodeKind::NameExpr:
        {
          auto* n = static_cast<NameExpr*>(e);
          Decl* r = n->resolved;
          if (owner && n->name.parts.size() == 1 && !n->name.global && n->name.parts[0].special == 0)
          {
            bool member = false;
            if (r)
              member = r->parent && r->parent->kind == NodeKind::ClassDecl &&
                       (r->kind == NodeKind::VariableDecl || r->kind == NodeKind::FunctionDecl) && !(r->flags & DF_Static);
            else
              member = memberNames(owner).count(n->name.parts[0].symbol) != 0; // unresolved, but spelled like a member
            if (member)
            {
              insert(toks[n->token].offset, "this->");
              counts.add("this-> added");
            }
          }
          break;
        }
        case NodeKind::CallExpr:
          walkExpr(static_cast<CallExpr*>(e)->callee, owner);
          walkExprs(static_cast<CallExpr*>(e)->args, owner);
          break;
        case NodeKind::IndexExpr:
          walkExpr(static_cast<IndexExpr*>(e)->base, owner);
          walkExprs(static_cast<IndexExpr*>(e)->args, owner);
          break;
        case NodeKind::MemberExpr: walkExpr(static_cast<MemberExpr*>(e)->base, owner); break;
        case NodeKind::UnaryExpr: walkExpr(static_cast<UnaryExpr*>(e)->operand, owner); break;
        case NodeKind::BinaryExpr:
          walkExpr(static_cast<BinaryExpr*>(e)->lhs, owner);
          walkExpr(static_cast<BinaryExpr*>(e)->rhs, owner);
          break;
        case NodeKind::ConditionalExpr:
          walkExpr(static_cast<ConditionalExpr*>(e)->cond, owner);
          walkExpr(static_cast<ConditionalExpr*>(e)->then, owner);
          walkExpr(static_cast<ConditionalExpr*>(e)->else_, owner);
          break;
        case NodeKind::CastExpr: walkExpr(static_cast<CastExpr*>(e)->operand, owner); break;
        case NodeKind::NamedCastExpr: walkExpr(static_cast<NamedCastExpr*>(e)->operand, owner); break;
        case NodeKind::SizeofExpr: walkExpr(static_cast<SizeofExpr*>(e)->operand, owner); break;
        case NodeKind::NameofExpr: walkExpr(static_cast<NameofExpr*>(e)->operand, owner); break;
        case NodeKind::NewExpr:
          walkExprs(static_cast<NewExpr*>(e)->placement, owner);
          walkExprs(static_cast<NewExpr*>(e)->args, owner);
          break;
        case NodeKind::DeleteExpr: walkExpr(static_cast<DeleteExpr*>(e)->operand, owner); break;
        case NodeKind::LambdaExpr:
        {
          auto* l = static_cast<LambdaExpr*>(e);
          for (const LambdaCapture& c : l->captures)
            walkExpr(c.init, owner);
          walkStmt(l->body, owner);
          break;
        }
        case NodeKind::BraceInitExpr: walkExprs(static_cast<BraceInitExpr*>(e)->args, owner); break;
        case NodeKind::ThrowExpr: walkExpr(static_cast<ThrowExpr*>(e)->operand, owner); break;
        case NodeKind::FunctionalCastExpr: walkExprs(static_cast<FunctionalCastExpr*>(e)->args, owner); break;
        case NodeKind::FoldExpr:
          walkExpr(static_cast<FoldExpr*>(e)->lhs, owner);
          walkExpr(static_cast<FoldExpr*>(e)->rhs, owner);
          break;
        case NodeKind::ParenExpr: walkExpr(static_cast<ParenExpr*>(e)->inner, owner); break;
        default: break;
      }
    }

    // --- putting it together ---

    // Definitions of overridden functions the parser did not see as functions, found by text:
    // `... name(` at the start of a line with a `{` before the next `;`, through the body's end.
    void removeUnparsedDefinitions()
    {
      if (!overrides)
        return;
      for (const auto& [qualified, count] : overrides->functionCounts)
      {
        if (count != 1 || replacedFunctions.count(qualified))
          continue;
        std::string name = qualified.substr(qualified.rfind(':') == std::string::npos ? 0 : qualified.rfind(':') + 1);
        for (size_t p = text.find(name); p != std::string::npos; p = text.find(name, p + 1))
        {
          if (p > 0 && (isalnum((unsigned char)text[p - 1]) || text[p - 1] == '_' || text[p - 1] == ':' || text[p - 1] == '.'))
            continue;
          size_t q = p + name.size();
          while (q < text.size() && text[q] == ' ')
            ++q;
          if (q >= text.size() || text[q] != '(')
            continue;
          size_t lineStart = text.rfind('\n', p);
          lineStart = lineStart == std::string::npos ? 0 : lineStart + 1;
          std::string_view head = text.substr(lineStart, p - lineStart);
          if (head.find_first_not_of(" ") == std::string_view::npos || head.find(';') != std::string_view::npos || head.find("//") != std::string_view::npos)
            continue; // not a declaration line
          size_t brace = text.find('{', q), semi = text.find(';', q);
          if (brace == std::string::npos || (semi != std::string::npos && semi < brace))
            continue; // a prototype
          int depth = 0;
          size_t end = brace;
          for (; end < text.size(); ++end)
          {
            if (text[end] == '{')
              ++depth;
            else if (text[end] == '}' && --depth == 0)
              break;
          }
          if (end >= text.size())
            break;
          ++end;
          while (end < text.size() && (text[end] == ' ' || text[end] == '\r'))
            ++end;
          if (end < text.size() && text[end] == '\n')
            ++end;
          remove(uint32_t(lineStart), uint32_t(end - lineStart));
          counts.add("replaced by the override file (by text)");
          replacedFunctions.insert(qualified);
          break;
        }
      }
    }

    std::string run()
    {
      convertPreprocessor();
      convertTokens();
      convertDecls(in.parsed->decls, true);
      removeUnparsedDefinitions();
      // By offset; at the same offset the longest first, so that a removal cancels the edits inside it.
      std::stable_sort(edits.begin(), edits.end(),
                       [](const Edit& a, const Edit& b) { return a.offset != b.offset ? a.offset < b.offset : a.length > b.length; });
      std::string out;
      out.reserve(text.size() + 1024);
      uint32_t pos = 0;
      for (const Edit& e : edits)
      {
        if (e.offset < pos)
        {
          // Inside an earlier removal or replacement: an insertion is dropped, a longer removal extends.
          if (e.length && e.offset + e.length > pos)
          {
            pos = e.offset + e.length;
            out += e.text;
          }
          continue;
        }
        out.append(text.substr(pos, e.offset - pos));
        out += e.text;
        pos = e.offset + e.length;
      }
      out.append(text.substr(std::min<size_t>(pos, text.size())));
      // At most two consecutive blank lines where includes and directives used to be.
      std::string squeezed;
      squeezed.reserve(out.size());
      int blank = 0;
      size_t lineStart = 0;
      for (size_t i = 0; i <= out.size(); ++i)
      {
        if (i == out.size() || out[i] == '\n')
        {
          std::string_view line(out.data() + lineStart, i - lineStart);
          bool isBlank = line.find_first_not_of(" \t\r") == std::string_view::npos;
          blank = isBlank ? blank + 1 : 0;
          if (blank <= 2)
          {
            squeezed.append(line);
            if (i < out.size())
              squeezed += '\n';
          }
          lineStart = i + 1;
        }
      }
      // `std::name` in text the parser never tokenized (the other branch of an #if): the same
      // renames by text.
      for (size_t p = squeezed.find("std::"); p != std::string::npos; p = squeezed.find("std::", p + 1))
      {
        if (p > 0 && (isalnum((unsigned char)squeezed[p - 1]) || squeezed[p - 1] == '_'))
          continue;
        size_t e = p + 5;
        while (e < squeezed.size() && (isalnum((unsigned char)squeezed[e]) || squeezed[e] == '_'))
          ++e;
        auto it = stdNames.find(squeezed.substr(p + 5, e - p - 5));
        if (it != stdNames.end())
        {
          squeezed.replace(p, e - p, it->second);
          counts.add("std renamed");
        }
      }
      return squeezed;
    }
  };

  std::string relativeTo(const std::filesystem::path& p, const std::vector<std::filesystem::path>& roots)
  {
    std::error_code ec;
    std::filesystem::path abs = std::filesystem::absolute(p, ec).lexically_normal();
    for (const auto& root : roots)
    {
      std::filesystem::path r = std::filesystem::absolute(root, ec).lexically_normal();
      std::filesystem::path rel = abs.lexically_relative(r);
      if (!rel.empty() && rel.native()[0] != '.')
        return (r.filename() / rel).generic_string();
    }
    return abs.filename().generic_string();
  }
}

std::string convertToKov(const std::vector<ConvertInput>& inputs, GlobalScope& global, const SymbolTable& symbols,
                         const std::vector<std::filesystem::path>& roots, const std::filesystem::path& outDir,
                         const std::filesystem::path& overridesDir)
{
  struct Piece
  {
    std::string text;
    Counts counts;
    std::vector<std::string> todos;
    std::string source;
    bool header;
  };
  std::map<std::string, std::vector<Piece>> outputs; // output path -> header piece(s) then source piece(s)
  Counts total;
  // Hand-written override files, parsed once: what they define replaces the converted definitions.
  std::map<std::string, std::unique_ptr<OverrideFile>> overrideFiles;
  auto overrideFor = [&](const std::string& key) -> OverrideFile*
  {
    if (overridesDir.empty())
      return nullptr;
    auto it = overrideFiles.find(key);
    if (it != overrideFiles.end())
      return it->second.get();
    std::unique_ptr<OverrideFile>& slot = overrideFiles[key];
    std::error_code ec;
    std::filesystem::path p = overridesDir / key;
    if (!std::filesystem::exists(p, ec))
      return nullptr;
    slot = std::make_unique<OverrideFile>();
    if (FILE* f = fopen(p.string().c_str(), "rb"))
    {
      char buffer[65536];
      size_t n;
      while ((n = fread(buffer, 1, sizeof buffer, f)) > 0)
        slot->text.append(buffer, n);
      fclose(f);
    }
    SymbolTable localSymbols;
    LexResult lexed = lex(slot->text, localSymbols);
    ParsedFile parsed;
    parseFile(lexed.tokens, slot->text, localSymbols, parsed, false, nullptr);
    collectOverrideKeys(parsed.decls, "", localSymbols, slot->keys);
    // One definition of a name replaces every overload of it (the original's parameter list
    // may hide behind a macro); several definitions match by arity.
    for (const auto& [name, count] : slot->keys.functionCounts)
      if (count == 1)
        slot->keys.functions.insert(name + "/*");
    return slot.get();
  };
  // Default arguments live on the header declaration, which the conversion drops when the
  // definition is elsewhere: carry them to the definition's parameters.
  CarriedDefaults carriedDefaults;
  {
    std::map<ParsedFile*, const ConvertInput*> inputOf;
    for (const ConvertInput& in : inputs)
      if (in.parsed)
        inputOf[in.parsed] = &in;
    std::function<void(Span<Decl*>)> scan = [&](Span<Decl*> decls)
    {
      for (Decl* d : decls)
      {
        if (d->kind == NodeKind::NamespaceDecl)
        {
          scan(static_cast<NamespaceDecl*>(d)->decls);
          continue;
        }
        if (d->kind != NodeKind::FunctionDecl)
          continue;
        auto* f = static_cast<FunctionDecl*>(d);
        if (f->body || (f->flags & (DF_Default | DF_Delete)))
          continue;
        bool any = false;
        for (Decl* p : f->params)
          any = any || static_cast<ParamDecl*>(p)->defaultValue;
        if (!any)
          continue;
        Decl* defDecl = global.definitionOf(f);
        if (!defDecl || defDecl == f || defDecl->kind != NodeKind::FunctionDecl)
          continue;
        auto* def = static_cast<FunctionDecl*>(defDecl);
        if (!def->body || def->params.size() != f->params.size())
          continue;
        auto it = inputOf.find(f->file);
        if (it == inputOf.end() || !it->second->tokens)
          continue;
        const ConvertInput& in = *it->second;
        for (size_t i = 0; i < f->params.size(); ++i)
        {
          Expr* dv = static_cast<ParamDecl*>(f->params[i])->defaultValue;
          if (!dv || static_cast<ParamDecl*>(def->params[i])->defaultValue)
            continue;
          uint32_t end = parameterEndIn(*in.tokens, dv->token);
          if (end == UINT32_MAX || end <= dv->token)
            continue;
          uint32_t from = (*in.tokens)[dv->token].offset;
          uint32_t to = (*in.tokens)[end - 1].offset + (*in.tokens)[end - 1].length;
          carriedDefaults[def].push_back({i, std::string(in.text.substr(from, to - from))});
        }
      }
    };
    for (const ConvertInput& in : inputs)
      if (in.parsed && !in.parsed->isImported)
        scan(in.parsed->decls);
  }
  for (const ConvertInput& in : inputs)
  {
    if (!in.parsed || in.parsed->isImported || in.path.string().rfind("<", 0) == 0)
      continue;
    std::string ext = in.path.extension().string();
    if (ext != ".cpp" && ext != ".hpp" && ext != ".h" && ext != ".cc" && ext != ".inl")
      continue;
    std::string rel = relativeTo(in.path, roots);
    std::string key = rel.substr(0, rel.size() - ext.size()) + ".kov";
    FileConverter fc(in, global, symbols);
    fc.carriedDefaults = &carriedDefaults;
    if (OverrideFile* ov = overrideFor(key))
      fc.overrides = &ov->keys;
    Piece piece;
    piece.text = fc.run();
    piece.counts = fc.counts;
    piece.todos = fc.todos;
    piece.source = in.path.string();
    piece.header = ext != ".cpp" && ext != ".cc";
    outputs[key].push_back(std::move(piece));
    for (auto& [k, v] : fc.counts.values)
      total.add(k, v);
  }
  std::string report = "# Conversion report\n\n";
  size_t files = 0;
  for (auto& [key, pieces] : outputs)
  {
    std::stable_sort(pieces.begin(), pieces.end(), [](const Piece& a, const Piece& b) { return a.header && !b.header; });
    std::filesystem::path outPath = outDir / key;
    std::error_code ec;
    std::filesystem::create_directories(outPath.parent_path(), ec);
    OverrideFile* ov = overrideFor(key);
    std::string text = "// Converted from";
    for (const Piece& p : pieces)
      text += " " + std::filesystem::path(p.source).filename().string();
    text += " by kovc --convert; see CONVERSION.md.\n\n";
    for (const Piece& p : pieces)
    {
      text += p.text;
      if (text.empty() || text.back() != '\n')
        text += '\n';
      text += '\n';
    }
    if (ov)
    {
      // The hand-written part: appended, its definitions replaced the converted ones above.
      text += "// --- Hand-written, from " + (overridesDir / key).generic_string() + " ---\n\n" + ov->text;
      if (text.back() != '\n')
        text += '\n';
      total.add("files with overrides");
    }
    FILE* f = fopen(outPath.string().c_str(), "wb");
    if (f)
    {
      fwrite(text.data(), 1, text.size(), f);
      fclose(f);
      ++files;
    }
    report += "## " + key + "\n";
    for (const Piece& p : pieces)
    {
      report += "- " + std::filesystem::path(p.source).filename().string() + ":";
      for (auto& [k, v] : p.counts.values)
        report += " " + k + " " + std::to_string(v) + ";";
      report += "\n";
      for (const std::string& t : p.todos)
        report += "  - TODO " + t + "\n";
    }
  }
  std::string summary = "Files written: " + std::to_string(files) + "\n\n";
  for (auto& [k, v] : total.values)
    summary += "- " + k + ": " + std::to_string(v) + "\n";
  report.insert(strlen("# Conversion report\n\n"), summary + "\n");
  std::filesystem::path reportPath = outDir / "CONVERSION.md";
  FILE* f = fopen(reportPath.string().c_str(), "wb");
  if (f)
  {
    fwrite(report.data(), 1, report.size(), f);
    fclose(f);
  }
  return summary;
}
