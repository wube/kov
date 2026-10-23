#pragma once
// Order-independent name resolution: phase 1 indexes every file's declarations into one shared
// scope tree (in parallel), phase 2 resolves type names and identifiers in all declarations and
// function bodies against that tree (in parallel, read-only). No type checking: member accesses
// after `.`/`->` need the type of the object expression and are counted as deferred.
#include "ast.hpp"
#include <cstdint>
#include <unordered_map>
#include <vector>

struct Scope;

struct ResolveStats
{
  // Type names (in declarations, casts, template arguments...).
  uint64_t typesResolved = 0, typesDependent = 0, typesUnresolved = 0;
  // Identifiers in expressions.
  uint64_t namesLocal = 0, namesMember = 0, namesGlobal = 0, namesDependent = 0, namesUnresolved = 0;
  uint64_t membersDeferred = 0; // `a.b`, `a->b`: needs the type of `a`.
  uint64_t qualifiedUnresolvedStd = 0; // std:: and other names that are not part of the corpus.
  uint64_t unresolvedMacroLike = 0;    // ALL_CAPS identifiers: almost always macros.
  std::unordered_map<uint32_t, uint32_t> unresolvedSymbols; // symbol -> count
  struct Where { uint32_t file; uint32_t token; };
  std::unordered_map<uint32_t, Where> unresolvedFirst; // symbol -> first occurrence
  uint32_t fileIndex = 0; // Set by the driver before resolving, so first occurrences can be reported.

  void add(const ResolveStats& o);
};

class GlobalScope
{
public:
  GlobalScope();
  ~GlobalScope();

  // Phase 1, thread-safe: index all top-level declarations of one file.
  void indexFile(ParsedFile& file);
  // After all files are indexed: attach out-of-line class definitions (`struct A::B {}`).
  void finishIndex();
  // Phase 2, thread-safe and read-only on the scope tree: resolve one file.
  void resolveFile(ParsedFile& file, const SymbolTable& symbols, ResolveStats& stats);

  size_t scopeCount() const;
  size_t entryCount() const;

private:
  struct Impl;
  Impl* impl;
};
