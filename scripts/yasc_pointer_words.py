#!/usr/bin/env python3
"""Rewrite yasc pointer words to the glued form (docs/yasc_cheatsheet.md).

    char * owned maybe p        ->  char *?owned p
    char * maybe p              ->  char *?p
    char * const owned p        ->  char *@(const, owned) p
    int a[maybe 4]              ->  int a[?4]
    int drop owned(close) maybe fd  ->  int drop ?owned(close) fd

Usage: yasc_pointer_words.py [--check] [--yasc] PATH...

A .c/.h file is rewritten when it is yasc (`#pragma yasc`, or under a
directory named yasc, or --yasc: every .c/.h given); its `#pragma tcc c_begin` .. `c_end` sections are left
alone.  Mixed text (.py, .md, .js, .ts, .json and .txt files):
the text may hold regular C too, so a level made only of C qualifiers is
merely glued when it has one word (`* const` -> `*const`, the same C) and
left as it is when it has more.  Block-comment continuation lines (` * ...`)
are not touched.
"""
import os
import re
import sys

YASC_WORD = r"(?:owned(?:\(\s*\w+\s*\))?|view|maybe|borrows\([^()]*\)|takes\(\s*\w+\s*\)|c_api\((?:[^()]|\([^()]*\))*\))"
C_WORD = r"(?:const|restrict|__restrict|__restrict__|volatile|_Atomic)"
WORD = r"(?:%s|%s)" % (YASC_WORD, C_WORD)
# a '*' (not a comment's) and its words
LEVEL = re.compile(r"(?<!/)(?<!/\*)\*(?!/)([ \t]*)(%s(?:[ \t]+%s)*)(?![\w(])" % (WORD, WORD))
WORD_RE = re.compile(WORD)
YASC_RE = re.compile(r"^%s$" % YASC_WORD)
FOLLOW = re.compile(r"[ \t]*")


def split_words(text):
    out, i = [], 0
    while i < len(text):
        if text[i] in " \t":
            i += 1
            continue
        m = WORD_RE.match(text, i)
        if not m:
            return None
        out.append(m.group(0))
        i = m.end()
    return out


def level_text(words, mixed):
    """The glued spelling of one '*''s words, or None to leave it."""
    maybe = "maybe" in words
    rest = [w for w in words if w != "maybe"]
    has_yasc = maybe or any(YASC_RE.match(w) for w in rest)
    if mixed and not has_yasc and len(rest) > 1:
        return None
    star = "*?" if maybe else "*"
    if not rest:
        return star
    if len(rest) == 1:
        return star + rest[0]
    return star + "@(" + ", ".join(rest) + ")"


def rewrite_line(line, mixed):
    if re.match(r"^\s*\*(\s|$)", line):
        return line  # a block comment's continuation
    def level(m):
        words = split_words(m.group(2))
        if words is None:
            return m.group(0)
        new = level_text(words, mixed)
        return m.group(0) if new is None else new

    out = LEVEL.sub(level, line)
    # a bare `*` / `*?` against the name it qualifies: `char *?p`
    out = re.sub(r"\*\?[ \t]+(?=[A-Za-z_\[])(?!(?:%s)\b)" % WORD, "*?", out)
    # brackets: `[maybe 4]`, `[view maybe]`
    def bracket(m):
        inner = m.group(1)
        if not re.search(r"\bmaybe\b", inner):
            return m.group(0)
        rest = re.sub(r"\s*\bmaybe\b\s*", " ", inner).strip()
        return "[?" + rest + "]"

    out = re.sub(r"\[([A-Za-z_0-9 \t:]*\bmaybe\b[A-Za-z_0-9 \t:]*)\]", bracket, out)
    # int handles: `owned(close) maybe`, `view maybe`, `maybe owned`
    out = re.sub(r"\b(owned(?:\(\s*\w+\s*\))?|view)[ \t]+maybe\b", r"?\1", out)
    out = re.sub(r"\bmaybe[ \t]+(owned|view)\b", r"?\1", out)
    return out


def is_yasc_c(path, text):
    return "#pragma yasc" in text or "/yasc/" in path.replace(os.sep, "/")


FORCE_YASC = False


def rewrite(path, text):
    ext = os.path.splitext(path)[1]
    mixed = ext not in (".c", ".h")
    if not mixed and not FORCE_YASC and not is_yasc_c(path, text):
        return text
    lines = text.split("\n")
    in_c = False
    for i, line in enumerate(lines):
        if not mixed:
            if re.match(r"\s*#\s*pragma\s+tcc\s+c_begin\b", line):
                in_c = True
            elif re.match(r"\s*#\s*pragma\s+tcc\s+c_end\b", line):
                in_c = False
            if in_c:
                continue
        lines[i] = rewrite_line(line, mixed)
    return "\n".join(lines)


EXTS = {".c", ".h", ".py", ".md", ".js", ".ts", ".json", ".txt", ".yc"}


def walk(paths):
    for p in paths:
        if os.path.isdir(p):
            for root, dirs, files in os.walk(p):
                dirs[:] = [d for d in dirs if d not in (".git", "node_modules", "__pycache__")]
                for f in files:
                    if os.path.splitext(f)[1] in EXTS:
                        yield os.path.join(root, f)
        else:
            yield p


def main(argv):
    global FORCE_YASC
    check = "--check" in argv
    FORCE_YASC = "--yasc" in argv
    paths = [a for a in argv if not a.startswith("--")]
    changed = 0
    for path in walk(paths):
        try:
            with open(path, encoding="utf-8") as f:
                text = f.read()
        except (UnicodeDecodeError, OSError):
            continue
        new = rewrite(path, text)
        if new != text:
            changed += 1
            print(path)
            if not check:
                with open(path, "w", encoding="utf-8") as f:
                    f.write(new)
    return 1 if check and changed else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
