// kovlex.cpp: kovlex.kov transliterated to C++ as literally as possible, so that the code the
// two compilers generate for the same algorithm can be compared. Build with the same clang:
//   clang -O2 kovlex.cpp -o kovlex_cpp.exe
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <io.h>
#include <string>
#include <vector>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

// --- tokens -------------------------------------------------------------------------------

enum class TokenKind : uint8_t { End, Identifier, Keyword, Number, String, Char, Punctuator, Directive, Error };

struct Token
{
  uint32_t offset;
  uint32_t length;
  uint8_t kind;
  uint32_t id;
};

// --- symbols ------------------------------------------------------------------------------

struct Slice
{
  const char* data;
  uint32_t length;
};

struct SymbolTable
{
  uint32_t* slots = nullptr;
  uint32_t capacity = 0;
  std::vector<Slice> names;

  SymbolTable() { this->rehash(1 << 17); }
  ~SymbolTable() { free(this->slots); }

  int size() const { return (int)this->names.size(); }

  static uint64_t hashOf(const char* data, uint32_t length)
  {
    uint64_t h = 14695981039346656037ull;
    for (uint32_t i = 0; i < length; ++i)
      h = (h ^ (uint64_t)(uint8_t)data[i]) * 1099511628211ull;
    return h;
  }

  void rehash(uint32_t newCapacity)
  {
    uint32_t* newSlots = (uint32_t*)malloc(newCapacity * sizeof(uint32_t));
    memset(newSlots, 0, newCapacity * sizeof(uint32_t));
    for (uint32_t i = 0; i < this->capacity; ++i)
      if (this->slots[i] != 0)
      {
        Slice& name = this->names[(int)(this->slots[i] - 1)];
        uint32_t index = (uint32_t)(SymbolTable::hashOf(name.data, name.length) & (newCapacity - 1));
        while (newSlots[index] != 0)
          index = (index + 1) & (newCapacity - 1);
        newSlots[index] = this->slots[i];
      }
    free(this->slots);
    this->slots = newSlots;
    this->capacity = newCapacity;
  }

  uint32_t intern(const char* data, uint32_t length)
  {
    if ((uint32_t)this->names.size() * 2 >= this->capacity)
      this->rehash(this->capacity * 2);
    uint32_t index = (uint32_t)(SymbolTable::hashOf(data, length) & (this->capacity - 1));
    for (;;)
    {
      uint32_t slot = this->slots[index];
      if (slot == 0)
      {
        Slice s;
        s.data = data;
        s.length = length;
        this->names.push_back(s);
        this->slots[index] = (uint32_t)this->names.size();
        return (uint32_t)this->names.size() - 1;
      }
      Slice& existing = this->names[(int)(slot - 1)];
      if (existing.length == length && memcmp(existing.data, data, length) == 0)
        return slot - 1;
      index = (index + 1) & (this->capacity - 1);
    }
  }
};

int addKeywords(SymbolTable& symbols)
{
  const char* words = "alignas alignof and and_eq asm auto bitand bitor bool break case catch char char8_t "
                      "char16_t char32_t class compl concept const consteval constexpr constinit const_cast "
                      "continue co_await co_return co_yield decltype default delete do double dynamic_cast "
                      "else enum explicit export extern false float for friend goto if inline int long "
                      "mutable namespace new noexcept not not_eq nullptr operator or or_eq private protected "
                      "public register reinterpret_cast requires return short signed sizeof static "
                      "static_assert static_cast struct switch template this thread_local throw true try "
                      "typedef typeid typename union unsigned using virtual void volatile wchar_t while xor "
                      "xor_eq";
  int count = 0;
  const char* p = words;
  while (*p != 0)
  {
    const char* start = p;
    while (*p != 0 && *p != ' ')
      ++p;
    symbols.intern(start, (uint32_t)(p - start));
    ++count;
    while (*p == ' ')
      ++p;
  }
  return count;
}

// --- the lexer ----------------------------------------------------------------------------

bool isIdentStart(char c)
{
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '$' || (uint8_t)c >= 128;
}
bool isIdentChar(char c) { return isIdentStart(c) || (c >= '0' && c <= '9'); }
bool isDigit(char c) { return c >= '0' && c <= '9'; }

struct Lexer
{
  const char* begin;
  const char* p;
  const char* end;
  SymbolTable* symbols;
  std::vector<Token>* tokens;
  uint32_t keywordCount;
  uint32_t lineCount = 0;
  uint32_t errorCount = 0;

  Lexer(const char* text, uint32_t size, SymbolTable* symbols, uint32_t keywordCount, std::vector<Token>* tokens)
    : begin(text), p(text), end(text + size), symbols(symbols), tokens(tokens), keywordCount(keywordCount)
  {
  }

  void push(TokenKind kind, const char* start, uint32_t id)
  {
    Token t;
    t.offset = (uint32_t)(start - this->begin);
    t.length = (uint32_t)(this->p - start) & 0xFFFFFF;
    t.kind = (uint8_t)kind;
    t.id = id;
    this->tokens->push_back(t);
  }

  void error(const char* start)
  {
    ++this->errorCount;
    this->push(TokenKind::Error, start, 0);
  }

  bool skipTrivia()
  {
    bool newline = false;
    for (;;)
    {
      char c = *this->p;
      if (c == '\n')
      {
        ++this->p;
        ++this->lineCount;
        newline = true;
      }
      else if (c == ' ' || c == '\t' || c == '\r' || c == '\f' || c == '\v')
        ++this->p;
      else if (c == '/' && this->p[1] == '/')
      {
        this->p += 2;
        while (*this->p != 0 && *this->p != '\n')
          ++this->p;
      }
      else if (c == '/' && this->p[1] == '*')
      {
        this->p += 2;
        while (*this->p != 0 && !(*this->p == '*' && this->p[1] == '/'))
        {
          if (*this->p == '\n')
            ++this->lineCount;
          ++this->p;
        }
        if (*this->p != 0)
          this->p += 2;
      }
      else if (c == '\\' && (this->p[1] == '\n' || (this->p[1] == '\r' && this->p[2] == '\n')))
      {
        this->p += this->p[1] == '\r' ? 3 : 2;
        ++this->lineCount;
      }
      else
        return newline;
    }
  }

  void lexDirective(const char* start)
  {
    for (;;)
    {
      char c = *this->p;
      if (c == 0)
        break;
      if (c == '\n')
        break;
      if (c == '\\' && (this->p[1] == '\n' || (this->p[1] == '\r' && this->p[2] == '\n')))
      {
        this->p += this->p[1] == '\r' ? 3 : 2;
        ++this->lineCount;
        continue;
      }
      if (c == '/' && this->p[1] == '*')
      {
        this->p += 2;
        while (*this->p != 0 && !(*this->p == '*' && this->p[1] == '/'))
        {
          if (*this->p == '\n')
            ++this->lineCount;
          ++this->p;
        }
        if (*this->p != 0)
          this->p += 2;
        continue;
      }
      if (c == '/' && this->p[1] == '/')
      {
        while (*this->p != 0 && *this->p != '\n')
          ++this->p;
        break;
      }
      ++this->p;
    }
    this->push(TokenKind::Directive, start, 0);
  }

  bool lexRawString(const char* start)
  {
    ++this->p;
    const char* delimStart = this->p;
    while (*this->p != 0 && *this->p != '(' && *this->p != '"' && *this->p != '\n' && this->p - delimStart < 16)
      ++this->p;
    if (*this->p != '(')
    {
      this->error(start);
      return false;
    }
    size_t delimLength = (size_t)(this->p - delimStart);
    ++this->p;
    for (;;)
    {
      if (*this->p == 0)
      {
        this->error(start);
        return false;
      }
      if (*this->p == '\n')
        ++this->lineCount;
      if (*this->p == ')' && strncmp(this->p + 1, delimStart, delimLength) == 0 && this->p[1 + delimLength] == '"')
      {
        this->p += 2 + delimLength;
        break;
      }
      ++this->p;
    }
    this->push(TokenKind::String, start, 0);
    return true;
  }

  void lexQuoted(const char* start, char quote)
  {
    ++this->p;
    for (;;)
    {
      char c = *this->p;
      if (c == 0 || c == '\n')
      {
        this->error(start);
        return;
      }
      if (c == '\\')
      {
        this->p += this->p[1] != 0 ? 2 : 1;
        continue;
      }
      ++this->p;
      if (c == quote)
        break;
    }
    while (isIdentChar(*this->p))
      ++this->p;
    this->push(quote == '"' ? TokenKind::String : TokenKind::Char, start, 0);
  }

  void lexNumber(const char* start)
  {
    for (;;)
    {
      char c = *this->p;
      if ((c == 'e' || c == 'E' || c == 'p' || c == 'P') && (this->p[1] == '+' || this->p[1] == '-'))
        this->p += 2;
      else if (isIdentChar(c) || c == '.' || (c == '\'' && isIdentChar(this->p[1])))
        ++this->p;
      else
        break;
    }
    this->push(TokenKind::Number, start, 0);
  }

  void lexIdentifier(const char* start)
  {
    while (isIdentChar(*this->p))
      ++this->p;
    if (*this->p == '"' || *this->p == '\'')
    {
      size_t prefixLength = (size_t)(this->p - start);
      bool raw = prefixLength > 0 && this->p[-1] == 'R';
      size_t encLength = raw ? prefixLength - 1 : prefixLength;
      bool knownEncoding = encLength == 0 || (encLength == 1 && (start[0] == 'u' || start[0] == 'U' || start[0] == 'L')) ||
                           (encLength == 2 && start[0] == 'u' && start[1] == '8');
      if (knownEncoding)
      {
        if (raw && *this->p == '"')
        {
          this->lexRawString(start);
          return;
        }
        if (!raw)
        {
          this->lexQuoted(start, *this->p);
          return;
        }
      }
    }
    uint32_t id = this->symbols->intern(start, (uint32_t)(this->p - start));
    if (id < this->keywordCount)
      this->push(TokenKind::Keyword, start, id);
    else
      this->push(TokenKind::Identifier, start, id - this->keywordCount);
  }

  void lexPunctuator(const char* start)
  {
    char c = *this->p;
    char n = this->p[1];
    int length = 1;
    switch (c)
    {
      case '<':
        if (n == '=' && this->p[2] == '>') length = 3;
        else if (n == '<' && this->p[2] == '=') length = 3;
        else if (n == '<' || n == '=') length = 2;
        break;
      case '>':
        if (n == '>' && this->p[2] == '=') length = 3;
        else if (n == '>' || n == '=') length = 2;
        break;
      case '-':
        if (n == '>' && this->p[2] == '*') length = 3;
        else if (n == '>' || n == '-' || n == '=') length = 2;
        break;
      case '.':
        if (n == '.' && this->p[2] == '.') length = 3;
        else if (n == '*') length = 2;
        break;
      case ':': if (n == ':') length = 2; break;
      case '+': if (n == '+' || n == '=') length = 2; break;
      case '=': if (n == '=') length = 2; break;
      case '!': if (n == '=') length = 2; break;
      case '&': if (n == '&' || n == '=') length = 2; break;
      case '|': if (n == '|' || n == '=') length = 2; break;
      case '*': if (n == '=') length = 2; break;
      case '/': if (n == '=') length = 2; break;
      case '%': if (n == '=') length = 2; break;
      case '^': if (n == '=') length = 2; break;
      case '#': if (n == '#') length = 2; break;
      case '?':
        if (n == '-' && this->p[2] == '>') length = 3;
        else if (n == ':') length = 2;
        break;
      case '{': break;
      case '}': break;
      case '[': break;
      case ']': break;
      case '(': break;
      case ')': break;
      case ';': break;
      case ',': break;
      case '~': break;
      default:
        ++this->p;
        this->error(start);
        return;
    }
    this->p += length;
    this->push(TokenKind::Punctuator, start, (uint32_t)(uint8_t)c * 4 + (uint32_t)length);
  }

  void run()
  {
    if (this->end - this->begin >= 3 && (uint8_t)this->p[0] == 0xEF && (uint8_t)this->p[1] == 0xBB && (uint8_t)this->p[2] == 0xBF)
      this->p += 3;
    bool lineStart = true;
    for (;;)
    {
      if (this->skipTrivia())
        lineStart = true;
      const char* start = this->p;
      char c = *this->p;
      if (c == 0 && this->p >= this->end)
        break;
      if (c == '#' && lineStart)
      {
        this->lexDirective(start);
        continue;
      }
      lineStart = false;
      if (isIdentStart(c))
        this->lexIdentifier(start);
      else if (isDigit(c) || (c == '.' && isDigit(this->p[1])))
        this->lexNumber(start);
      else if (c == '"' || c == '\'')
        this->lexQuoted(start, c);
      else
        this->lexPunctuator(start);
    }
    this->push(TokenKind::End, this->p, 0);
  }
};

// --- files --------------------------------------------------------------------------------

bool hasSourceExtension(const std::string& path)
{
  const char* s = path.c_str();
  int n = (int)path.size();
  if (n >= 4 && memcmp(s + n - 4, ".cpp", 4) == 0)
    return true;
  if (n >= 4 && memcmp(s + n - 4, ".hpp", 4) == 0)
    return true;
  return n >= 2 && memcmp(s + n - 2, ".h", 2) == 0;
}

void collectFiles(const std::string& dir, std::vector<std::string>& out)
{
  __finddata64_t data;
  std::string pattern(dir);
  pattern.append("\\*");
  intptr_t handle = _findfirst64(pattern.c_str(), &data);
  if (handle == -1)
    return;
  for (;;)
  {
    if (data.name[0] != '.')
    {
      std::string path(dir);
      path.append("\\");
      path.append(data.name);
      if ((data.attrib & _A_SUBDIR) != 0)
        collectFiles(path, out);
      else if (hasSourceExtension(path))
        out.push_back(path);
    }
    if (_findnext64(handle, &data) != 0)
      break;
  }
  _findclose(handle);
}

char* readFile(const char* path, uint32_t& size)
{
  FILE* f = fopen(path, "rb");
  if (f == nullptr)
    return nullptr;
  fseek(f, 0, SEEK_END);
  long length = ftell(f);
  fseek(f, 0, SEEK_SET);
  char* buffer = (char*)malloc((size_t)length + 1);
  size = (uint32_t)fread(buffer, 1, (size_t)length, f);
  buffer[size] = 0;
  fclose(f);
  return buffer;
}

double now()
{
  LARGE_INTEGER counter;
  LARGE_INTEGER frequency;
  QueryPerformanceCounter(&counter);
  QueryPerformanceFrequency(&frequency);
  return (double)counter.QuadPart / (double)frequency.QuadPart;
}

int main(int argc, char** argv)
{
  if (argc < 2)
  {
    printf("usage: kovlex <directory>...\n");
    return 1;
  }
  std::vector<std::string> paths;
  for (int i = 1; i < argc; ++i)
  {
    std::string root(argv[i]);
    collectFiles(root, paths);
  }

  double t0 = now();
  std::vector<char*> texts;
  std::vector<uint32_t> sizes;
  uint64_t bytes = 0;
  for (int i = 0; i < (int)paths.size(); ++i)
  {
    uint32_t size = 0;
    char* text = readFile(paths[i].c_str(), size);
    if (text == nullptr)
    {
      printf("cannot read %s\n", paths[i].c_str());
      continue;
    }
    texts.push_back(text);
    sizes.push_back(size);
    bytes += size;
  }
  double t1 = now();

  SymbolTable symbols;
  uint32_t keywordCount = (uint32_t)addKeywords(symbols);
  std::vector<Token> tokens;
  tokens.reserve((size_t)(bytes / 5));
  uint64_t lines = 0;
  uint64_t errors = 0;
  for (int i = 0; i < (int)texts.size(); ++i)
  {
    Lexer lexer(texts[i], sizes[i], &symbols, keywordCount, &tokens);
    lexer.run();
    lines += lexer.lineCount;
    errors += lexer.errorCount;
  }
  double t2 = now();

  double lexTime = t2 - t1;
  printf("files:   %d\n", (int)texts.size());
  printf("bytes:   %llu (%.1f MB)\n", (unsigned long long)bytes, (double)bytes / 1000000.0);
  printf("lines:   %llu\n", (unsigned long long)lines);
  printf("tokens:  %d\n", (int)tokens.size());
  printf("symbols: %d distinct identifiers\n", symbols.size() - (int)keywordCount);
  printf("errors:  %llu\n", (unsigned long long)errors);
  printf("read:    %.3f s\n", t1 - t0);
  printf("lex:     %.3f s  (%.0f MB/s, %.1f M lines/s, %.1f M tokens/s)\n", lexTime, (double)bytes / 1000000.0 / lexTime,
         (double)lines / 1000000.0 / lexTime, (double)tokens.size() / 1000000.0 / lexTime);
  return 0;
}
