// kovc: the Kov compiler. Currently the speed-test prototype: parallel lexing of a file tree.
#include "lexer.hpp"
#include "parser.hpp"
#include "source.hpp"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

namespace
{
  struct Options
  {
    std::vector<std::filesystem::path> roots;
    std::vector<std::string> extensions = {".cpp", ".hpp", ".h", ".kov"};
    unsigned threads = std::thread::hardware_concurrency();
    bool dumpTokens = false;
    bool parse = true;
    int showErrors = 20;
    int repeat = 1;
  };

  void usage()
  {
    fprintf(stderr,
            "usage: kovc [options] <file-or-dir>...\n"
            "  -j N          worker threads (default: hardware threads)\n"
            "  --ext .x,.y   file extensions to pick up (default .cpp,.hpp,.h,.kov)\n"
            "  --repeat N    lex the corpus N times, report the best time\n"
            "  --dump        print tokens\n");
  }

  struct FileStats
  {
    uint64_t bytes = 0;
    uint64_t tokens = 0;
    uint64_t lines = 0;
    uint64_t errors = 0;
  };

  template<typename F>
  void parallelFor(size_t count, unsigned threads, F&& body)
  {
    std::atomic<size_t> next{0};
    auto worker = [&]
    {
      for (;;)
      {
        size_t i = next.fetch_add(1, std::memory_order_relaxed);
        if (i >= count)
          return;
        body(i);
      }
    };
    std::vector<std::thread> pool;
    for (unsigned t = 1; t < threads; ++t)
      pool.emplace_back(worker);
    worker();
    for (auto& t : pool)
      t.join();
  }

  double seconds(std::chrono::steady_clock::duration d);

  uint32_t lineOf(const std::string& text, uint32_t offset)
  {
    uint32_t line = 1;
    for (uint32_t i = 0; i < offset && i < text.size(); ++i)
      if (text[i] == '\n')
        ++line;
    return line;
  }

  void runParse(std::vector<SourceFile>& files, std::vector<LexResult>& lexed, SymbolTable& symbols, const Options& options)
  {
    using Clock = std::chrono::steady_clock;
    std::vector<std::unique_ptr<ParsedFile>> parsed(files.size());
    auto start = Clock::now();
    parallelFor(files.size(), options.threads, [&](size_t i)
    {
      parsed[i] = std::make_unique<ParsedFile>();
      parseFile(lexed[i].tokens, files[i].text, symbols, *parsed[i]);
    });
    double parseTime = seconds(Clock::now() - start);
    uint64_t decls = 0, bodies = 0, nodes = 0, errors = 0, filesWithErrors = 0, arenaBytes = 0, lines = 0;
    for (size_t i = 0; i < files.size(); ++i)
    {
      decls += parsed[i]->declCount;
      bodies += parsed[i]->functionBodies;
      nodes += parsed[i]->nodeCount;
      errors += parsed[i]->diagnostics.size();
      filesWithErrors += !parsed[i]->diagnostics.empty();
      arenaBytes += parsed[i]->arena.bytesAllocated();
      lines += lexed[i].lineCount;
    }
    printf("parse:   %.3f s  (%.1f M lines/s)\n", parseTime, lines / 1e6 / parseTime);
    printf("  decls: %llu, function bodies: %llu, nodes: %llu, AST memory: %.1f MB\n", (unsigned long long)decls,
           (unsigned long long)bodies, (unsigned long long)nodes, arenaBytes / 1e6);
    printf("  parse errors: %llu in %llu files (%.2f%% of files clean)\n", (unsigned long long)errors,
           (unsigned long long)filesWithErrors, 100.0 * (files.size() - filesWithErrors) / std::max<size_t>(1, files.size()));
    int shown = 0;
    for (size_t i = 0; i < files.size() && shown < options.showErrors; ++i)
      for (const Diagnostic& d : parsed[i]->diagnostics)
      {
        if (shown++ >= options.showErrors)
          break;
        const Token& t = lexed[i].tokens[std::min<size_t>(d.token, lexed[i].tokens.size() - 1)];
        printf("  %s:%u: %s (at '%.*s')\n", files[i].path.string().c_str(), lineOf(files[i].text, t.offset), d.message,
               int(std::min<uint32_t>(t.length, 30)), files[i].text.data() + t.offset);
      }
  }

  double seconds(std::chrono::steady_clock::duration d)
  {
    return std::chrono::duration<double>(d).count();
  }
}

int main(int argc, char** argv)
{
  Options options;
  for (int i = 1; i < argc; ++i)
  {
    std::string arg = argv[i];
    if (arg == "-j" && i + 1 < argc)
      options.threads = unsigned(atoi(argv[++i]));
    else if (arg == "--ext" && i + 1 < argc)
    {
      options.extensions.clear();
      std::string list = argv[++i];
      size_t pos = 0;
      while (pos <= list.size())
      {
        size_t comma = list.find(',', pos);
        if (comma == std::string::npos)
          comma = list.size();
        options.extensions.push_back(list.substr(pos, comma - pos));
        pos = comma + 1;
      }
    }
    else if (arg == "--repeat" && i + 1 < argc)
      options.repeat = atoi(argv[++i]);
    else if (arg == "--dump")
      options.dumpTokens = true;
    else if (arg == "--lex-only")
      options.parse = false;
    else if (arg == "--errors" && i + 1 < argc)
      options.showErrors = atoi(argv[++i]);
    else if (arg[0] == '-')
    {
      usage();
      return 2;
    }
    else
      options.roots.push_back(arg);
  }
  if (options.roots.empty())
  {
    usage();
    return 2;
  }
  if (options.threads == 0)
    options.threads = 1;

  using Clock = std::chrono::steady_clock;

  // Phase 0: find and read all files. Reading is I/O bound, so it is timed separately.
  auto startCollect = Clock::now();
  std::vector<std::filesystem::path> paths;
  for (const auto& root : options.roots)
  {
    auto found = collectFiles(root, options.extensions);
    paths.insert(paths.end(), found.begin(), found.end());
  }
  std::vector<SourceFile> files(paths.size());
  std::atomic<uint64_t> totalBytes{0};
  parallelFor(paths.size(), options.threads, [&](size_t i)
  {
    files[i].path = paths[i];
    if (!readFile(paths[i], files[i].text))
      fprintf(stderr, "cannot read %s\n", paths[i].string().c_str());
    totalBytes += files[i].text.size();
  });
  double collectTime = seconds(Clock::now() - startCollect);

  // Phase 1: lex everything in parallel.
  double bestLex = 1e9;
  FileStats total;
  for (int round = 0; round < options.repeat; ++round)
  {
    SymbolTable symbols;
    std::vector<FileStats> stats(files.size());
    std::vector<LexResult> results(files.size());
    auto start = Clock::now();
    parallelFor(files.size(), options.threads, [&](size_t i)
    {
      results[i] = lex(files[i].text, symbols);
      stats[i].bytes = files[i].text.size();
      stats[i].tokens = results[i].tokens.size();
      stats[i].lines = results[i].lineCount;
      stats[i].errors = results[i].errorCount;
    });
    double lexTime = seconds(Clock::now() - start);
    bestLex = std::min(bestLex, lexTime);

    total = {};
    for (const auto& s : stats)
    {
      total.bytes += s.bytes;
      total.tokens += s.tokens;
      total.lines += s.lines;
      total.errors += s.errors;
    }

    if (round == options.repeat - 1)
    {
      printf("files:   %zu\n", files.size());
      printf("bytes:   %llu (%.1f MB)\n", (unsigned long long)total.bytes, total.bytes / 1e6);
      printf("lines:   %llu\n", (unsigned long long)total.lines);
      printf("tokens:  %llu\n", (unsigned long long)total.tokens);
      printf("symbols: %zu distinct identifiers\n", symbols.size());
      printf("errors:  %llu\n", (unsigned long long)total.errors);
      printf("threads: %u\n", options.threads);
      printf("read:    %.3f s\n", collectTime);
      printf("lex:     %.3f s  (%.0f MB/s, %.1f M lines/s, %.1f M tokens/s)\n", bestLex,
             total.bytes / 1e6 / bestLex, total.lines / 1e6 / bestLex, total.tokens / 1e6 / bestLex);
      if (total.errors)
      {
        unsigned shown = 0;
        for (size_t i = 0; i < files.size() && shown < 20; ++i)
          for (const Token& t : results[i].tokens)
            if (t.getKind() == TokenKind::Error && shown++ < 20)
              printf("  error in %s at offset %u: '%.*s'\n", files[i].path.string().c_str(), t.offset,
                     int(std::min<uint32_t>(t.length, 40)), files[i].text.data() + t.offset);
      }
      if (options.parse)
        runParse(files, results, symbols, options);
      if (options.dumpTokens)
        for (size_t i = 0; i < files.size(); ++i)
        {
          printf("== %s\n", files[i].path.string().c_str());
          for (const Token& t : results[i].tokens)
            printf("%-10s %.*s\n", tokenKindName(t.getKind()), int(t.length), files[i].text.data() + t.offset);
        }
    }
  }
  return 0;
}
