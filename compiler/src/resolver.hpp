#pragma once
// Order-independent name resolution and type checking: phase 1 indexes every file's declarations
// into one shared scope tree (in parallel), phase 2 resolves type names and identifiers in all
// declarations and function bodies against that tree and computes the type of every expression
// (in parallel; the tree is read-only, canonical types are interned in a shared table).
#include "ast.hpp"
#include "types.hpp"
#include <cstdint>
#include <string_view>
#include <unordered_map>
#include <vector>

struct Scope;

struct ResolveStats
{
  // Type names (in declarations, casts, template arguments...).
  uint64_t typesResolved = 0, typesDependent = 0, typesUnresolved = 0, typesBuiltinByName = 0;
  // Identifiers in expressions.
  uint64_t namesLocal = 0, namesMember = 0, namesGlobal = 0, namesDependent = 0, namesUnresolved = 0;
  uint64_t qualifiedUnresolvedStd = 0; // std:: and other names that are not part of the corpus.
  uint64_t unresolvedMacroLike = 0;    // ALL_CAPS identifiers: almost always macros.
  std::unordered_map<uint32_t, uint32_t> unresolvedSymbols; // symbol -> count
  struct Where { uint32_t file; uint32_t token; };
  std::unordered_map<uint32_t, Where> unresolvedFirst; // symbol -> first occurrence
  uint32_t fileIndex = 0; // Set by the driver before resolving, so first occurrences can be reported.

  // Member accesses `a.b` / `a->b` / `this->b`, bound through the type of the object.
  uint64_t membersBound = 0, membersDependent = 0, membersUnknownBase = 0, membersNotFound = 0;
  std::unordered_map<uint32_t, uint32_t> missingMembers; // symbol -> count (known class, no such member)
  std::unordered_map<uint32_t, Where> missingMembersFirst;
  std::unordered_map<uint32_t, uint32_t> unknownBaseMembers; // member name -> count, when the object type is unknown

  // Expressions.
  uint64_t exprsTyped = 0, exprsDependent = 0, exprsUnknown = 0;
  // Why an expression's type is unknown (first cause wins).
  uint64_t unknownName = 0, unknownBase = 0, unknownMember = 0, unknownOverload = 0, unknownDeduction = 0,
           unknownAutoReturn = 0, unknownUnsupported = 0, unknownOperator = 0, unknownAuto = 0;
  uint64_t callsResolved = 0, callsAmbiguousArity = 0;

  void add(const ResolveStats& o);
};

class GlobalScope
{
public:
  explicit GlobalScope(SymbolTable& symbols);
  ~GlobalScope();

  // Phase 1, thread-safe: index all top-level declarations of one file.
  void indexFile(ParsedFile& file);
  // After all files are indexed: attach out-of-line class definitions (`struct A::B {}`).
  void finishIndex();
  // Phase 2, thread-safe and read-only on the scope tree: resolve and type one file.
  void resolveFile(ParsedFile& file, const std::vector<Token>& tokens, std::string_view text, ResolveStats& stats);

  size_t scopeCount() const;
  size_t entryCount() const;
  size_t typeCount() const;

private:
  struct Impl;
  Impl* impl;
};
