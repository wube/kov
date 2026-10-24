#pragma once
// The C++ to Kov converter: rewrites parsed and resolved C++ files into Kov sources.
// See docs/path-to-factorio.md. It does the mechanical part (preprocessor lines, forward
// declarations, `this->`, `implicit`, `fallthrough`, `std::` names) and reports what it could not
// do, so that the compiler's errors on the output are the worklist.
#include "ast.hpp"
#include "lexer.hpp"
#include "resolver.hpp"
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

struct ConvertInput
{
  std::filesystem::path path;
  std::string_view text;
  const std::vector<Token>* tokens;
  ParsedFile* parsed;
};

// Writes one .kov file per input (a header and its source of the same stem merged into one)
// under outDir, mirroring the paths below `roots`. A file of the same relative path under
// overridesDir replaces the generated one: the hand-written conversion of what the tool cannot
// do. Returns the conversion report.
std::string convertToKov(const std::vector<ConvertInput>& inputs, GlobalScope& global, const SymbolTable& symbols,
                         const std::vector<std::filesystem::path>& roots, const std::filesystem::path& outDir,
                         const std::filesystem::path& overridesDir = {});
