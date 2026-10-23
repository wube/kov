#pragma once
#include "ast.hpp"
#include <cstdint>
#include <map>
#include <string>
#include "lexer.hpp"
#include <string_view>

// Parses one lexed file into out. Tokens may be modified (`>>` is split into two `>` when it
// closes nested template argument lists).
// Build configuration constants (`config::name`). The parser evaluates conditions made of them
// for `if constexpr` at declaration scope and `requires` at the top of a file.
struct ConfigValue
{
  bool isNumber = true; // false: an enumerator or string, compared by its spelling
  int64_t number = 0;
  std::string text;
};
using ConfigValues = std::map<std::string, ConfigValue>;

// cMode: the text is preprocessed C (imported headers): every declaration gets C linkage.
void parseFile(std::vector<Token>& tokens, std::string_view text, SymbolTable& symbols, ParsedFile& out, bool cMode = false,
               const ConfigValues* config = nullptr);
