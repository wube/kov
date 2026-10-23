#include "lexer.hpp"
#include <algorithm>
#include <array>
#include <cstring>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>

// --- Keywords and punctuators -----------------------------------------------------------

static constexpr std::string_view keywords[] = {
#define X(name) #name,
  KOV_KEYWORDS(X)
#undef X
};
static_assert(std::size(keywords) == size_t(Kw::Count));

static constexpr std::string_view punctuators[] = {
#define X(name, spelling) spelling,
  KOV_PUNCTUATORS(X)
#undef X
};
static_assert(std::size(punctuators) == size_t(P::Count));

std::string_view keywordSpelling(Kw id) { return keywords[uint32_t(id)]; }
std::string_view punctuatorSpelling(P id) { return punctuators[uint32_t(id)]; }

namespace
{
  struct KeywordMap
  {
    std::unordered_map<std::string_view, uint32_t> map;
    KeywordMap()
    {
      for (uint32_t i = 0; i < std::size(keywords); ++i)
        map.emplace(keywords[i], i);
    }
  };
  const KeywordMap& keywordMap()
  {
    static KeywordMap instance;
    return instance;
  }

  // Punctuator candidates per first byte, longest first.
  struct PunctuatorIndex
  {
    std::array<std::vector<uint32_t>, 256> byFirst;
    PunctuatorIndex()
    {
      for (uint32_t i = 0; i < std::size(punctuators); ++i)
        byFirst[uint8_t(punctuators[i][0])].push_back(i);
      for (auto& list : byFirst)
        std::sort(list.begin(), list.end(),
                  [](uint32_t a, uint32_t b) { return punctuators[a].size() > punctuators[b].size(); });
    }
  };
  const PunctuatorIndex& punctuatorIndex()
  {
    static PunctuatorIndex instance;
    return instance;
  }
}

uint32_t lookupKeyword(std::string_view spelling)
{
  const auto& map = keywordMap().map;
  auto it = map.find(spelling);
  return it == map.end() ? UINT32_MAX : it->second;
}

// --- SymbolTable ------------------------------------------------------------------------

struct SymbolTable::Impl
{
  static constexpr uint32_t shardCount = 256;
  struct Shard
  {
    std::shared_mutex mutex;
    std::unordered_map<std::string_view, uint32_t> map;
  };
  Shard shards[shardCount];
  // Names are stored in stable chunks so string_views in the maps stay valid.
  std::mutex namesMutex;
  std::vector<std::string*> names; // id -> name (pointer stable)
  std::vector<std::unique_ptr<char[]>> chunks;
  char* chunkPos = nullptr;
  char* chunkEnd = nullptr;
  std::vector<std::string_view> byId;

  static uint64_t hash(std::string_view s)
  {
    uint64_t h = 14695981039346656037ull;
    for (unsigned char c : s)
      h = (h ^ c) * 1099511628211ull;
    return h;
  }

  std::string_view store(std::string_view s)
  {
    if (size_t(chunkEnd - chunkPos) < s.size())
    {
      size_t size = std::max<size_t>(1 << 16, s.size());
      chunks.push_back(std::make_unique<char[]>(size));
      chunkPos = chunks.back().get();
      chunkEnd = chunkPos + size;
    }
    memcpy(chunkPos, s.data(), s.size());
    std::string_view result(chunkPos, s.size());
    chunkPos += s.size();
    return result;
  }
};

SymbolTable::SymbolTable() : impl(new Impl) {}
SymbolTable::~SymbolTable() { delete impl; }

uint32_t SymbolTable::intern(std::string_view name)
{
  uint64_t h = Impl::hash(name);
  Impl::Shard& shard = impl->shards[h & (Impl::shardCount - 1)];
  {
    std::shared_lock lock(shard.mutex);
    auto it = shard.map.find(name);
    if (it != shard.map.end())
      return it->second;
  }
  std::unique_lock lock(shard.mutex);
  auto it = shard.map.find(name);
  if (it != shard.map.end())
    return it->second;
  uint32_t id;
  std::string_view stored;
  {
    std::lock_guard namesLock(impl->namesMutex);
    stored = impl->store(name);
    id = uint32_t(impl->byId.size());
    impl->byId.push_back(stored);
  }
  shard.map.emplace(stored, id);
  return id;
}

std::string_view SymbolTable::name(uint32_t id) const
{
  std::lock_guard lock(impl->namesMutex);
  return impl->byId[id];
}

size_t SymbolTable::size() const
{
  std::lock_guard lock(impl->namesMutex);
  return impl->byId.size();
}

// --- Lexer ------------------------------------------------------------------------------

namespace
{
  inline bool isIdentStart(unsigned char c)
  {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '$' || c >= 0x80;
  }
  inline bool isIdentChar(unsigned char c) { return isIdentStart(c) || (c >= '0' && c <= '9'); }
  inline bool isDigit(unsigned char c) { return c >= '0' && c <= '9'; }

  struct Lexer
  {
    const char* begin;
    const char* p;
    const char* end;
    SymbolTable& symbols;
    LexResult result;

    Lexer(std::string_view text, SymbolTable& symbols)
      : begin(text.data()), p(text.data()), end(text.data() + text.size()), symbols(symbols)
    {
    }

    void push(TokenKind kind, const char* start, uint32_t id = 0)
    {
      Token t;
      t.offset = uint32_t(start - begin);
      t.length = uint32_t(p - start) & 0xFFFFFF;
      t.kind = uint32_t(kind);
      t.id = id;
      result.tokens.push_back(t);
    }

    void error(const char* start)
    {
      ++result.errorCount;
      push(TokenKind::Error, start);
    }

    // Skips whitespace and comments. Returns true if a newline was crossed.
    bool skipTrivia()
    {
      bool newline = false;
      for (;;)
      {
        char c = *p;
        if (c == '\n')
        {
          ++p;
          ++result.lineCount;
          newline = true;
        }
        else if (c == ' ' || c == '\t' || c == '\r' || c == '\f' || c == '\v')
          ++p;
        else if (c == '/' && p[1] == '/')
        {
          p += 2;
          while (*p && *p != '\n')
            ++p;
        }
        else if (c == '/' && p[1] == '*')
        {
          p += 2;
          while (*p && !(*p == '*' && p[1] == '/'))
          {
            if (*p == '\n')
              ++result.lineCount;
            ++p;
          }
          if (*p)
            p += 2;
        }
        else if (c == '\\' && (p[1] == '\n' || (p[1] == '\r' && p[2] == '\n')))
        {
          // Line continuation outside a directive; just skip it.
          p += p[1] == '\r' ? 3 : 2;
          ++result.lineCount;
        }
        else
          return newline;
      }
    }

    void lexDirective(const char* start)
    {
      // Consume to end of line, honouring backslash continuations and comments.
      for (;;)
      {
        char c = *p;
        if (c == 0)
          break;
        if (c == '\n')
          break;
        if (c == '\\' && (p[1] == '\n' || (p[1] == '\r' && p[2] == '\n')))
        {
          p += p[1] == '\r' ? 3 : 2;
          ++result.lineCount;
          continue;
        }
        if (c == '/' && p[1] == '*')
        {
          p += 2;
          while (*p && !(*p == '*' && p[1] == '/'))
          {
            if (*p == '\n')
              ++result.lineCount;
            ++p;
          }
          if (*p)
            p += 2;
          continue;
        }
        if (c == '/' && p[1] == '/')
        {
          while (*p && *p != '\n')
            ++p;
          break;
        }
        ++p;
      }
      push(TokenKind::Directive, start);
    }

    bool lexRawString(const char* start)
    {
      // p points at '"' after R. Delimiter up to ')'.
      ++p;
      const char* delimStart = p;
      while (*p && *p != '(' && *p != '"' && *p != '\n' && p - delimStart < 16)
        ++p;
      if (*p != '(')
      {
        error(start);
        return false;
      }
      std::string_view delim(delimStart, size_t(p - delimStart));
      ++p;
      for (;;)
      {
        if (*p == 0)
        {
          error(start);
          return false;
        }
        if (*p == '\n')
          ++result.lineCount;
        if (*p == ')' && strncmp(p + 1, delim.data(), delim.size()) == 0 && p[1 + delim.size()] == '"')
        {
          p += 2 + delim.size();
          break;
        }
        ++p;
      }
      push(TokenKind::String, start);
      return true;
    }

    void lexQuoted(const char* start, char quote)
    {
      ++p;
      for (;;)
      {
        char c = *p;
        if (c == 0 || c == '\n')
        {
          error(start);
          return;
        }
        if (c == '\\')
        {
          p += p[1] ? 2 : 1;
          continue;
        }
        ++p;
        if (c == quote)
          break;
      }
      // User-defined literal suffix.
      while (isIdentChar(uint8_t(*p)))
        ++p;
      push(quote == '"' ? TokenKind::String : TokenKind::Char, start);
    }

    void lexNumber(const char* start)
    {
      // pp-number: digit or .digit, then [0-9a-zA-Z_.'] with e+/e-/p+/p-.
      for (;;)
      {
        char c = *p;
        if ((c == 'e' || c == 'E' || c == 'p' || c == 'P') && (p[1] == '+' || p[1] == '-'))
          p += 2;
        else if (isIdentChar(uint8_t(c)) || c == '.' || (c == '\'' && isIdentChar(uint8_t(p[1]))))
          ++p;
        else
          break;
      }
      push(TokenKind::Number, start);
    }

    void lexIdentifier(const char* start)
    {
      while (isIdentChar(uint8_t(*p)))
        ++p;
      // String/char literal prefix: u8"..", L'..', R"(..)", u8R"(..)" etc.
      if (*p == '"' || *p == '\'')
      {
        std::string_view prefix(start, size_t(p - start));
        bool raw = !prefix.empty() && prefix.back() == 'R';
        std::string_view enc = raw ? prefix.substr(0, prefix.size() - 1) : prefix;
        if (enc.empty() || enc == "u8" || enc == "u" || enc == "U" || enc == "L")
        {
          if (raw && *p == '"')
          {
            lexRawString(start);
            return;
          }
          if (!raw)
          {
            lexQuoted(start, *p);
            return;
          }
        }
      }
      std::string_view spelling(start, size_t(p - start));
      uint32_t keyword = lookupKeyword(spelling);
      if (keyword != UINT32_MAX)
        push(TokenKind::Keyword, start, keyword);
      else
        push(TokenKind::Identifier, start, symbols.intern(spelling));
    }

    void lexPunctuator(const char* start)
    {
      for (uint32_t id : punctuatorIndex().byFirst[uint8_t(*p)])
      {
        std::string_view s = punctuators[id];
        if (strncmp(p, s.data(), s.size()) == 0)
        {
          p += s.size();
          push(TokenKind::Punctuator, start, id);
          return;
        }
      }
      ++p;
      error(start);
    }

    void run()
    {
      result.tokens.reserve(size_t(end - begin) / 5);
      if (end - begin >= 3 && uint8_t(p[0]) == 0xEF && uint8_t(p[1]) == 0xBB && uint8_t(p[2]) == 0xBF)
        p += 3; // UTF-8 byte order mark
      bool lineStart = true;
      for (;;)
      {
        if (skipTrivia())
          lineStart = true;
        const char* start = p;
        unsigned char c = uint8_t(*p);
        if (c == 0 && p >= end)
          break;
        if (c == '#' && lineStart)
        {
          lexDirective(start);
          continue;
        }
        lineStart = false;
        if (isIdentStart(c))
          lexIdentifier(start);
        else if (isDigit(c) || (c == '.' && isDigit(uint8_t(p[1]))))
          lexNumber(start);
        else if (c == '"' || c == '\'')
          lexQuoted(start, char(c));
        else
          lexPunctuator(start);
      }
      push(TokenKind::End, p);
    }
  };
}

LexResult lex(std::string_view text, SymbolTable& symbols)
{
  Lexer lexer(text, symbols);
  lexer.run();
  return std::move(lexer.result);
}

const char* tokenKindName(TokenKind kind)
{
  switch (kind)
  {
    case TokenKind::End: return "end";
    case TokenKind::Identifier: return "identifier";
    case TokenKind::Keyword: return "keyword";
    case TokenKind::Number: return "number";
    case TokenKind::String: return "string";
    case TokenKind::Char: return "char";
    case TokenKind::Punctuator: return "punctuator";
    case TokenKind::Directive: return "directive";
    case TokenKind::Error: return "error";
  }
  return "?";
}
