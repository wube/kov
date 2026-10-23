#pragma once
#include "ast.hpp"
#include "lexer.hpp"
#include <string_view>

// Parses one lexed file into out. Tokens may be modified (`>>` is split into two `>` when it
// closes nested template argument lists).
void parseFile(std::vector<Token>& tokens, std::string_view text, SymbolTable& symbols, ParsedFile& out);
