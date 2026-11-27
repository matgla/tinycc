# tcc -format: laying out C and yasc source

`tcc -format` lays out C and yasc source the way clang-format's LLVM style
does, for the words clang-format does not know: the yasc pointer words
(`* owned`, `* borrows(s) maybe`), slices (`[]const char`, `[:0]char`,
`s[0:n:0]`), `cast<int(T)>(x)`, `catch |e| { }`, error sets, `@min`, methods
and tagged unions. It is part of tcc, so it runs wherever tcc runs, the board
included.

```
tcc -format f.c                 # f.c laid out, on stdout
tcc -format -i a.c b.h          # rewrite the files (only those that change)
tcc -format -n *.c              # change nothing: name the files that would change, exit 1 if any
tcc -format -assume-filename=src/f.c < f.c   # stdin; the path finds its .tcc-format
```

With no file (or `-`) it reads stdin and writes stdout. When it cannot format
the input (an unterminated comment, a line splice outside a directive), it
writes the input unchanged to stdout, says why on stderr and exits 1, so an
editor that formats on save keeps the text.

## What it changes

- **Indentation**: `IndentWidth` per block (2), `case` labels and goto labels
  one level out (clang-format's `IndentCaseLabels: false`,
  `IndentGotoLabels`), a braceless `if`/`for`/`while`/`else`/`do` body one
  level in. Directives stay in column 0.
- **Spaces between tokens**: around binary operators, after commas and the
  `;` of a `for`, none inside parentheses or after a unary operator, `if (`
  but `f(`, `sizeof(`, `(int)x` (no space after a cast). A declaration's `*`
  goes with the name (`char *p`); a pointer word after it gets a space on both
  sides (`char * view p`, `char * borrows(s) maybe f(...)`), as in the yasc
  cheat sheet.
- **One statement per line**: `a = 1; b = 2;` and `if (x) return;` are split;
  a block's `{` ends its line and its `}` starts one. `case 1: x();` is split
  too, unless `AllowShortCaseLabelsOnASingleLine` is on.
- **Braces**: where a block's `{` goes is `BraceStyle`; `} else {`,
  `} while (x);` and `else if` join their lines.
- **Blank lines**: at most one in a row; none right after a block's `{` or
  right before its `}`.
- **Trailing comments** on neighbouring lines line up one column after the
  longest line's code; a `//` comment continuing one of them on the next line
  stays under it.
- Trailing blanks go; the file ends with one newline. CRLF files stay CRLF.

## What it keeps

- **Your line breaks inside a statement.** Nothing is re-wrapped to a column
  limit (zig fmt works the same way): a long line stays long, and a statement
  you broke stays broken. A continued line lines up after the open
  parenthesis, bracket or brace it is in (`f(a,` / `  b)`), or, after an
  opener that ends its line, is indented `ContinuationIndentWidth` (4) more
  than that line; otherwise it is indented 4 more than the statement. A
  continued `:` of a `?:` lines up under its `?`.
- **Initializers** (`= { ... }`) keep their lines; elements on lines of their
  own are indented 4.
- **Directives and macro bodies** stay as written (trailing blanks aside).
- **Comments** stay as written; a block comment's later lines move with its
  first.
- **`#if 0` bodies**, and anything between `// tcc-format off` and
  `// tcc-format on` (clang-format's `clang-format off`/`on` too), are copied
  as they are.

`#if`/`#else` branches each start from the state at `#if`, so a branch that
opens a brace its sibling also opens (`if (a) {` / `#else` / `if (b) {`)
indents like either branch alone; after `#endif` the first branch's state
goes on.

## Nothing but layout

What `tcc -format` writes is lexed again and compared with the input: the
same tokens in the same order (comments compared without their blanks,
directives without trailing ones). If they differ, the file is left as it was
and the error says so (that would be a bug in tcc -format). So formatting
cannot change what a file means.

Words like `out`, `in`, `view` and `maybe` are yasc keywords only where one
can stand (`out char *p`, `char * view p`); in regular C they stay names:
`*out++ = c` and `in = in * 2` keep their spacing.

## .tcc-format

The style comes from the first `.tcc-format` found in the file's directory or
above it; without one the defaults below apply. `Key: value` lines, `#`
comments (clang-format's `.clang-format` is not read: its options do not map).

| Key | Default | |
|---|---|---|
| `IndentWidth` | 2 | columns per block |
| `ContinuationIndentWidth` | 4 | a continued line, an initializer's elements |
| `BraceStyle` | `attach` | `attach`: every `{` ends a line (`) {`); `linux`: a function's `{` on a line of its own; `mozilla`: also struct/union/enum; `allman`: every block's, and `else` after `}` on its own line |
| `AllowShortCaseLabelsOnASingleLine` | false | `case 1: return x;` stays on one line when written on one |
| `DisableFormat` | false | files under it are left as they are |

## .tcc-format-ignore

Files to leave alone (vendored code, fixtures whose line numbers matter): a
`.tcc-format-ignore` in a directory above the file, one `fnmatch(3)` pattern
per line, `#` comments. A pattern with a `/` matches the path below that
directory (`vendor/*.c`, `tests/fixtures/*`); one without matches the file
name. An ignored file goes to stdout unchanged and `-i`/`-n` skip it.

## Format on save

VS Code: the yasc extension (`tools/vscode-yasc` in the YasOS tree) formats
`.c`/`.h` files with `tcc -format` (Format Document, and on save with
`"editor.formatOnSave": true`). It runs `yasc.tccPath -format
-assume-filename=<file>`, so the file's `.tcc-format` applies.

Any other editor: pipe the buffer through
`tcc -format -assume-filename=<path>`; the exit status is 0 when the output
is the formatted text, 1 when it is the input unchanged.
