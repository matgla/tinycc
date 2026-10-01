# Open bug reports

One file per known, unfixed tinycc bug: `docs/bugs/<short-slug>.md`. A bug
found but not fixed in the session that found it goes here; a session that
comes across a report here fixes it.

Format:

```
# <one-line statement of the defect>

**Status:** open · **Severity:** correctness (miscompile) | performance | ... · **Found:** YYYY-MM-DD (how)

## Summary
## Reproducer        (C source + -O level + the wrong output or code)
## Root cause        (file:function, what check is missing)
## Regression lock   (unit test pinning the current behaviour, if any)
## Likely fix
```

When fixed: flip any `_bug` unit-test lock the report names, add an IR test,
and delete the report in the fixing commit (git history keeps it).
