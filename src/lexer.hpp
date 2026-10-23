#pragma once
#include <cstdint>
#include <string_view>
#include <vector>

enum class TokenKind : uint8_t
{
  End,
  Identifier,
  Keyword,
  Number,
  String,
  Char,
  Punctuator,
  Directive, // A whole preprocessor line, from '#' to the end of line (continuations included).
  Error,
};

// Compact token: 12 bytes. Text is recovered from the file by offset+length.
struct Token
{
  uint32_t offset;
  uint32_t length : 24;
  uint32_t kind : 8;
  // Identifier: interned symbol id. Keyword: keyword id. Punctuator: punctuator id. Else 0.
  uint32_t id;

  TokenKind getKind() const { return TokenKind(kind); }
};

struct LexResult
{
  std::vector<Token> tokens;
  uint32_t lineCount = 0;
  uint32_t errorCount = 0;
};

// Interns identifier spellings to dense ids, safe to call from many threads at once.
class SymbolTable
{
public:
  SymbolTable();
  ~SymbolTable();
  uint32_t intern(std::string_view name);
  std::string_view name(uint32_t id) const;
  size_t size() const;

private:
  struct Impl;
  Impl* impl;
};

// Keyword and punctuator ids are indices into these tables.
std::string_view keywordSpelling(uint32_t id);
std::string_view punctuatorSpelling(uint32_t id);
// Returns the keyword id or UINT32_MAX.
uint32_t lookupKeyword(std::string_view spelling);

// text must be followed by a '\0' sentinel (std::string guarantees it).
LexResult lex(std::string_view text, SymbolTable& symbols);

const char* tokenKindName(TokenKind kind);
