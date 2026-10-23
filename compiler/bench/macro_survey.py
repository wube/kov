"""Survey of preprocessor macro usage in the Factorio corpus (read-only).

Collects every #define, classifies it by what it looks like, counts how often each macro name is
used in the code, and summarises the #if conditions. Writes a report to macro_survey.txt next to
this script and prints the headline numbers.
"""
import os, re, sys, collections, json

ROOTS = [r"C:\Factorio\src", r"C:\Factorio\tests", r"C:\Factorio\libraries\CommonUtil", r"C:\Factorio\libraries\Agui"]
EXTS = {".cpp", ".hpp", ".h"}
OUT = os.path.join(os.getcwd(), "macro_survey.txt")

files = []
for root in ROOTS:
    for dirpath, dirnames, filenames in os.walk(root):
        for fn in filenames:
            if os.path.splitext(fn)[1] in EXTS:
                files.append(os.path.join(dirpath, fn))

texts = {}
for f in files:
    with open(f, "rb") as fh:
        texts[f] = fh.read().decode("utf-8", "replace")

define_re = re.compile(r"^[ \t]*#[ \t]*define[ \t]+([A-Za-z_]\w*)(\([^)]*\))?[ \t]*(.*)$")
cond_re = re.compile(r"^[ \t]*#[ \t]*(if|ifdef|ifndef|elif|else|endif|pragma|include|undef|error|warning|line)\b[ \t]*(.*)$")
ident_re = re.compile(r"[A-Za-z_]\w*")
comment_re = re.compile(r"//[^\n]*|/\*.*?\*/", re.S)
string_re = re.compile(r'"(?:[^"\\\n]|\\.)*"')

defs = []            # dicts
directives = collections.Counter()
cond_symbols = collections.Counter()
defines_per_file = collections.Counter()
lines_total = 0

def logical_lines(text):
    """Yields (line_number, logical_line) with backslash continuations joined."""
    lines = text.split("\n")
    i = 0
    n = len(lines)
    while i < n:
        start = i
        line = lines[i].rstrip("\r")
        parts = []
        while line.endswith("\\") and i + 1 < n:
            parts.append(line[:-1])
            i += 1
            line = lines[i].rstrip("\r")
        parts.append(line)
        yield start + 1, " ".join(parts), len(parts)
        i += 1

for f, text in texts.items():
    lines_total += text.count("\n")
    for lineno, logical, physical in logical_lines(text):
        m = define_re.match(logical)
        if m:
            name, params, body = m.group(1), m.group(2), m.group(3)
            body = body.replace("\\\n", " ")
            body = re.sub(r"[ \t]+", " ", body).strip()
            defs.append({"name": name, "params": params, "body": body, "file": f, "line": lineno, "lines": physical})
            defines_per_file[f] += 1
            directives["define"] += 1
            continue
        c = cond_re.match(logical)
        if c:
            kind, rest = c.group(1), c.group(2)
            directives[kind] += 1
            if kind in ("if", "ifdef", "ifndef", "elif"):
                rest = rest.split("//")[0]
                for sym in ident_re.findall(rest):
                    if sym in ("defined", "and", "or", "not"):
                        continue
                    cond_symbols[sym] += 1

# Usage counts: identifiers in code with comments and strings removed.
usage = collections.Counter()
for f, text in texts.items():
    code = comment_re.sub(" ", text)
    code = string_re.sub('""', code)
    usage.update(ident_re.findall(code))

names = collections.Counter(d["name"] for d in defs)
by_name = collections.defaultdict(list)
for d in defs:
    by_name[d["name"]].append(d)

# Uses = occurrences minus definition lines (each definition mentions the name once).
def uses_of(name):
    return max(0, usage.get(name, 0) - names[name])

MODIFIER_NAMES = re.compile(r"(FORCE_?INLINE|NO_?INLINE|LIKELY|UNLIKELY|UNUSED|DEPRECATED|NORETURN|ALIGN|PACKED|RESTRICT|EXPORT|IMPORT|_API$|CALL$|NODISCARD|FALLTHROUGH|THREAD_LOCAL|HOT$|COLD$|VISIBILITY|CONSTEXPR|NOEXCEPT|OVERRIDE|FINAL|PRINTF|FORMAT_ATTR|WEAK|SECTION|DLL|CDECL|STDCALL|INLINE$|PURE|FLATTEN|ALWAYS_INLINE|NO_SANITIZE|DISABLE_WARNING|PUSH_WARNING|POP_WARNING|PRAGMA)")
MODIFIER_BODIES = re.compile(r"^(__forceinline|__declspec\(.*\)|__attribute__\(\(.*\)\)|inline|\[\[.*\]\]|__builtin_expect\(.*\)|__restrict|noexcept|constexpr|__pragma\(.*\)|_Pragma\(.*\)|static inline|extern \"C\"|__cdecl|__stdcall|__vectorcall)$")
ASSERT_RE = re.compile(r"(ASSERT|CHECK|VERIFY|EXPECT|REQUIRE|FAIL|ABORT|LOG|TRACE|ERROR|WARN|THROW|PANIC|DEBUG_BREAK|UNREACHABLE)")
TEST_RE = re.compile(r"^(TEST|CHECK|REQUIRE|EXPECT|SECTION|BENCHMARK|SCENARIO|GIVEN|WHEN|THEN|CATCH_|DOCTEST_|TEST_CASE)")
LUA_RE = re.compile(r"LUA|Lua")
NUMBER_RE = re.compile(r"^[-+(]*\s*(0[xX][0-9a-fA-F']+|[0-9][0-9']*\.?[0-9]*([eE][-+]?[0-9]+)?)[uUlLfF]*\s*[)]*$")
CONST_EXPR_RE = re.compile(r"^[-+*/%()<>|&~^ 0-9a-fA-FxXuUlL.'eE]+$")

def classify(d):
    name, params, body, f = d["name"], d["params"], d["body"], d["file"]
    kinds = []
    if params is None and body == "":
        if re.search(r"(_H|_HPP|_INCLUDED|_HEADER)$", name):
            return ["include-guard"]
        return ["flag (empty, used in #if)"]
    if MODIFIER_BODIES.match(body) or (MODIFIER_NAMES.search(name) and len(body) < 80 and "(" not in name):
        return ["compiler modifier / attribute"]
    if params is None and (NUMBER_RE.match(body) or CONST_EXPR_RE.match(body) or string_re.fullmatch(body) or body in ("true", "false", "nullptr")):
        return ["constant"]
    if TEST_RE.match(name) or os.sep + "tests" + os.sep in f.lower() or os.sep + "Tests" + os.sep in f:
        kinds.append("test framework")
    if "##" in body or re.search(r"(^|[^#])#[A-Za-z_]", body):
        kinds.append("stringify / token paste")
    if "__VA_ARGS__" in body or (params and "..." in params):
        kinds.append("variadic")
    if "__LINE__" in body or "__FILE__" in body or "__func" in body or "__FUNCTION__" in body or "__COUNTER__" in body:
        kinds.append("source location / unique name")
    if ASSERT_RE.search(name) or re.search(r"\b(assert|abort|throw|LOG|log\()", body):
        kinds.append("assert / log / error")
    if LUA_RE.search(name) or "lua_" in body or os.sep + "Lua" in f or os.sep + "Scripting" in f:
        kinds.append("lua binding")
    if params:
        pnames = [p.strip() for p in params.strip("()").split(",") if p.strip() and p.strip() != "..."]
        if any(re.search(r"\b" + re.escape(p) + r"\s*\(", body) for p in pnames):
            kinds.append("x-macro (parameter is applied)")
    calls = re.findall(r"\b([A-Z][A-Z0-9_]{2,})\s*\(", body)
    if calls:
        cnt = collections.Counter(calls).most_common(1)[0]
        if cnt[1] >= 3:
            kinds.append("list of invocations (x-macro list)")
    if re.search(r"\b(template|class|struct|enum|union|namespace|typedef|using|friend|virtual|static|operator|public:|private:|protected:)\b", body) or re.search(r"\b(void|bool|int|auto|const)\s+[A-Za-z_]\w*\s*\(", body):
        kinds.append("declares code (class/function/enum/template)")
    if re.search(r"\bcase\b|\bswitch\b|\breturn\b|\bfor\s*\(|\bif\s*\(|\bwhile\s*\(|do\s*\{", body):
        kinds.append("statement / control flow")
    if params is None and re.fullmatch(r"[A-Za-z_][\w:<>, ]*", body) and body not in ("true", "false", "nullptr"):
        kinds.append("alias of a name")
    if params and re.fullmatch(r"[A-Za-z_][\w:]*\s*\(.*\)", body) and "##" not in body:
        kinds.append("call forwarder / alias")
    if not kinds:
        kinds.append("other expression")
    return kinds

for d in defs:
    d["kinds"] = classify(d)
    d["uses"] = uses_of(d["name"])

# Aggregate by primary kind (first), counting definitions and uses once per name.
kind_defs = collections.Counter()
kind_uses = collections.Counter()
kind_examples = collections.defaultdict(list)
seen = set()
for d in sorted(defs, key=lambda d: -d["uses"]):
    k = d["kinds"][0]
    kind_defs[k] += 1
    if d["name"] not in seen:
        kind_uses[k] += d["uses"]
        seen.add(d["name"])
    if len(kind_examples[k]) < 12:
        kind_examples[k].append(d)

rel = lambda f: os.path.relpath(f, r"C:\Factorio")

with open(OUT, "w", encoding="utf-8") as out:
    w = lambda *a: print(*a, file=out)
    w("files", len(files), "lines", lines_total)
    w("directives", dict(directives.most_common()))
    w("defines", len(defs), "distinct names", len(names), "function-like", sum(1 for d in defs if d["params"]),
      "object-like", sum(1 for d in defs if not d["params"]))
    w("multi-line defines", sum(1 for d in defs if d["lines"] > 1), "max lines", max(d["lines"] for d in defs))
    unused = [n for n in names if uses_of(n) == 0]
    w("names with zero uses in the corpus (set for #if or dead)", len(unused))
    w("names defined more than once", sum(1 for n, c in names.items() if c > 1))
    w()
    w("== uses by kind (definitions / use sites)")
    for k, c in kind_defs.most_common():
        w(f"  {kind_uses[k]:7d} uses  {c:5d} defs  {k}")
    w()
    w("== files with most #defines")
    for f, c in defines_per_file.most_common(25):
        w(f"  {c:4d}  {rel(f)}")
    w()
    w("== #if condition symbols")
    for s, c in cond_symbols.most_common(45):
        w(f"  {c:5d}  {s}")
    w()
    w("== top 120 macros by use count")
    done = set()
    for d in sorted(defs, key=lambda d: -d["uses"]):
        if d["name"] in done:
            continue
        done.add(d["name"])
        if len(done) > 120:
            break
        w(f"  {d['uses']:6d}  {d['name']}{d['params'] or ''}  [{', '.join(d['kinds'])}]  {rel(d['file'])}:{d['line']}")
        w(f"          = {d['body'][:150]}")
    w()
    for k in kind_defs:
        w(f"== examples: {k}")
        for d in kind_examples[k]:
            w(f"  {d['uses']:6d}  {d['name']}{d['params'] or ''}  {rel(d['file'])}:{d['line']}")
            w(f"          = {d['body'][:160]}")
        w()
    w("== ALL-CAPS identifiers used but not defined in the corpus (external macros / enumerators), top 80")
    ext = [(n, c) for n, c in usage.items() if re.fullmatch(r"[A-Z][A-Z0-9_]{2,}", n) and n not in names]
    for n, c in sorted(ext, key=lambda x: -x[1])[:80]:
        w(f"  {c:6d}  {n}")
    w()
    w("== macros defined per top-level directory")
    per_dir = collections.Counter()
    per_dir_uses = collections.Counter()
    for d in defs:
        parts = rel(d["file"]).split(os.sep)
        key = os.sep.join(parts[:2])
        per_dir[key] += 1
        per_dir_uses[key] += d["uses"]
    for k, c in per_dir.most_common(40):
        w(f"  {c:5d} defs {per_dir_uses[k]:7d} uses  {k}")

json.dump([{k: (rel(v) if k == "file" else v) for k, v in d.items()} for d in defs], open(OUT.replace(".txt", ".json"), "w"), indent=0)
print("files", len(files), "defines", len(defs), "distinct", len(names), "report", OUT)
