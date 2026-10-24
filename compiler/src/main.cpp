// kovc: the Kov compiler. Currently the speed-test prototype: parallel lexing of a file tree.
#include "lexer.hpp"
#include "codegen.hpp"
#include "convert.hpp"
#include "parser.hpp"
#include "resolver.hpp"
#include "source.hpp"
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstring>
#include <exception>
#include <set>
#include <cstdarg>
#include <cstdlib>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

namespace
{
  bool quietOutput = false;
  int report(const char* format, ...)
  {
    if (quietOutput)
      return 0;
    va_list args;
    va_start(args, format);
    int n = vprintf(format, args);
    va_end(args);
    return n;
  }

  struct Options
  {
    std::vector<std::filesystem::path> roots;
    std::vector<std::string> extensions = {".cpp", ".hpp", ".h", ".kov"};
    unsigned threads = std::thread::hardware_concurrency();
    bool dumpTokens = false;
    bool parse = true;
    int showErrors = 20;
    int showUnresolved = 25;
    bool quiet = false;
    std::string emitLLVM; // --emit-llvm <path>
    std::string exe;      // --exe <path>
    std::string stdDir;   // --std <dir>; default: <kovc dir>\..\..\stdlib when it exists
    bool noStd = false;
    std::vector<std::string> cIncludeDirs; // --cinclude DIR, for imported C headers
    std::vector<std::string> libs;         // --lib FILE: libraries linked into the executable
    bool showImportErrors = false; // --errors given: list parse errors in imported headers too
    ConfigValues configValues;     // --config name=value, --release; becomes the `config` namespace
    bool debug = true;             // config::debug
    unsigned modules = 0;          // --modules N: IR modules compiled in parallel (0: 2 per thread, 1: one module)
    std::string convertDir;        // --convert DIR: write the inputs as Kov sources there
    std::string overridesDir;      // --overrides DIR: hand-written files that replace converted ones
    int repeat = 1;
  };

  void usage()
  {
    fprintf(stderr,
            "usage: kovc [options] <file-or-dir>...\n"
            "  -j N          worker threads (default: hardware threads)\n"
            "  --ext .x,.y   file extensions to pick up (default .cpp,.hpp,.h,.kov)\n"
            "  --repeat N    lex the corpus N times, report the best time\n"
            "  --dump        print tokens\n"
            "  --lex-only    skip parsing\n"
            "  --errors N    show the first N parse errors (default 20)\n"
            "  --unresolved N show the N most common unresolved names (default 25)\n"
            "  --quiet       no statistics\n"
            "  --emit-llvm F write LLVM IR for the program to F\n"
            "  --exe F       compile the program to the executable F (needs clang)\n"
            "  --std DIR     standard library sources (default: stdlib next to the compiler)\n"
            "  --no-std      compile without the standard library\n"
            "  --cinclude D  directory searched by imported C headers (repeatable)\n"
            "  --lib F       a library (.lib) linked into the executable (repeatable)\n"
            "  --config N=V  build configuration constant config::N (bool, integer, enumerator or string)\n"
            "  --release     the same as --config debug=false\n"
            "  --modules N   split the program into N IR modules compiled in parallel (default: two per thread)\n"
            "  --convert D   rewrite the C++ inputs as Kov sources under D (see docs/path-to-factorio.md)\n");
  }

  struct FileStats
  {
    uint64_t bytes = 0;
    uint64_t tokens = 0;
    uint64_t lines = 0;
    uint64_t errors = 0;
  };

  template<typename F>
  void parallelFor(size_t count, unsigned threads, F&& body)
  {
    std::atomic<size_t> next{0};
    auto worker = [&]
    {
      for (;;)
      {
        size_t i = next.fetch_add(1, std::memory_order_relaxed);
        if (i >= count)
          return;
        body(i);
      }
    };
    std::vector<std::thread> pool;
    for (unsigned t = 1; t < threads; ++t)
      pool.emplace_back(worker);
    worker();
    for (auto& t : pool)
      t.join();
  }

  double seconds(std::chrono::steady_clock::duration d);
  int codegenExit = 0;

  // What the executable links against: legacy_stdio_definitions (the printf family the UCRT
  // headers define inline), the Windows libraries programs commonly reach, --lib files; unused
  // functions go (/OPT:REF).
  std::string linkFlags(const Options& options)
  {
    std::string flags = "-Wl,/OPT:REF -llegacy_stdio_definitions -luser32 -ladvapi32 -lshell32 -lole32 -lws2_32";
    for (const std::string& lib : options.libs)
      flags += " \"" + lib + "\"";
    return flags;
  }

  // Splits a flag string on spaces, keeping quoted paths whole (without the quotes).
  std::vector<std::string> splitFlags(const std::string& text)
  {
    std::vector<std::string> out;
    std::string current;
    bool quoted = false;
    for (char c : text)
    {
      if (c == '"')
        quoted = !quoted;
      else if (c == ' ' && !quoted)
      {
        if (!current.empty())
          out.push_back(current);
        current.clear();
      }
      else
        current += c;
    }
    if (!current.empty())
      out.push_back(current);
    return out;
  }
  void importCHeaders(std::vector<SourceFile>& files, std::vector<LexResult>& lexed, std::vector<std::unique_ptr<ParsedFile>>& parsed,
                      SymbolTable& symbols, const Options& options);

  // A --config value: true/false, an integer, or a word (an enumerator such as Windows, or a string).
  ConfigValue parseConfigValue(const std::string& text)
  {
    ConfigValue v;
    v.text = text;
    if (text == "true" || text == "false")
    {
      v.number = text == "true";
      return v;
    }
    char* end = nullptr;
    long long n = strtoll(text.c_str(), &end, 0);
    if (end && *end == 0 && !text.empty())
    {
      v.number = n;
      return v;
    }
    v.isNumber = false;
    return v;
  }

  // The `config` namespace as Kov source, one constant per --config value.
  std::string configSource(const ConfigValues& values)
  {
    std::string s = "// Build configuration, generated by kovc from --config name=value and --release.\n"
                    "enum class Platform { Windows, Linux, Mac };\n"
                    "namespace config\n{\n";
    for (const auto& [name, value] : values)
    {
      if (name == "platform")
        s += "  const Platform platform = Platform::" + value.text + ";\n";
      else if (!value.isNumber)
        s += "  const char* " + name + " = \"" + value.text + "\";\n";
      else if (value.text == "true" || value.text == "false")
        s += "  const bool " + name + " = " + value.text + ";\n";
      else
        s += "  const int " + name + " = " + std::to_string(value.number) + ";\n";
    }
    return s + "}\n";
  }

  std::string clangPath()
  {
    const char* clangEnv = getenv("KOV_CLANG");
    return clangEnv ? clangEnv : "C:\\Program Files\\Microsoft Visual Studio\\2022\\Community\\VC\\Tools\\Llvm\\x64\\bin\\clang.exe";
  }

  std::string readWholeFile(const std::filesystem::path& path)
  {
    std::string text;
    readFile(path, text);
    return text;
  }

  // Turns `#define NAME body` lines (clang -dM output) into Kov constant declarations where the
  // body looks like a constant: numbers, string literals, operators, parentheses and names.
  std::string macroConstants(const std::string& defines)
  {
    std::string out = "\n// Object-like macros, as constants.\n";
    // Names of object-like macros with a body: a body may refer to them (INFINITY uses _HUGE_ENUF).
    std::set<std::string> candidates;
    {
      size_t p = 0;
      while (p < defines.size())
      {
        size_t end = defines.find('\n', p);
        if (end == std::string::npos)
          end = defines.size();
        std::string line = defines.substr(p, end - p);
        p = end + 1;
        if (line.rfind("#define ", 0) != 0)
          continue;
        size_t nameEnd = line.find_first_of(" (", 8);
        if (nameEnd == std::string::npos || line[nameEnd] == '(' || nameEnd + 1 >= line.size())
          continue;
        candidates.insert(line.substr(8, nameEnd - 8));
      }
    }
    static const std::set<std::string> typeWords = {"float", "double", "int", "long", "short", "char", "unsigned", "signed", "const",
                                                    "sizeof", "size_t", "int8_t", "int16_t", "int32_t", "int64_t", "uint8_t", "uint16_t",
                                                    "uint32_t", "uint64_t", "intptr_t", "uintptr_t", "wchar_t", "bool", "true", "false",
                                                    "DWORD", "WORD", "BYTE", "HANDLE", "LONG", "ULONG", "UINT", "INT", "LONG_PTR", "ULONG_PTR",
                                                    "LPVOID", "SIZE_T", "DWORD_PTR", "HRESULT", "BOOL", "LONGLONG", "ULONGLONG", "UINT_PTR", "INT_PTR"};
    std::map<std::string, std::string> integerTypes; // constants with a plain integer body, by name
    for (size_t p = 0; p < defines.size();)
    {
      size_t end = defines.find('\n', p);
      if (end == std::string::npos)
        end = defines.size();
      std::string line = defines.substr(p, end - p);
      p = end + 1;
      if (line.rfind("#define ", 0) != 0)
        continue;
      size_t nameEnd = line.find_first_of(" (", 8);
      if (nameEnd == std::string::npos || line[nameEnd] == '(' || nameEnd + 1 >= line.size())
        continue;
      std::string body = line.substr(nameEnd + 1);
      while (!body.empty() && (body.back() == ' ' || body.back() == '\r'))
        body.pop_back();
      if (body.empty() || !isdigit((unsigned char)body[0]) || body.find_first_not_of("0123456789xXabcdefABCDEFuUlLi") != std::string::npos ||
          (body.find('i') != std::string::npos && body.find("i64") == std::string::npos && body.find("i32") == std::string::npos) ||
          (body.find_first_of("xX") == std::string::npos && body.find_first_of("abcdefABCDEF") != std::string::npos))
        continue;
      bool wide = body.find_first_of("lL") != std::string::npos || body.find("i64") != std::string::npos;
      bool isUnsigned = body.find_first_of("uU") != std::string::npos;
      integerTypes[line.substr(8, nameEnd - 8)] = wide ? (isUnsigned ? "unsigned long long" : "long long") : (isUnsigned ? "unsigned int" : "int");
    }
    size_t pos = 0;
    while (pos < defines.size())
    {
      size_t end = defines.find('\n', pos);
      if (end == std::string::npos)
        end = defines.size();
      std::string line = defines.substr(pos, end - pos);
      pos = end + 1;
      if (line.rfind("#define ", 0) != 0)
        continue;
      size_t nameStart = 8;
      size_t nameEnd = line.find_first_of(" (", nameStart);
      if (nameEnd == std::string::npos || line[nameEnd] == '(')
        continue; // function-like or empty
      std::string name = line.substr(nameStart, nameEnd - nameStart);
      std::string body = line.substr(nameEnd + 1);
      while (!body.empty() && (body.back() == ' ' || body.back() == '\r'))
        body.pop_back();
      bool internal = name.rfind("__", 0) == 0; // __FILE__-style internals stay out, unless a plain number (__LONG_LONG_MAX__)
      if (name.empty() || body.empty() || (internal && body.find_first_not_of("0123456789xXabcdefABCDEFuUlL") != std::string::npos))
        continue;
      bool hasNumber = false, ok = true, call = false, namesKnown = true;
      int quotes = 0;
      for (size_t i = 0; i < body.size(); ++i)
      {
        char c = body[i];
        bool prevIdent = i > 0 && (isalnum((unsigned char)body[i - 1]) || body[i - 1] == '_');
        if (c == '"')
          ++quotes;
        else if (isdigit((unsigned char)c))
          hasNumber = hasNumber || !prevIdent; // a digit inside a name (_SAL1_1_) is not a number
        else if (!(isalnum((unsigned char)c) || strchr("_ .+-*/()<>|&~!^,'\\xX", c)))
          ok = false;
        if ((isalpha((unsigned char)c) || c == '_') && !prevIdent && quotes % 2 == 0)
        {
          // An identifier: it must be a type word or another constant macro for the body to be a constant.
          size_t j = i;
          while (j < body.size() && (isalnum((unsigned char)body[j]) || body[j] == '_'))
            ++j;
          std::string word = body.substr(i, j - i);
          if (!typeWords.count(word) && !candidates.count(word))
            namesKnown = false;
        }
        if (c == '(')
        {
          size_t j = i;
          while (j > 0 && body[j - 1] == ' ')
            --j;
          if (j > 0 && (isalnum((unsigned char)body[j - 1]) || body[j - 1] == '_'))
            call = true; // `name(`: a function-like macro or a call, not a constant; `)(` in casts is fine
        }
      }
      bool aliasOfInteger = integerTypes.count(body) != 0; // DBL_MANT_DIG = __DBL_MANT_DIG__
      if (!ok || call || (quotes != 0 && quotes != 2) || !namesKnown ||
          (!hasNumber && quotes == 0 && body.find('(') == std::string::npos && !aliasOfInteger))
        continue;
      // A declared type where the body shows one: a cast, a string, a float or an integer literal.
      std::string type = "auto";
      if (body.rfind("((", 0) == 0 && body.find(')') != std::string::npos)
      {
        std::string cast = body.substr(2, body.find(')') - 2);
        bool word = !cast.empty();
        for (char ch : cast)
          if (!(isalnum((unsigned char)ch) || ch == '_' || ch == ' '))
            word = false;
        if (word && cast.find(' ') == std::string::npos)
          type = cast;
      }
      else if (quotes == 2)
        type = "char*";
      else if (hasNumber && body.find_first_of("\"") == std::string::npos && body.find_first_of("xX") == std::string::npos &&
               body.find_first_of(".eE") != std::string::npos)
        type = body.back() == 'f' || body.back() == 'F' ? "float" : "double";
      else if (hasNumber && body.find_first_not_of("0123456789xXabcdefABCDEFuUlLi()- +") == std::string::npos &&
               (body.find('i') == std::string::npos || body.find("i64") != std::string::npos || body.find("i32") != std::string::npos))
      {
        bool wide = body.find_first_of("lL") != std::string::npos || body.find("i64") != std::string::npos;
        bool isUnsigned = body.find_first_of("uU") != std::string::npos;
        type = wide ? (isUnsigned ? "unsigned long long" : "long long") : (isUnsigned ? "unsigned int" : "int");
      }
      if (type == "auto" && aliasOfInteger)
        type = integerTypes[body];
      if (type == "auto" && hasNumber && quotes == 0)
      {
        // Names of other integer constants with numbers and operators (LLONG_MIN = (-__LONG_LONG_MAX__-1LL)):
        // an integer of the widest kind seen.
        bool integer = true, wide = false, isUnsigned = false;
        for (size_t i = 0; i < body.size() && integer;)
        {
          char c = body[i];
          if (isalpha((unsigned char)c) || c == '_')
          {
            size_t j = i;
            while (j < body.size() && (isalnum((unsigned char)body[j]) || body[j] == '_'))
              ++j;
            std::string word = body.substr(i, j - i);
            auto known = integerTypes.find(word);
            bool suffix = i > 0 && isdigit((unsigned char)body[i - 1]) && word.find_first_not_of("uUlLi0123456789") == std::string::npos;
            if (known != integerTypes.end())
            {
              wide = wide || known->second.find("long long") != std::string::npos;
              isUnsigned = isUnsigned || known->second.find("unsigned") != std::string::npos;
            }
            else if (suffix)
            {
              wide = wide || word.find_first_of("lL") != std::string::npos || word.find("i64") != std::string::npos;
              isUnsigned = isUnsigned || word.find_first_of("uU") != std::string::npos;
            }
            else
              integer = false;
            i = j;
            continue;
          }
          if (c == '.' || c == '"' || c == '\'')
            integer = false;
          ++i;
        }
        if (integer)
          type = wide ? (isUnsigned ? "unsigned long long" : "long long") : (isUnsigned ? "unsigned int" : "int");
      }
      if (type.find("int") != std::string::npos || type.find("long") != std::string::npos)
        integerTypes[name] = type;
      out += "const " + type + " " + name + " = " + body + ";\n";
    }
    return out;
  }

  // Collects `extern "C" import "header";` declarations, preprocesses the headers with clang
  // once (cached by content hash in the temp directory) and parses the result as one more file.
  void importCHeaders(std::vector<SourceFile>& files, std::vector<LexResult>& lexed, std::vector<std::unique_ptr<ParsedFile>>& parsed,
                      SymbolTable& symbols, const Options& options)
  {
    std::vector<std::string> headers;
    for (size_t i = 0; i < parsed.size(); ++i)
      for (Decl* d : parsed[i]->decls)
        if (d->kind == NodeKind::ImportDecl)
        {
          const Token& t = lexed[i].tokens[static_cast<ImportDecl*>(d)->pathToken];
          std::string path = files[i].text.substr(t.offset + 1, t.length - 2);
          if (std::find(headers.begin(), headers.end(), path) == headers.end())
            headers.push_back(path);
        }
    if (headers.empty())
      return;
    auto importStart = std::chrono::steady_clock::now();
    std::string wrapper = "#define WIN32_LEAN_AND_MEAN\n#define NOMINMAX\n#define _USE_MATH_DEFINES\n";
    for (const std::string& h : headers)
      wrapper += "#include <" + h + ">\n";
    std::string includeArgs;
    for (const std::string& dir : options.cIncludeDirs)
      includeArgs += " -I\"" + dir + "\"";
    uint64_t hash = 1469598103934665603ull;
    for (unsigned char c : wrapper + includeArgs)
      hash = (hash ^ c) * 1099511628211ull;
    std::error_code ec;
    std::filesystem::path cacheDir = std::filesystem::temp_directory_path(ec) / "kov-imports";
    std::filesystem::create_directories(cacheDir, ec);
    char hex[32];
    snprintf(hex, sizeof hex, "%016llx", (unsigned long long)hash);
    std::filesystem::path cached = cacheDir / (std::string(hex) + ".i");
    std::string text;
    bool cacheHit = std::filesystem::exists(cached, ec);
    if (!cacheHit)
    {
      std::filesystem::path wrapperPath = cacheDir / (std::string(hex) + ".c");
      std::filesystem::path preprocessed = cacheDir / (std::string(hex) + ".pp");
      std::filesystem::path defines = cacheDir / (std::string(hex) + ".dm");
      FILE* w = fopen(wrapperPath.string().c_str(), "wb");
      if (!w)
      {
        fprintf(stderr, "cannot write %s\n", wrapperPath.string().c_str());
        return;
      }
      fwrite(wrapper.data(), 1, wrapper.size(), w);
      fclose(w);
      std::string clang = clangPath();
      std::string base = "\"\"" + clang + "\" -x c -w" + includeArgs + " \"" + wrapperPath.string() + "\"";
      int rc = system((base + " -E -P -o \"" + preprocessed.string() + "\"\"").c_str());
      int rc2 = system((base + " -E -dM -o \"" + defines.string() + "\"\"").c_str());
      if (rc != 0 || rc2 != 0)
      {
        fprintf(stderr, "importing C headers failed (clang exit %d/%d)\n", rc, rc2);
        codegenExit = 1;
        return;
      }
      text = readWholeFile(preprocessed) + macroConstants(readWholeFile(defines));
      FILE* out = fopen(cached.string().c_str(), "wb");
      if (out)
      {
        fwrite(text.data(), 1, text.size(), out);
        fclose(out);
      }
    }
    else
      text = readWholeFile(cached);
    // The imported declarations become one more file, parsed in C mode.
    SourceFile source;
    source.path = "<imported C headers>";
    source.text = std::move(text);
    source.text.reserve(source.text.size() + 1);
    files.push_back(std::move(source));
    lexed.push_back(lex(files.back().text, symbols));
    parsed.push_back(std::make_unique<ParsedFile>());
    parsed.back()->path = files.back().path.string();
    parsed.back()->isImported = true;
    parseFile(lexed.back().tokens, files.back().text, symbols, *parsed.back(), true, nullptr);
    for (size_t i = 0; i < parsed.size(); ++i)
    {
      // The vectors may have grown: re-point the parsed files at their tokens and text.
      parsed[i]->tokens = &lexed[i].tokens;
      parsed[i]->text = files[i].text;
    }
    report("imports: %zu header(s) -> %zu declarations, %zu parse errors (ignored), %.3f s%s\n", headers.size(),
           size_t(parsed.back()->declCount), parsed.back()->diagnostics.size(), seconds(std::chrono::steady_clock::now() - importStart),
           cacheHit ? " (cached)" : "");
  }

  void runCodegen(std::vector<std::unique_ptr<ParsedFile>>& parsed, GlobalScope& global, SymbolTable& symbols, const Options& options)
  {
    using Clock = std::chrono::steady_clock;
    // --emit-llvm wants the whole program in one file; an executable is built from several modules
    // compiled in parallel (one per group of files), unless --modules 1.
    unsigned modules = options.modules ? options.modules : unsigned(std::min<size_t>(parsed.size(), 2 * std::max(1u, options.threads)));
    if (!options.emitLLVM.empty())
      modules = 1;
    auto start = Clock::now();
    CodegenResult cg = generateLLVM(parsed, global, symbols, options.debug, modules);
    double t = seconds(Clock::now() - start);
    size_t irBytes = cg.ir.size();
    for (const std::string& m : cg.modules)
      irBytes += m.size();
    report("codegen: %.3f s, %zu functions, %zu structs, %zu bytes of IR in %zu module(s)\n", t, cg.functions, cg.structs, irBytes,
           std::max<size_t>(1, cg.modules.size()));
    for (const std::string& e : cg.errors)
      fprintf(stderr, "%s\n", e.c_str());
    if (!cg.errors.empty())
      codegenExit = 1;
    auto writeFile = [&](const std::string& path, const std::string& text)
    {
      FILE* out = fopen(path.c_str(), "wb");
      if (!out)
      {
        fprintf(stderr, "cannot write %s\n", path.c_str());
        codegenExit = 1;
        return false;
      }
      fwrite(text.data(), 1, text.size(), out);
      fclose(out);
      return true;
    };
    std::string clang = clangPath();
    const char* level = options.debug ? "-O0" : "-O2";
    // Every function in its own section, so that the linker drops the ones nobody calls (and
    // the libraries they would need): a program pays only for what it uses.
    const char* compileFlags = "-ffunction-sections -fdata-sections";
    if (cg.modules.empty())
    {
      std::string llPath = options.emitLLVM.empty() ? options.exe + ".ll" : options.emitLLVM;
      if (!writeFile(llPath, cg.ir) || options.exe.empty() || codegenExit)
        return;
      std::string command = "\"\"" + clang + "\" " + level + " -w " + compileFlags + " \"" + llPath + "\" -o \"" + options.exe + "\" " + linkFlags(options) + "\"";
      start = Clock::now();
      int rc = system(command.c_str());
      report("clang:   %.3f s (%s, exit %d)\n", seconds(Clock::now() - start), level, rc);
      if (rc != 0)
        codegenExit = 1;
      return;
    }
    if (codegenExit)
      return;
    std::vector<std::string> objects(cg.modules.size());
    std::atomic<int> failures{0}, cached{0};
    start = Clock::now();
    parallelFor(cg.modules.size(), options.threads, [&](size_t k)
    {
      std::string llPath = options.exe + "." + std::to_string(k) + ".ll";
      objects[k] = options.exe + "." + std::to_string(k) + ".o";
      // A module whose text (and optimisation level) did not change keeps its object file.
      uint64_t h = 1469598103934665603ull;
      for (unsigned char c : cg.modules[k])
        h = (h ^ c) * 1099511628211ull;
      for (const char* c = level; *c; ++c)
        h = (h ^ (unsigned char)*c) * 1099511628211ull;
      std::string hashText = std::to_string(h);
      std::string hashPath = options.exe + "." + std::to_string(k) + ".hash";
      std::error_code ec;
      if (readWholeFile(hashPath) == hashText && std::filesystem::exists(objects[k], ec))
      {
        ++cached;
        return;
      }
      if (!writeFile(llPath, cg.modules[k]))
      {
        ++failures;
        return;
      }
      std::string command = "\"\"" + clang + "\" " + level + " -w " + compileFlags + " -c \"" + llPath + "\" -o \"" + objects[k] + "\"\"";
      if (system(command.c_str()) != 0)
        ++failures;
      else
        writeFile(hashPath, hashText);
    });
    double compileTime = seconds(Clock::now() - start);
    if (failures)
    {
      report("clang:   %.3f s (%s, %zu modules, %d failed)\n", compileTime, level, cg.modules.size(), failures.load());
      codegenExit = 1;
      return;
    }
    start = Clock::now();
    // The objects go through a response file: 64 paths do not fit a command line.
    std::string response;
    auto slashes = [](std::string path) // clang reads a backslash in a response file as an escape
    {
      for (char& c : path)
        if (c == '\\')
          c = '/';
      return path;
    };
    for (const std::string& o : objects)
      response += "\"" + slashes(o) + "\"\n";
    response += "-o \"" + slashes(options.exe) + "\"\n";
    for (const std::string& flag : splitFlags(linkFlags(options)))
      response += slashes(flag) + "\n";
    std::string responsePath = options.exe + ".link.rsp";
    if (!writeFile(responsePath, response))
      return;
    std::string link = "\"\"" + clang + "\" @\"" + responsePath + "\"\"";
    int rc = system(link.c_str());
    report("clang:   %.3f s (%s, %zu modules on %u threads, %d unchanged) + link %.3f s (exit %d)\n", compileTime, level, cg.modules.size(),
           options.threads, cached.load(), seconds(Clock::now() - start), rc);
    if (rc != 0)
      codegenExit = 1;
  }

  void runResolve(std::vector<std::unique_ptr<ParsedFile>>& parsed, SymbolTable& symbols, const Options& options,
                  std::vector<SourceFile>& files, std::vector<LexResult>& lexed);

  uint32_t lineOf(const std::string& text, uint32_t offset)
  {
    uint32_t line = 1;
    for (uint32_t i = 0; i < offset && i < text.size(); ++i)
      if (text[i] == '\n')
        ++line;
    return line;
  }

  void runParse(std::vector<SourceFile>& files, std::vector<LexResult>& lexed, SymbolTable& symbols, const Options& options)
  {
    using Clock = std::chrono::steady_clock;
    std::vector<std::unique_ptr<ParsedFile>> parsed(files.size());
    double parseTime = 1e9;
    for (int round = 0; round < std::max(1, options.repeat); ++round)
    {
      // Repeated rounds re-parse from the original tokens ('>>' splits are undone by re-lexing).
      std::vector<std::vector<Token>> tokens(files.size());
      if (round > 0)
        parallelFor(files.size(), options.threads, [&](size_t i) { tokens[i] = lexed[i].tokens; });
      auto start = Clock::now();
      parallelFor(files.size(), options.threads, [&](size_t i)
      {
        parsed[i] = std::make_unique<ParsedFile>();
        parsed[i]->path = files[i].path.string();
        parseFile(round == 0 ? lexed[i].tokens : tokens[i], files[i].text, symbols, *parsed[i], false, &options.configValues);
      });
      double t = seconds(Clock::now() - start);
      if (round > 0)
        report("parse round %d: %.3f s\n", round + 1, t);
      parseTime = std::min(parseTime, t);
      if (round + 1 < std::max(1, options.repeat))
        continue;
      if (round > 0)
        for (size_t i = 0; i < files.size(); ++i)
        {
          lexed[i].tokens = std::move(tokens[i]);
          parsed[i]->tokens = &lexed[i].tokens;
        }
    }
    importCHeaders(files, lexed, parsed, symbols, options);
    uint64_t decls = 0, bodies = 0, nodes = 0, errors = 0, filesWithErrors = 0, arenaBytes = 0, lines = 0;
    for (size_t i = 0; i < files.size(); ++i)
    {
      if (parsed[i]->isImported)
        continue; // C headers are not part of the parse statistics
      decls += parsed[i]->declCount;
      bodies += parsed[i]->functionBodies;
      nodes += parsed[i]->nodeCount;
      errors += parsed[i]->diagnostics.size();
      filesWithErrors += !parsed[i]->diagnostics.empty();
      arenaBytes += parsed[i]->arena.bytesAllocated();
      lines += lexed[i].lineCount;
    }
    report("parse:   %.3f s  (%.1f M lines/s)\n", parseTime, lines / 1e6 / parseTime);
    report("  decls: %llu, function bodies: %llu, nodes: %llu, AST memory: %.1f MB\n", (unsigned long long)decls,
           (unsigned long long)bodies, (unsigned long long)nodes, arenaBytes / 1e6);
    report("  parse errors: %llu in %llu files (%.2f%% of files clean)\n", (unsigned long long)errors,
           (unsigned long long)filesWithErrors, 100.0 * (files.size() - filesWithErrors) / std::max<size_t>(1, files.size()));
    int shown = 0;
    for (size_t i = 0; i < files.size() && shown < options.showErrors; ++i)
      for (const Diagnostic& d : parsed[i]->diagnostics)
      {
        if (parsed[i]->isImported && !options.showImportErrors)
          break; // reported in one line by importCHeaders
        if (shown++ >= options.showErrors)
          break;
        const Token& t = lexed[i].tokens[std::min<size_t>(d.token, lexed[i].tokens.size() - 1)];
        report("  %s:%u: %s (at '%.*s')\n", files[i].path.string().c_str(), lineOf(files[i].text, t.offset), d.message,
               int(std::min<uint32_t>(t.length, 30)), files[i].text.data() + t.offset);
      }
    runResolve(parsed, symbols, options, files, lexed);
  }

  void runResolve(std::vector<std::unique_ptr<ParsedFile>>& parsed, SymbolTable& symbols, const Options& options,
                  std::vector<SourceFile>& files, std::vector<LexResult>& lexed)
  {
    using Clock = std::chrono::steady_clock;
    GlobalScope global(symbols);
    auto start = Clock::now();
    parallelFor(parsed.size(), options.threads, [&](size_t i) { global.indexFile(*parsed[i]); });
    double indexTime = seconds(Clock::now() - start);
    start = Clock::now();
    global.finishIndex();
    double finishTime = seconds(Clock::now() - start);
    report("index:   %.3f s  (+%.3f s for out-of-line class definitions): %zu scopes, %zu entries\n", indexTime, finishTime,
           global.scopeCount(), global.entryCount());

    std::vector<ResolveStats> stats(parsed.size());
    start = Clock::now();
    for (size_t i = 0; i < stats.size(); ++i)
      stats[i].fileIndex = uint32_t(i);
    for (size_t i = 0; i < stats.size(); ++i)
      stats[i].fileIndex = uint32_t(i);
    parallelFor(parsed.size(), options.threads, [&](size_t i) { global.resolveFile(*parsed[i], lexed[i].tokens, files[i].text, stats[i]); });
    double resolveTime = seconds(Clock::now() - start);
    ResolveStats total;
    for (const ResolveStats& s : stats)
      total.add(s);
    uint64_t types = total.typesResolved + total.typesDependent + total.typesUnresolved;
    uint64_t names = total.namesLocal + total.namesMember + total.namesGlobal + total.namesDependent + total.namesUnresolved;
    uint64_t project = total.typesUnresolved + total.namesUnresolved - total.qualifiedUnresolvedStd - total.unresolvedMacroLike;
    report("resolve: %.3f s\n", resolveTime);
    report("  type names:  %llu  (%.1f%% resolved, %.1f%% dependent on template parameters, %.1f%% unresolved)\n",
           (unsigned long long)types, 100.0 * total.typesResolved / std::max<uint64_t>(1, types),
           100.0 * total.typesDependent / std::max<uint64_t>(1, types), 100.0 * total.typesUnresolved / std::max<uint64_t>(1, types));
    report("  identifiers: %llu  (%.1f%% local, %.1f%% member, %.1f%% namespace/global, %.1f%% dependent, %.1f%% unresolved)\n",
           (unsigned long long)names, 100.0 * total.namesLocal / std::max<uint64_t>(1, names),
           100.0 * total.namesMember / std::max<uint64_t>(1, names), 100.0 * total.namesGlobal / std::max<uint64_t>(1, names),
           100.0 * total.namesDependent / std::max<uint64_t>(1, names), 100.0 * total.namesUnresolved / std::max<uint64_t>(1, names));
    report("  unresolved: %llu std::, %llu macro-like, %llu other; %llu fixed-width integer names typed as builtins\n",
           (unsigned long long)total.qualifiedUnresolvedStd, (unsigned long long)total.unresolvedMacroLike,
           (unsigned long long)project, (unsigned long long)total.typesBuiltinByName);
    uint64_t members = total.membersBound + total.membersDependent + total.membersUnknownBase + total.membersNotFound;
    uint64_t exprs = total.exprsTyped + total.exprsDependent + total.exprsUnknown;
    report("check:   %zu distinct types\n", global.typeCount());
    report("  member accesses: %llu  (%.1f%% bound to a declaration, %.1f%% dependent, %.1f%% object type unknown, %.1f%% no such member)\n",
           (unsigned long long)members, 100.0 * total.membersBound / std::max<uint64_t>(1, members),
           100.0 * total.membersDependent / std::max<uint64_t>(1, members), 100.0 * total.membersUnknownBase / std::max<uint64_t>(1, members),
           100.0 * total.membersNotFound / std::max<uint64_t>(1, members));
    report("  expressions: %llu  (%.1f%% typed, %.1f%% dependent on template parameters, %.1f%% unknown)\n",
           (unsigned long long)exprs, 100.0 * total.exprsTyped / std::max<uint64_t>(1, exprs),
           100.0 * total.exprsDependent / std::max<uint64_t>(1, exprs), 100.0 * total.exprsUnknown / std::max<uint64_t>(1, exprs));
    report("  unknown because: name %llu, object type %llu, member %llu, overload %llu, deduction %llu, auto return %llu, "
           "auto %llu, operator %llu, unsupported construct %llu\n",
           (unsigned long long)total.unknownName, (unsigned long long)total.unknownBase, (unsigned long long)total.unknownMember,
           (unsigned long long)total.unknownOverload, (unsigned long long)total.unknownDeduction, (unsigned long long)total.unknownAutoReturn,
           (unsigned long long)total.unknownAuto, (unsigned long long)total.unknownOperator, (unsigned long long)total.unknownUnsupported);
    report("  calls resolved to a function: %llu (%llu still ambiguous after conversion ranking)\n", (unsigned long long)total.callsResolved,
           (unsigned long long)total.callsAmbiguousArity);
    GlobalScope::InstantiationStats inst = global.instantiationStats();
    uint64_t instExprs = total.instExprsTyped + total.instExprsDependent + total.instExprsUnknown;
    report("instantiate: %zu class and %zu function template instantiations checked once each (%zu requests, %zu waits), %.3f s of CPU\n",
           inst.classes, inst.functions, inst.requests, inst.waits, inst.seconds);
    report("  %zu member bodies checked inside instantiations; %llu expressions there (%.1f%% typed, %.1f%% dependent, %.1f%% unknown)\n",
           inst.bodies, (unsigned long long)instExprs, 100.0 * total.instExprsTyped / std::max<uint64_t>(1, instExprs),
           100.0 * total.instExprsDependent / std::max<uint64_t>(1, instExprs), 100.0 * total.instExprsUnknown / std::max<uint64_t>(1, instExprs));
    std::vector<std::pair<uint32_t, uint32_t>> missing(total.missingMembers.begin(), total.missingMembers.end());
    std::sort(missing.begin(), missing.end(), [](auto& a, auto& b) { return a.second > b.second; });
    if (!missing.empty() && options.showUnresolved > 0)
      report("  most common members not found in a known class:\n");
    for (size_t i = 0; i < missing.size() && int(i) < options.showUnresolved / 2; ++i)
    {
      ResolveStats::Where w = total.missingMembersFirst[missing[i].first];
      const Token& t = lexed[w.file].tokens[std::min<size_t>(w.token, lexed[w.file].tokens.size() - 1)];
      report("    %6u  %-28.*s first at %s:%u\n", missing[i].second, int(symbols.name(missing[i].first).size()),
             symbols.name(missing[i].first).data(), files[w.file].path.string().c_str(), lineOf(files[w.file].text, t.offset));
    }
    std::vector<std::pair<uint32_t, uint32_t>> unknownBase(total.unknownBaseMembers.begin(), total.unknownBaseMembers.end());
    std::sort(unknownBase.begin(), unknownBase.end(), [](auto& a, auto& b) { return a.second > b.second; });
    if (!unknownBase.empty() && options.showUnresolved > 0)
      report("  most common members accessed on an object of unknown type:\n");
    for (size_t i = 0; i < unknownBase.size() && int(i) < options.showUnresolved / 2; ++i)
      report("    %6u  %.*s\n", unknownBase[i].second, int(symbols.name(unknownBase[i].first).size()), symbols.name(unknownBase[i].first).data());
    if (options.showUnresolved > 0)
      report("  most common unresolved names:\n");
    std::vector<std::pair<uint32_t, uint32_t>> top(total.unresolvedSymbols.begin(), total.unresolvedSymbols.end());
    std::sort(top.begin(), top.end(), [](auto& a, auto& b) { return a.second > b.second; });
    if (!options.emitLLVM.empty() || !options.exe.empty())
      runCodegen(parsed, global, symbols, options);
    if (!options.convertDir.empty())
    {
      std::vector<ConvertInput> inputs;
      for (size_t i = 0; i < parsed.size(); ++i)
        inputs.push_back({files[i].path, files[i].text, &lexed[i].tokens, parsed[i].get()});
      std::vector<std::filesystem::path> roots(options.roots.begin(), options.roots.end());
      std::string summary = convertToKov(inputs, global, symbols, roots, options.convertDir, options.overridesDir);
      report("convert: %s\n%s", options.convertDir.c_str(), summary.c_str());
    }
    for (size_t i = 0; i < top.size() && int(i) < options.showUnresolved; ++i)
    {
      ResolveStats::Where w = total.unresolvedFirst[top[i].first];
      const Token& t = lexed[w.file].tokens[std::min<size_t>(w.token, lexed[w.file].tokens.size() - 1)];
      report("    %6u  %-28.*s first at %s:%u\n", top[i].second, int(symbols.name(top[i].first).size()),
             symbols.name(top[i].first).data(), files[w.file].path.string().c_str(), lineOf(files[w.file].text, t.offset));
    }
  }

  double seconds(std::chrono::steady_clock::duration d)
  {
    return std::chrono::duration<double>(d).count();
  }
}

int main(int argc, char** argv)
{
  setvbuf(stdout, nullptr, _IONBF, 0); // progress lines survive a crash
  std::set_terminate([]
  {
    try
    {
      if (auto e = std::current_exception())
        std::rethrow_exception(e);
    }
    catch (const std::exception& e) { fprintf(stderr, "fatal: %s\n", e.what()); }
    catch (...) { fprintf(stderr, "fatal: unknown exception\n"); }
    abort();
  });
  Options options;
  for (int i = 1; i < argc; ++i)
  {
    std::string arg = argv[i];
    if (arg == "-j" && i + 1 < argc)
      options.threads = unsigned(atoi(argv[++i]));
    else if (arg == "--ext" && i + 1 < argc)
    {
      options.extensions.clear();
      std::string list = argv[++i];
      size_t pos = 0;
      while (pos <= list.size())
      {
        size_t comma = list.find(',', pos);
        if (comma == std::string::npos)
          comma = list.size();
        options.extensions.push_back(list.substr(pos, comma - pos));
        pos = comma + 1;
      }
    }
    else if (arg == "--repeat" && i + 1 < argc)
      options.repeat = atoi(argv[++i]);
    else if (arg == "--dump")
      options.dumpTokens = true;
    else if (arg == "--lex-only")
      options.parse = false;
    else if (arg == "--errors" && i + 1 < argc)
      options.showErrors = atoi(argv[++i]), options.showImportErrors = true;
    else if (arg == "--unresolved" && i + 1 < argc)
      options.showUnresolved = atoi(argv[++i]);
    else if (arg == "--quiet")
      options.quiet = true;
    else if (arg == "--emit-llvm" && i + 1 < argc)
      options.emitLLVM = argv[++i];
    else if (arg == "--exe" && i + 1 < argc)
      options.exe = argv[++i];
    else if (arg == "--std" && i + 1 < argc)
      options.stdDir = argv[++i];
    else if (arg == "--no-std")
      options.noStd = true;
    else if (arg == "--cinclude" && i + 1 < argc)
      options.cIncludeDirs.push_back(argv[++i]);
    else if (arg == "--lib" && i + 1 < argc)
      options.libs.push_back(argv[++i]);
    else if (arg == "--config" && i + 1 < argc)
    {
      std::string item = argv[++i];
      size_t eq = item.find('=');
      if (eq == std::string::npos)
        fprintf(stderr, "warning: --config expects name=value, got %s\n", item.c_str());
      else
        options.configValues[item.substr(0, eq)] = parseConfigValue(item.substr(eq + 1));
    }
    else if (arg == "--release")
      options.configValues["debug"] = parseConfigValue("false");
    else if (arg == "--modules" && i + 1 < argc)
      options.modules = unsigned(atoi(argv[++i]));
    else if (arg == "--convert" && i + 1 < argc)
      options.convertDir = argv[++i];
    else if (arg == "--overrides" && i + 1 < argc)
      options.overridesDir = argv[++i];
    else if (arg[0] == '-')
    {
      usage();
      return 2;
    }
    else
      options.roots.push_back(arg);
  }
  if (options.roots.empty())
  {
    usage();
    return 2;
  }
  if (options.threads == 0)
    options.threads = 1;
  quietOutput = options.quiet;
  // The standard library joins the program for code generation, and whenever --std names it.
  if ((!options.exe.empty() || !options.emitLLVM.empty() || !options.stdDir.empty()) && !options.noStd)
  {
    std::error_code ec;
    std::filesystem::path stdPath = options.stdDir.empty()
      ? std::filesystem::absolute(argv[0], ec).parent_path() / ".." / ".." / "stdlib"
      : std::filesystem::path(options.stdDir);
    if (std::filesystem::is_directory(stdPath, ec))
      options.roots.insert(options.roots.begin(), stdPath.lexically_normal());
    else if (!options.stdDir.empty())
      fprintf(stderr, "warning: standard library directory %s not found\n", options.stdDir.c_str());
  }

  using Clock = std::chrono::steady_clock;

  // Phase 0: find and read all files. Reading is I/O bound, so it is timed separately.
  auto startCollect = Clock::now();
  std::vector<std::filesystem::path> paths;
  for (const auto& root : options.roots)
  {
    auto found = collectFiles(root, options.extensions);
    paths.insert(paths.end(), found.begin(), found.end());
  }
  std::vector<SourceFile> files(paths.size());
  files.reserve(paths.size() + 2); // the configuration and the imported C headers
  std::atomic<uint64_t> totalBytes{0};
  parallelFor(paths.size(), options.threads, [&](size_t i)
  {
    files[i].path = paths[i];
    if (!readFile(paths[i], files[i].text))
      fprintf(stderr, "cannot read %s\n", paths[i].string().c_str());
    totalBytes += files[i].text.size();
  });
  {
    // The build configuration is one more source file: `config::debug`, `config::platform`...
    if (!options.configValues.count("debug"))
      options.configValues["debug"] = parseConfigValue("true");
    if (!options.configValues.count("platform"))
      options.configValues["platform"] = parseConfigValue("Windows");
    options.debug = options.configValues["debug"].number != 0;
    SourceFile config;
    config.path = "<config>";
    config.text = configSource(options.configValues);
    totalBytes += config.text.size();
    files.push_back(std::move(config));
  }
  double collectTime = seconds(Clock::now() - startCollect);

  // Phase 1: lex everything in parallel.
  double bestLex = 1e9;
  FileStats total;
  for (int round = 0; round < options.repeat; ++round)
  {
    SymbolTable symbols;
    std::vector<FileStats> stats(files.size());
    std::vector<LexResult> results(files.size());
    auto start = Clock::now();
    parallelFor(files.size(), options.threads, [&](size_t i)
    {
      results[i] = lex(files[i].text, symbols);
      stats[i].bytes = files[i].text.size();
      stats[i].tokens = results[i].tokens.size();
      stats[i].lines = results[i].lineCount;
      stats[i].errors = results[i].errorCount;
    });
    double lexTime = seconds(Clock::now() - start);
    bestLex = std::min(bestLex, lexTime);

    total = {};
    for (const auto& s : stats)
    {
      total.bytes += s.bytes;
      total.tokens += s.tokens;
      total.lines += s.lines;
      total.errors += s.errors;
    }

    if (round == options.repeat - 1)
    {
      report("files:   %zu\n", files.size());
      report("bytes:   %llu (%.1f MB)\n", (unsigned long long)total.bytes, total.bytes / 1e6);
      report("lines:   %llu\n", (unsigned long long)total.lines);
      report("tokens:  %llu\n", (unsigned long long)total.tokens);
      report("symbols: %zu distinct identifiers\n", symbols.size());
      report("errors:  %llu\n", (unsigned long long)total.errors);
      report("threads: %u\n", options.threads);
      report("read:    %.3f s\n", collectTime);
      report("lex:     %.3f s  (%.0f MB/s, %.1f M lines/s, %.1f M tokens/s)\n", bestLex,
             total.bytes / 1e6 / bestLex, total.lines / 1e6 / bestLex, total.tokens / 1e6 / bestLex);
      if (total.errors)
      {
        unsigned shown = 0;
        for (size_t i = 0; i < files.size() && shown < 20; ++i)
          for (const Token& t : results[i].tokens)
            if (t.getKind() == TokenKind::Error && shown++ < 20)
              report("  error in %s at offset %u: '%.*s'\n", files[i].path.string().c_str(), t.offset,
                     int(std::min<uint32_t>(t.length, 40)), files[i].text.data() + t.offset);
      }
      if (options.parse)
        runParse(files, results, symbols, options);
      if (options.dumpTokens)
        for (size_t i = 0; i < files.size(); ++i)
        {
          report("== %s\n", files[i].path.string().c_str());
          for (const Token& t : results[i].tokens)
            report("%-10s %.*s\n", tokenKindName(t.getKind()), int(t.length), files[i].text.data() + t.offset);
        }
    }
  }
  return codegenExit;
}
