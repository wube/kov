#pragma once
// Code generation for the Kov v0 subset: textual LLVM IR for the whole program, compiled to
// machine code by the bundled clang. See docs/design.md, "Kov v0".
#include "ast.hpp"
#include "resolver.hpp"
#include <memory>
#include <string>
#include <vector>

struct CodegenResult
{
  std::string ir;
  std::vector<std::string> errors; // "file:line: message"
  size_t functions = 0;
  size_t structs = 0;
};

// debug: config::debug; `assert` statements compile to nothing when it is false.
CodegenResult generateLLVM(std::vector<std::unique_ptr<ParsedFile>>& files, GlobalScope& global, const SymbolTable& symbols,
                           bool debug = true);
