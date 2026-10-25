"""The feature specification suite of kovc.

Every compiler/tests/spec/<area>/<name>.kov is one small program showing one construct; it is
compiled with build/kovc.exe, run, and its output compared with <name>.expected. The header of a
snippet says what it is about:

    // spec: default arguments
    // factorio: 32235 uses in 4539 files        (from the construct survey, optional)
    // pending: no generic lambdas yet           (a known gap: the snippet is expected to fail)

Statuses: pass, FAIL (a regression), pending (a known gap, still failing), fixed (a pending
snippet that passes: remove its pending line). The run writes docs/spec-status.md, the
specification of what kovc can and cannot do yet, with the first compiler complaint of every gap.

    python compiler\\tests\\spec\\run_spec.py [--filter TEXT] [--jobs N] [--no-doc]
"""
import argparse, os, re, subprocess, sys, concurrent.futures, time

HERE = os.path.dirname(os.path.abspath(__file__))
COMPILER = os.path.dirname(os.path.dirname(HERE))
ROOT = os.path.dirname(COMPILER)
KOVC = os.path.join(COMPILER, "build", "kovc.exe")
OUT = os.path.join(COMPILER, "build", "spec")
DOC = os.path.join(ROOT, "docs", "spec-status.md")

AREA_TITLES = {
  "language": "Language: statements, expressions, declarations",
  "classes": "Classes and objects",
  "templates": "Templates",
  "library": "Standard library (what std:: converts to)",
  "kov": "Kov's own features (design.md sections 7 and 8)",
  "interop": "C interop",
}


def header(path):
    info = {"spec": os.path.splitext(os.path.basename(path))[0], "factorio": "", "pending": None}
    with open(path, encoding="utf-8") as f:
        for line in f:
            m = re.match(r"//\s*(spec|factorio|pending):\s*(.*)", line.strip())
            if not m:
                if line.strip() and not line.startswith("//"):
                    break
                continue
            info[m.group(1)] = m.group(2).strip()
    return info


def normalise(text):
    return "\n".join(l.rstrip() for l in text.replace("\r\n", "\n").split("\n")).strip("\n")


def first_complaint(text):
    for line in text.replace("\r\n", "\n").split("\n"):
        s = line.strip()
        if s and not s.startswith(("files:", "bytes:", "lines:", "tokens:", "symbols:", "errors:", "threads:", "read:", "lex:", "parse:", "index:", "resolve:", "check:", "instantiate:", "codegen:", "clang:", "imports:")):
            return s
    return text.strip().split("\n")[0] if text.strip() else ""


def run_one(path):
    area = os.path.basename(os.path.dirname(path))
    name = os.path.splitext(os.path.basename(path))[0]
    info = header(path)
    exe = os.path.join(OUT, f"{area}.{name}.exe")
    expected_path = os.path.splitext(path)[0] + ".expected"
    result = {"area": area, "name": name, "info": info, "note": ""}
    try:
        c = subprocess.run([KOVC, "--quiet", "--exe", exe, path], capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=300)
    except subprocess.TimeoutExpired:
        result["status"] = "FAIL"; result["note"] = "compiler timed out"; return finish(result)
    if c.returncode != 0 or not os.path.exists(exe):
        result["status"] = "FAIL"
        if c.returncode not in (0, 1):
            result["note"] = f"the compiler crashed (exit 0x{c.returncode & 0xFFFFFFFF:X})"
        else:
            result["note"] = first_complaint(c.stdout + c.stderr) or "compilation failed"
        return finish(result)
    try:
        r = subprocess.run([exe], capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=60)
    except subprocess.TimeoutExpired:
        result["status"] = "FAIL"; result["note"] = "program timed out"; return finish(result)
    if r.returncode != 0 and r.returncode > 1:
        result["status"] = "FAIL"; result["note"] = f"program exited with 0x{r.returncode & 0xFFFFFFFF:X}"
        return finish(result)
    expected = open(expected_path, encoding="utf-8").read() if os.path.exists(expected_path) else ""
    if normalise(r.stdout) == normalise(expected):
        result["status"] = "pass"
    else:
        result["status"] = "FAIL"
        got, want = normalise(r.stdout).split("\n"), normalise(expected).split("\n")
        for i in range(max(len(got), len(want))):
            g = got[i] if i < len(got) else "<end>"
            w = want[i] if i < len(want) else "<end>"
            if g != w:
                result["note"] = f"line {i + 1}: got `{g}`, expected `{w}`"
                break
    return finish(result)


def finish(result):
    pending = result["info"]["pending"]
    if result["status"] == "FAIL" and pending is not None:
        result["status"] = "pending"
    elif result["status"] == "pass" and pending is not None:
        result["status"] = "fixed"
    return result


def write_doc(results, elapsed):
    counts = {s: sum(1 for r in results if r["status"] == s) for s in ("pass", "pending", "FAIL", "fixed")}
    lines = ["# What kovc compiles: the specification suite", "",
             "Generated by `compiler\\tests\\spec.cmd` from `compiler/tests/spec/<area>/*.kov`: one small program per",
             "construct, compiled, run and compared with its `.expected` output. The Factorio column is the",
             "construct's count in the game's sources (`src`, `CommonUtil`, `Agui`; 7,371 files), so the gaps are",
             "ordered by what the conversion needs. A pending row is a known gap and shows the compiler's first",
             "complaint; it turns into a failure of the suite the day it starts passing, so the row gets removed.", "",
             f"**{counts['pass']} pass, {counts['pending']} pending, {counts['FAIL']} failing, {counts['fixed']} fixed** "
             f"({len(results)} snippets, {elapsed:.1f} s).", ""]
    for area in sorted({r["area"] for r in results}, key=lambda a: list(AREA_TITLES).index(a) if a in AREA_TITLES else 99):
        rows = sorted((r for r in results if r["area"] == area), key=lambda r: (r["status"] == "pass", r["name"]))
        lines += [f"## {AREA_TITLES.get(area, area)}", "", "| Snippet | Construct | Status | Factorio | Note |", "|---|---|---|---|---|"]
        for r in rows:
            note = r["info"]["pending"] or ""
            if r["status"] in ("pending", "FAIL") and r["note"]:
                note = (note + "; " if note else "") + r["note"].replace("|", "\\|")
            if r["status"] == "fixed":
                note = "passes now: remove the pending line"
            lines.append(f"| `{area}/{r['name']}.kov` | {r['info']['spec']} | {r['status']} | {r['info']['factorio']} | {note} |")
        lines.append("")
    os.makedirs(os.path.dirname(DOC), exist_ok=True)
    with open(DOC, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--filter", default="")
    ap.add_argument("--jobs", type=int, default=8)
    ap.add_argument("--no-doc", action="store_true")
    args = ap.parse_args()
    if not os.path.exists(KOVC):
        print("no compiler at", KOVC); return 2
    os.makedirs(OUT, exist_ok=True)
    files = sorted(os.path.join(dp, f) for dp, dn, fn in os.walk(HERE) for f in fn
                   if f.endswith(".kov") and args.filter in os.path.relpath(os.path.join(dp, f), HERE).replace("\\", "/"))
    start = time.time()
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
        results = list(pool.map(run_one, files))
    elapsed = time.time() - start
    for r in results:
        tag = {"pass": "ok     ", "pending": "pending", "FAIL": "FAIL   ", "fixed": "FIXED  "}[r["status"]]
        line = f"{tag} {r['area']}/{r['name']}"
        if r["status"] in ("FAIL", "pending") and r["note"]:
            line += f"  -- {r['note']}"
        print(line)
    counts = {s: sum(1 for r in results if r["status"] == s) for s in ("pass", "pending", "FAIL", "fixed")}
    print(f"{counts['pass']} pass, {counts['pending']} pending, {counts['FAIL']} failing, {counts['fixed']} fixed ({elapsed:.1f} s)")
    if not args.no_doc and not args.filter:
        write_doc(results, elapsed)
        print("wrote", os.path.relpath(DOC, ROOT))
    return 1 if counts["FAIL"] or counts["fixed"] else 0


if __name__ == "__main__":
    sys.exit(main())
