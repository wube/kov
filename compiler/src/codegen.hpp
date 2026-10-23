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
  std::string ir;                   // the whole program as one module (moduleCount == 1)
  std::vector<std::string> modules; // or one module per group of files, each complete on its own
  std::vector<std::string> errors; // "file:line: message"
  size_t functions = 0;
  size_t structs = 0;
};

// debug: config::debug; `assert` statements compile to nothing when it is false.
// moduleCount > 1 splits the output into that many modules (functions by file, instantiations
// with the function that asked for them) so that LLVM can compile them in parallel.
CodegenResult generateLLVM(std::vector<std::unique_ptr<ParsedFile>>& files, GlobalScope& global, const SymbolTable& symbols,
                           bool debug = true, unsigned moduleCount = 1);
