#!/usr/bin/env python3
"""Property/differential test runner for tinyobjloader.

Phase 1 (robustness): run 200 malformed OBJ files through obj_dump built
with ASan+UBSan+LSan. Require: zero crashes, zero sanitizer reports
(out-of-bounds / UB), zero leaks. Exit code 0 (parsed) or 3 (rejected with
a line-numbered error) are both acceptable.

Phase 2 (differential): parse 50 valid OBJ files with obj_dump and with the
reference parser (ref_parse.py); compare the canonical dumps item by item.

Writes summary.md and counterexamples/ under the work directory.
"""

import difflib
import os
import re
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
BUILD = os.path.join(HERE, "build")
CASES = os.path.join(HERE, "cases")
COUNTER = os.path.join(HERE, "counterexamples")
OBJ_DUMP = os.path.join(BUILD, "obj_dump")

SANITIZER_PATTERNS = [
    "AddressSanitizer", "LeakSanitizer", "runtime error:",
    "UndefinedBehaviorSanitizer", "SEGV", "heap-buffer-overflow",
    "stack-buffer-overflow", "use-after-free", "detected memory leaks",
]

CXX = os.environ.get("CXX", "c++")
CXXFLAGS = [
    "-std=c++11", "-O1", "-g", "-fno-omit-frame-pointer",
    "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
    "-DTINYOBJLOADER_USE_DOUBLE",
    "-I", ROOT,
]

ENV = dict(os.environ)
ENV["ASAN_OPTIONS"] = "detect_leaks=1:exitcode=1:abort_on_error=0"
ENV["UBSAN_OPTIONS"] = "print_stacktrace=1:halt_on_error=1"
ENV["LSAN_OPTIONS"] = "report_objects=0"

TIMEOUT = 60


def build():
    os.makedirs(BUILD, exist_ok=True)
    cmd = [CXX] + CXXFLAGS + [os.path.join(HERE, "obj_dump.cc"), "-o", OBJ_DUMP]
    print("build:", " ".join(cmd))
    subprocess.check_call(cmd)


def run_one(path):
    """Returns (rc, stdout, stderr)."""
    proc = subprocess.run([OBJ_DUMP, path], capture_output=True,
                          timeout=TIMEOUT, env=ENV)
    return proc.returncode, proc.stdout.decode("latin-1"), \
        proc.stderr.decode("latin-1")


def sanitizer_hit(stderr):
    return [p for p in SANITIZER_PATTERNS if p in stderr]


def extract_error_lines(stderr):
    """Pull source/input line numbers out of parser/sanitizer messages."""
    hits = re.findall(r":(\d+)(?::\d+)?:\s*(?:error|warning)", stderr)
    hits += re.findall(r"\(line (\d+)", stderr)
    hits += re.findall(r"line: (\d+)", stderr)
    return sorted(set(int(h) for h in hits))


def phase_malformed(mal_dir):
    results = []
    failures = []
    files = sorted(os.listdir(mal_dir))
    for name in files:
        path = os.path.join(mal_dir, name)
        cat = name.split("_")[2].split(".")[0]
        try:
            rc, out, err = run_one(path)
        except subprocess.TimeoutExpired:
            results.append((name, cat, "TIMEOUT", []))
            failures.append((name, cat, "timeout after %ds" % TIMEOUT, ""))
            continue
        san = sanitizer_hit(err)
        err_lines = extract_error_lines(err)
        if san or rc not in (0, 3):
            status = "CRASH" if rc not in (0, 3) and not san else "SANITIZER"
            results.append((name, cat, status, err_lines))
            failures.append((name, cat,
                             "rc=%d sanitizer=%s" % (rc, ",".join(san) or "-"),
                             err))
        else:
            status = "PARSED" if rc == 0 else "REJECTED"
            results.append((name, cat, status, err_lines))
    return results, failures


def phase_valid(valid_dir):
    mismatches = []
    files = sorted(os.listdir(valid_dir))
    n_ok = 0
    for name in files:
        path = os.path.join(valid_dir, name)
        rc, dump_cpp, err = run_one(path)
        if rc != 0:
            mismatches.append((name, "obj_dump rc=%d\n%s" % (rc, err), ""))
            continue
        ref = subprocess.run([sys.executable,
                              os.path.join(HERE, "ref_parse.py"), path],
                             capture_output=True, timeout=TIMEOUT)
        if ref.returncode != 0:
            mismatches.append((name,
                               "reference parser rejected: %s"
                               % ref.stderr.decode("latin-1"), ""))
            continue
        dump_ref = ref.stdout.decode("latin-1")
        if dump_cpp == dump_ref:
            n_ok += 1
            continue
        diff = "\n".join(difflib.unified_diff(
            dump_ref.splitlines(), dump_cpp.splitlines(),
            fromfile="reference", tofile="tinyobjloader", lineterm=""))
        hdr = ("+" * 3, "-" * 3)
        first = next((l for l in diff.splitlines()
                      if l.startswith(("+", "-"))
                      and not l.startswith(hdr)), "")
        mismatches.append((name, "first diff: %s" % first, diff))
    return n_ok, mismatches


def main():
    sys.path.insert(0, HERE)
    import gen_cases

    build()
    valid_dir, mal_dir = gen_cases.generate(CASES)

    if os.path.isdir(COUNTER):
        shutil.rmtree(COUNTER)
    os.makedirs(COUNTER)

    print("phase 1: %d malformed files" % len(os.listdir(mal_dir)))
    mal_results, mal_failures = phase_malformed(mal_dir)

    print("phase 2: %d valid files" % len(os.listdir(valid_dir)))
    n_valid_ok, mismatches = phase_valid(valid_dir)

    # ---- counterexamples ----
    for name, why, detail in mal_failures:
        dst = os.path.join(COUNTER, name)
        shutil.copy(os.path.join(mal_dir, name), dst)
        with open(dst + ".txt", "w") as f:
            f.write("reason: %s\n\n%s\n" % (why, detail[:20000]))
    for name, why, diff in mismatches:
        dst = os.path.join(COUNTER, name)
        shutil.copy(os.path.join(valid_dir, name), dst)
        with open(dst + ".diff", "w") as f:
            f.write("%s\n\n%s\n" % (why, diff[:20000]))

    # ---- summary ----
    by_cat = {}
    for name, cat, status, err_lines in mal_results:
        by_cat.setdefault(cat, {}).setdefault(status, []).append(
            (name, err_lines))

    lines = []
    lines.append("# tinyobjloader property test summary")
    lines.append("")
    lines.append("Binary: `%s` (ASan+UBSan+LSan, `-DTINYOBJLOADER_USE_DOUBLE`)"
                 % os.path.relpath(OBJ_DUMP, ROOT))
    lines.append("")
    lines.append("## Phase 1: malformed corpus (%d files)" % len(mal_results))
    lines.append("")
    lines.append("| category | parsed | rejected (line-numbered error) "
                 "| crash/sanitizer |")
    lines.append("|---|---|---|---|")
    for cat in ("trunc", "negidx", "badface", "hugeverts"):
        st = by_cat.get(cat, {})
        bad = len(st.get("CRASH", [])) + len(st.get("SANITIZER", [])) \
            + len(st.get("TIMEOUT", []))
        lines.append("| %s | %d | %d | %d |" % (
            cat, len(st.get("PARSED", [])), len(st.get("REJECTED", [])),
            bad))
    lines.append("")
    n_bad = len(mal_failures)
    lines.append("Crashes / sanitizer reports / leaks: **%d**" % n_bad)
    lines.append("")
    if mal_failures:
        lines.append("### Failures (with input line numbers)")
        lines.append("")
        for name, cat, why, detail in mal_failures:
            err_lines = extract_error_lines(detail)
            loc = ("input line(s) %s" % err_lines) if err_lines \
                else "no line info"
            lines.append("- `%s` (%s): %s -- %s" % (name, cat, why, loc))
        lines.append("")
    rejected = [(n, el) for n, c, s, el in mal_results if s == "REJECTED"]
    lines.append("Rejected files all carry parser line numbers; sample:")
    lines.append("")
    for name, err_lines in rejected[:10]:
        lines.append("- `%s`: rejected at input line(s) %s"
                     % (name, err_lines or "?"))
    lines.append("")
    lines.append("## Phase 2: differential vs reference (%d files)"
                 % (n_valid_ok + len(mismatches)))
    lines.append("")
    lines.append("Identical dumps: **%d**, mismatches: **%d**"
                 % (n_valid_ok, len(mismatches)))
    lines.append("")
    if mismatches:
        lines.append("### Mismatches")
        lines.append("")
        for name, why, _ in mismatches:
            lines.append("- `%s`: %s" % (name, why))
        lines.append("")
    n_counter = len(os.listdir(COUNTER))
    lines.append("Counterexample artifacts: `%s` (%d files)" % (
        os.path.relpath(COUNTER, ROOT), n_counter))

    summary = "\n".join(lines) + "\n"
    with open(os.path.join(HERE, "summary.md"), "w") as f:
        f.write(summary)
    sys.stdout.write(summary)

    return 1 if (mal_failures or mismatches) else 0


if __name__ == "__main__":
    sys.exit(main())
