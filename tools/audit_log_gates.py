#!/usr/bin/env python3
"""
Find, and with --fix rewrite, "the first N times" gates whose counter goes on counting.

    static int n; if (n++ < 8) fprintf(stderr, ...);

is everywhere in the runtime: a diagnostic that is wanted a few times and never
again. The counter is incremented on every call though, and on a path that is
called often enough it passes INT_MAX, goes negative, and the gate is open
again -- for the next two thousand million calls. One of these on the lv2/HLE
dispatch path (about 100,000 calls a second in one title) turned itself on
after six hours and wrote 51 GB of one line before the run was stopped.

The cure is to stop counting once the gate has shut. --fix rewrites

    x++ <  N   ->  (x <  N && ++x)        ++x <= N  ->  (x < N && ++x)
    x++ <= N   ->  (x <= N && ++x)        ++x <  N  ->  (x + 1 < N && ++x)

which is true on exactly the same calls and leaves the counter with the same
value wherever the gate is open. A gate is rewritten only when that is all
there is to it. It is reported and left for a person when:

  - the counter is read somewhere else in its scope. Look at every such read
    before deciding: stopping at N is right when they all compare with less
    than N; `(x <= N && x++ < N)`, which stops one later, keeps a later
    `x <= N` right; a read that needs the true count (`x % 2000`, a total)
    means the counter must go on counting, so make it unsigned instead;
  - the counter is also used with % on the line (a periodic line as well: it
    has to keep counting, so make it unsigned -- that wraps to 0, not below);
  - the limit is more than a name or a number.

Reported as well: a signed static counter incremented by a statement of its
own (`n++;`) outside any block its own `<` test guards -- "print the first N,
say 'suppressed' once, count on" -- which wraps the same way; `if (n <= N)
n++;` stops it.

Unsigned counters are left alone (wrapping to 0 shows the N lines again and
that is all), as are 64-bit ones, loop conditions, and counters that are not
static. tests/ and third-party headers are not looked at.

    python tools/audit_log_gates.py [--fix] [--exclude NAME ...] [DIR ...]

DIR defaults to runtime libs include. Exits 1 if anything is reported or (without
--fix) would be rewritten, so it can stand as a check.
"""
import argparse
import os
import re
import sys

THIRD_PARTY = {"stb_image.h", "stb_image_write.h", "stb_truetype.h"}
SIGNED = r"(?:signed\s+)?(?:long|int|short|char|int32_t|int16_t|int8_t|LONG|s32|s16|s8|i32)"
OTHER = (r"(?:unsigned(?:\s+(?:long\s+long|long|int|short|char))?|long\s+long|u8|u16|u32|u64|uint8_t|uint16_t|"
         r"uint32_t|uint64_t|int64_t|s64|i64|size_t|DWORD|ULONG)")
GATE = re.compile(r"(?:\b(?P<post>[A-Za-z_]\w*)\s*\+\+|\+\+\s*(?P<pre>[A-Za-z_]\w*))\s*(?P<op><=?)\s*"
                  r"(?P<lim>[A-Za-z_]\w*|\d+[uUlL]*|0[xX][0-9a-fA-F]+)")
STRING = re.compile(r'"(\\.|[^"\\])*"|\'(\\.|[^\'\\])\'')   # and a character: '\\n' is not a use of n
COMMENT = re.compile(r"/\*.*?\*/|//.*$")


def code(line):
    """The line without what is inside its strings and comments."""
    return COMMENT.sub("", STRING.sub('""', line))


def scope_end(lines, dline, col):
    """The last line of the block a declaration at (dline, col) is in."""
    depth = 0
    for j in range(dline, min(len(lines), dline + 600)):
        for ch in code(lines[j])[col if j == dline else 0:]:
            if ch == "{":
                depth += 1
            elif ch == "}":
                depth -= 1
                if depth < 0:
                    return j
    return min(len(lines) - 1, dline + 600)


def declaration(lines, i, var):
    """(type, line, column) of the nearest static declaration of var above line i, or None."""
    pat = re.compile(r"\bstatic\s+(?:volatile\s+|const\s+)*(" + OTHER + "|" + SIGNED + r")\s+(?:[^;()]*[\s,*])?"
                     + re.escape(var) + r"\b(?!\s*\()")
    for j in range(i, max(-1, i - 400), -1):
        m = pat.search(lines[j])
        if m:
            return m.group(1), j, m.start()
    return None


def audit_file(path, fix):
    # newline="": the file's own line endings, read and written back as they are (text mode would turn
    # every \n into \r\n on Windows, and a CRLF file into an LF one elsewhere)
    text = open(path, errors="surrogateescape", newline="").read()
    lines = text.split("\n")
    rewritten, reports = 0, []

    # --- x++ < N and its kin -------------------------------------------------
    for i, line in enumerate(lines):
        if "++" not in line or "<" not in line:
            continue
        edits = []
        for m in GATE.finditer(line):
            var, lim, op = m.group("post") or m.group("pre"), m.group("lim"), m.group("op")
            before, after = line[:m.start()], line[m.end():]
            if re.search(r"\b(for|while)\s*\([^)]*$", before) or var == lim:
                continue
            decl = declaration(lines, i, var)
            if not decl or not re.fullmatch(SIGNED, decl[0]):
                continue   # not static, unsigned, or 64-bit
            # already the form that stops one past the limit
            if re.search(r"\b" + re.escape(var) + r"\s*<=\s*" + re.escape(lim) + r"\s*&&\s*$", before):
                continue
            why = None
            if not re.match(r"\s*(\)|&&|\|\||\?|;|,|$)", after):
                why = "the limit is more than a name or a number"
            elif re.search(r"\b" + re.escape(var) + r"\b\s*%|%\s*\(?\s*" + re.escape(var) + r"\b", code(line)):
                why = "the counter is also used with %: it must keep counting (make it unsigned)"
            else:
                _, dline, dcol = decl
                others = []
                for j in range(dline, scope_end(lines, dline, dcol) + 1):
                    raw = lines[j]
                    if j == dline:   # the declaration itself, wherever on its line it is
                        raw = raw[:dcol] + re.sub(r"^[^;]*;", "", raw[dcol:], count=1)
                    s = GATE.sub(lambda g: "" if (g.group("post") or g.group("pre")) == var else g.group(0), code(raw))
                    if re.search(r"\b" + re.escape(var) + r"\b", s):
                        others.append((j, s.strip()))
                # As an argument of the gate's own print it shows what the body sees, which is the same:
                # those uses are taken out of the line, and it is excused only if nothing of the counter is left.
                arg = re.compile(r"(?<=[,(])\s*" + re.escape(var) + r"\s*(?:-\s*1\s*)?(?=[,)])")
                real = [o for o in others if not (i <= o[0] <= i + 6 and arg.search(o[1])
                                                  and not re.search(r"\b" + re.escape(var) + r"\b", arg.sub("", o[1])))]
                if real:   # every one of them: the first is not always the one that matters
                    why = "the counter is read elsewhere: " + "; ".join(
                        "line %d: %s" % (j + 1, t[:60]) for j, t in real[:4]) + (" ..." if len(real) > 4 else "")
            if why:
                reports.append((i + 1, why, line.strip()))
                continue
            new = {("post", "<"): f"({var} < {lim} && ++{var})", ("post", "<="): f"({var} <= {lim} && ++{var})",
                   ("pre", "<="): f"({var} < {lim} && ++{var})", ("pre", "<"): f"({var} + 1 < {lim} && ++{var})"}[
                       ("post" if m.group("post") else "pre", op)]
            edits.append((m.start(), m.end(), new))
        if edits:
            for a, b, new in reversed(edits):
                line = line[:a] + new + line[b:]
            # if ((x < N && ++x)) reads better without the doubled parentheses
            line = re.sub(r"\bif\s*\(\((\w+ (?:\+ 1 )?<=? \w+ && \+\+\w+)\)\)", r"if (\1)", line)
            lines[i] = line
            rewritten += len(edits)

    # --- n++; on its own, outside the counter's own test ----------------------
    stripped = [code(x) for x in lines]
    decl_re = re.compile(r"\bstatic\s+(?:volatile\s+)?(" + SIGNED + r")\s+(?!long\b)([^;(]*);")   # not long long
    for i, line in enumerate(stripped):
        if not line.startswith((" ", "\t")):
            continue
        for m in decl_re.finditer(line):
            for name in re.findall(r"\b([A-Za-z_]\w*)\b\s*(?:=\s*[-\w]+)?\s*(?:,|$)", m.group(2)):
                end = scope_end(lines, i, m.start())
                cmp_re = re.compile(r"\b" + re.escape(name) + r"\s*<=?\s*[\w(]")
                inc_re = re.compile(r"(?:^|[;{}]|\)|else)\s*(?:" + re.escape(name) + r"\s*\+\+|\+\+\s*" + re.escape(name)
                                    + r"|" + re.escape(name) + r"\s*\+=\s*1)\s*;")
                if not any(cmp_re.search(stripped[j]) and not re.search(r"\b(for|while)\s*\(", stripped[j])
                           for j in range(i, end + 1)):
                    continue
                for j in range(i, end + 1):
                    for im in inc_re.finditer(stripped[j]):
                        if cmp_re.search(stripped[j][:im.start() + 1]):
                            continue   # guarded earlier on its own line
                        guarded, depth = False, 0
                        for k in range(j, i - 1, -1):
                            seg = stripped[k][:im.start()] if k == j else stripped[k]
                            for ch in reversed(seg):
                                if ch == "}":
                                    depth += 1
                                elif ch == "{":
                                    if depth == 0:
                                        if cmp_re.search(" ".join(stripped[max(i, k - 3):k + 1])):
                                            guarded = True
                                    else:
                                        depth -= 1
                            if guarded:
                                break
                        if not guarded and j > i and cmp_re.search(stripped[j - 1]) and "{" not in stripped[j - 1]:
                            guarded = True   # the one statement of the if above
                        if not guarded:
                            reports.append((j + 1, "the counter %s is incremented outside its own test" % name,
                                            lines[j].strip()))

    if fix and rewritten:
        open(path, "w", errors="surrogateescape", newline="").write("\n".join(lines))
    return rewritten, reports


def main():
    ap = argparse.ArgumentParser(description=__doc__.strip().split("\n")[0])
    ap.add_argument("dirs", nargs="*", default=["runtime", "libs", "include"], metavar="DIR")
    ap.add_argument("--fix", action="store_true", help="rewrite the gates that are plainly gates")
    ap.add_argument("--exclude", action="append", default=[], metavar="NAME", help="a file name to leave out")
    a = ap.parse_args()
    skip = THIRD_PARTY | set(a.exclude)
    total, found = 0, 0
    for root in a.dirs:
        for d, _, files in os.walk(root):
            parts = d.split(os.sep)
            if "tests" in parts or "build" in parts:
                continue
            for f in sorted(files):
                if not f.endswith((".c", ".cpp", ".h", ".m", ".mm", ".inc")) or f in skip:
                    continue
                path = os.path.join(d, f)
                n, reports = audit_file(path, a.fix)
                total += n
                for line, why, src in reports:
                    found += 1
                    print(f"{path}:{line}: {why}\n    {src[:140]}")
    print(f"{total} gate(s) {'rewritten' if a.fix else 'would be rewritten by --fix'}; {found} left for a person")
    return 1 if found or (total and not a.fix) else 0


if __name__ == "__main__":
    sys.exit(main())
