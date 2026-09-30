#!/usr/bin/env python3
"""Generate the unity-build groups in source/unity/.

A unity group is one TU that #includes several library sources, so the headers
every source shares (tcc.h alone is ~560 KB of preprocessing) are read once per
group instead of once per file.  See the "Unity build" block in the Makefile.

Putting two .c files in one TU is only safe if neither can see the other's
file-local names.  This script guarantees that by construction, from each
source's own preprocessed text (gcc -E -dD, which says which file every #define
came from):

  * Macros a source defines are #undef'd right after it.  A source that defines
    or #undefs a macro one of its own headers also defines is kept out of every
    group: the #undef would take the header's definition away from the members
    after it, and the header, include-guarded, would not bring it back.  Any
    other source only shares a group whose members' headers do not define its
    macros.
  * Sources only share a group when their file-scope statics, tags, typedefs
    and enumerators are disjoint, and none of them is a name that the headers
    of another member declare (a local `static T name[]` next to some header's
    `extern U name[]`).  Two same-named functions would only fail to compile,
    but two `static int x;` are both tentative definitions of ONE object in a
    single TU, which would silently share state.
  * tcc.h picks TCC_STATE_VAR / TCC_SET_STATE / _tcc_error by whether the
    includer defined USING_GLOBALS first; a later member cannot change that, so
    sources only share a group when they agree on it.
  * Groups stay within one library and under --max-lines source lines, and a
    source bigger than that stands alone.

Usage (in a configured tree):
    scripts/gen_unity.py            # rewrite source/unity/
    scripts/gen_unity.py --check    # exit 1 if source/unity/ is stale
"""

import argparse
import collections
import concurrent.futures
import difflib
import os
import re
import shlex
import tempfile
import subprocess
import sys

TOP = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
UNITY_DIR = os.path.join('source', 'unity')

# Macros whose effect the grouping key already captures (see mode_key).  tcc.h
# consumes USING_GLOBALS and #undefs it, so it is local to the includer.
MODE_MACROS = {'USING_GLOBALS'}
MODE_KEYS = ('TCC_STATE_VAR', 'TCC_SET_STATE', '_tcc_error')

ARCH_DIRS = ('source/backend/arch/arm', 'source/backend/arch/arm/thumb')

# The same sources also build the on-device compiler (build_rootfs.sh), whose
# #ifs can declare other names; analyse both and keep the union.
NATIVE_DEFINES = ['-DTCC_IS_NATIVE', '-DTARGETOS_YasOS=1', '-DTCC_DEBUG=0',
                  '-DCONFIG_TCC_DEBUG_ENV=0']

LINEMARK = re.compile(r'^#\s+\d+\s+"([^"]*)"((?:\s+\d)*)')
DEFINE = re.compile(r'^#define\s+(\w+)(.*)$')
UNDEF = re.compile(r'^#undef\s+(\w+)')
SRC_DEFINE = re.compile(r'^[ \t]*#[ \t]*(?:define|undef)[ \t]+(\w+)', re.M)


def preprocessor_flags(args):
    """Only what decides the preprocessed text.  -O, -g, -f and -W flags differ
    between configurations (CI builds with ASan) and must not change the
    groups, or --check would disagree from one tree to the next."""
    return [a for a in args if a[:2] in ('-D', '-U', '-I') or a.startswith('-std=')]


def make_info(target):
    out = subprocess.run(['make', '--no-print-directory', 'unity-info',
                          'CROSS_TARGET=' + target, 'UNITY=no'],
                         cwd=TOP, capture_output=True, text=True, check=True).stdout
    flags, libs = None, collections.OrderedDict()
    for line in out.splitlines():
        if line.startswith('CC '):
            flags = preprocessor_flags(shlex.split(line[3:])[1:])
        elif line.startswith('LIB '):
            f = line.split()
            libs[f[1]] = f[2:]
    if flags is None or not libs:
        sys.exit('gen_unity: `make unity-info` gave nothing -- is the tree configured?')
    lib_flags = {lib: flags for lib in libs}
    # The arch sub-libraries are separate makes (source/backend/arch/unity.mk);
    # they compile with the top-level flags plus these two -I's.
    for d in ARCH_DIRS:
        out = subprocess.run(['make', '--no-print-directory', '-C', d, 'unity-info',
                              'TOP=' + TOP, 'UNITY=no'],
                             cwd=TOP, capture_output=True, text=True, check=True).stdout
        for line in out.splitlines():
            if line.startswith('LIB '):
                f = line.split()
                libs[f[1]] = f[2:]
                lib_flags[f[1]] = flags + ['-I.', '-Isource/ir']
    return lib_flags, libs


def is_header(path):
    # <built-in> and <command-line> are shared by every source, like a header
    return path.endswith('.h') or path.startswith('<')


def strip_literals(text):
    """Blank string and char literals so their braces and words do not count."""
    return re.sub(r'"(?:\\.|[^"\\\n])*"|\'(?:\\.|[^\'\\\n])*\'', '""', text)


def file_scope_names(text, every=False):
    """File-scope names a stretch of preprocessed C declares with internal
    linkage or as types: statics, struct/union/enum tags, typedefs, enumerators;
    with every=True, all declared names, externs and prototypes included.
    Coarse, but it only ever errs towards reporting too much, which just keeps
    two sources apart."""
    names = set()
    text = strip_literals(text)
    depth = 0
    stmt = []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if c == '{':
            if depth == 0:
                head = ''.join(stmt)
                tag = re.search(r'\b(struct|union|enum)\s+(\w+)\s*$', head)
                if tag:
                    names.add(tag.group(1) + ' ' + tag.group(2))
                if re.search(r'\benum\b[^;]*$', head):
                    j = text.find('}', i)
                    for e in text[i + 1:j].split(','):
                        m = re.match(r'\s*([A-Za-z_]\w*)', e)
                        if m:
                            names.add(m.group(1))
                if '=' in head or re.search(r'\b(struct|union|enum)\b[^()]*$', head):
                    # an initializer, or an aggregate body inside a declaration:
                    # skip it, keep the declaration going so its declarators
                    # are seen at the ';'
                    d = 0
                    while i < n:
                        if text[i] == '{':
                            d += 1
                        elif text[i] == '}':
                            d -= 1
                            if d == 0:
                                break
                        i += 1
                    stmt.append(' {} ')
                    i += 1
                    continue
                # a function body: the declaration ends here
                declare(head, names, True, every)
                stmt = []
            depth += 1
        elif c == '}':
            depth -= 1
        elif depth == 0:
            if c == ';':
                declare(''.join(stmt), names, False, every)
                stmt = []
            else:
                stmt.append(c)
        i += 1
    return names


ATTRIBUTE = re.compile(r'\b(?:__attribute__|__attribute|__asm__|__asm|asm)\s*\(')


def strip_attributes(decl):
    """Drop __attribute__((...)) and asm("...") labels: their parentheses would
    otherwise pass for a declarator's."""
    while True:
        m = ATTRIBUTE.search(decl)
        if not m:
            return decl.replace('__extension__', ' ')
        depth, j = 0, m.end() - 1
        while j < len(decl):
            depth += {'(': 1, ')': -1}.get(decl[j], 0)
            j += 1
            if depth == 0:
                break
        decl = decl[:m.start()] + ' ' + decl[j:]


def declare(decl, names, is_function, every):
    decl = ' '.join(strip_attributes(decl).split())
    if not decl:
        return
    words = re.findall(r'[A-Za-z_]\w*', decl)
    if every or 'typedef' in words or 'static' in words:
        if is_function:
            m = re.match(r'([^(]*)\(', decl)
            ids = re.findall(r'[A-Za-z_]\w*', m.group(1)) if m else []
            if ids:
                names.add(ids[-1])
            return
        # split the declarators at top-level commas
        depth, cur, parts = 0, '', []
        for ch in decl:
            if ch in '([':
                depth += 1
            elif ch in ')]':
                depth -= 1
            if ch == ',' and depth == 0:
                parts.append(cur)
                cur = ''
            else:
                cur += ch
        parts.append(cur)
        for k, p in enumerate(parts):
            p = p.split('=')[0]
            # `(*name)(...)` declares name; otherwise the last word before '[' or '('
            m = re.search(r'\(\s*\*+\s*([A-Za-z_]\w*)\s*\)', p)
            if m:
                names.add(m.group(1))
                continue
            p = re.split(r'[\[(]', p)[0]
            ids = re.findall(r'[A-Za-z_]\w*', p)
            if ids:
                names.add(ids[-1])


def analyse(src, flags):
    pp = subprocess.run(['gcc', '-E', '-dD'] + flags + [src], cwd=TOP,
                        capture_output=True, text=True)
    if pp.returncode != 0:
        sys.exit('gen_unity: gcc -E failed on %s:\n%s' % (src, pp.stderr))
    # A region is the source's own when the source itself, or a non-header it
    # includes, produced it; stab.def included from stab.h is a header's.
    stack = [(src, True)]
    local = True
    hdr_macros = set()
    local_defs, local_undefs = set(), set()
    mode = {}
    local_text, hdr_text = [], []
    code = []
    for line in pp.stdout.splitlines():
        m = LINEMARK.match(line)
        if m:
            path, fl = m.group(1), m.group(2).split()
            if '1' in fl:
                stack.append((path, stack[-1][1] and not is_header(path)))
            elif '2' in fl:
                while len(stack) > 1 and stack[-1][0] != path:
                    stack.pop()
            elif len(stack) == 1:
                stack[0] = (path, not is_header(path))
            local = stack[-1][1]
            continue
        m = DEFINE.match(line)
        if m:
            name = m.group(1)
            if name in MODE_KEYS:
                mode[name] = m.group(2).strip()
            (local_defs if local else hdr_macros).add(name)
            continue
        m = UNDEF.match(line)
        if m:
            name = m.group(1)
            if name in MODE_KEYS:
                mode.pop(name, None)
            (local_undefs if local else hdr_macros).add(name)
            continue
        if line.startswith('#'):
            continue
        (local_text if local else hdr_text).append(line)
        if local:
            code.extend(line.split())
    # Also every #define in the file itself, including ones this configuration's
    # #ifs skip: the #undef list has to hold for every configuration.
    with open(os.path.join(TOP, src), errors='replace') as f:
        raw = f.read()
    local_defs |= set(SRC_DEFINE.findall(raw)) - local_undefs
    return dict(
        src=src,
        lines=raw.count('\n'),
        mode=tuple(mode.get(k) for k in MODE_KEYS),
        hdr_macros=hdr_macros,
        local_defs=local_defs,
        local_undefs=local_undefs,
        code=code,
        names=file_scope_names('\n'.join(local_text)),
        hdr_names=file_scope_names('\n'.join(hdr_text), every=True),
    )


def plan(libs, info, max_lines, ungroupable=()):
    kept_apart = {}
    groups = collections.OrderedDict()
    for lib, srcs in libs.items():
        chunks = collections.defaultdict(list)      # mode -> [chunk]
        for src in srcs:
            i = info[src]
            # A file that redefines a macro one of its own headers defines stays
            # alone; otherwise it only has to avoid the headers of its group.
            local = (i['local_defs'] | i['local_undefs']) - MODE_MACROS
            clash = local & i['hdr_macros']
            if clash:
                kept_apart[src] = 'redefines header macros ' + ' '.join(sorted(clash))
                continue
            if src in ungroupable:
                kept_apart[src] = 'expands differently inside a group'
                continue
            if i['lines'] > max_lines:
                kept_apart[src] = '%d lines' % i['lines']
                continue
            # A file-local name clashes with another member's file-local names,
            # and with anything the headers of another member declare -- unless
            # this file's own headers declare it too, which it already agrees with.
            own, hdr = i['names'], i['hdr_names']
            loose = own - hdr
            for ch in chunks[i['mode']]:
                if (ch['lines'] + i['lines'] <= max_lines and not (own & ch['names'])
                        and not (loose & ch['hdr']) and not (ch['loose'] & hdr)
                        and not (local & ch['hdr_macros']) and not (ch['local'] & i['hdr_macros'])):
                    break
            else:
                ch = dict(lines=0, names=set(), hdr=set(), loose=set(), members=[],
                          local=set(), hdr_macros=set())
                chunks[i['mode']].append(ch)
            ch['local'] |= local
            ch['hdr_macros'] |= i['hdr_macros']
            ch['lines'] += i['lines']
            ch['names'] |= own
            ch['hdr'] |= hdr
            ch['loose'] |= loose
            ch['members'].append(src)
        # number the groups in source order, so the output is stable
        found = [ch for cs in chunks.values() for ch in cs if len(ch['members']) > 1]
        found.sort(key=lambda ch: srcs.index(ch['members'][0]))
        for k, ch in enumerate(found):
            groups['%s_%02d' % (lib, k + 1)] = (lib, ch['members'])
    return groups, kept_apart


HEADER = '/* Generated by scripts/gen_unity.py -- do not edit; rerun it instead. */\n'


def unity_text(members, info):
    body = [HEADER]
    for src in members:
        body.append('#include "../%s"\n' % os.path.relpath(src, 'source'))
        for name in sorted(info[src]['local_defs']):
            body.append('#undef %s\n' % name)
    return ''.join(body)


def member_code(text, flags, members):
    """Preprocess a unity TU and return each member's own tokens, the way
    analyse() collects them from the member alone."""
    fd, tmp = tempfile.mkstemp(prefix='.gen_unity_', suffix='.c', dir=os.path.join(TOP, UNITY_DIR))
    try:
        with os.fdopen(fd, 'w') as f:
            f.write(text)
        rel = os.path.relpath(tmp, TOP)
        pp = subprocess.run(['gcc', '-E', '-dD'] + flags + [rel], cwd=TOP,
                            capture_output=True, text=True)
    finally:
        os.remove(tmp)
    if pp.returncode != 0:
        return None
    out = pp.stdout
    # __FILE__ says source/unity/../x.c inside a group; compare as source/x.c
    for src in members:
        out = out.replace('"' + os.path.join(UNITY_DIR, '..', os.path.relpath(src, 'source')) + '"',
                          '"' + src + '"')
    codes = {src: [] for src in members}
    stack = []
    for line in out.splitlines():
        m = LINEMARK.match(line)
        if m:
            path, fl = os.path.normpath(m.group(1)), m.group(2).split()
            if '1' in fl:
                local = (len(stack) == 1 and not is_header(path)) or \
                        (len(stack) > 1 and stack[-1][1] and not is_header(path))
                stack.append((path, local))
            elif '2' in fl:
                while len(stack) > 1 and stack[-1][0] != path:
                    stack.pop()
            elif not stack:
                stack.append((path, False))
            continue
        if line.startswith('#') or len(stack) < 2 or not stack[-1][1]:
            continue
        if stack[1][0] in codes:
            codes[stack[1][0]].extend(line.split())
    return codes


def verify(groups, info, lib_flags):
    """Members whose own code does not preprocess to the same tokens inside
    their group as alone (in either configuration)."""
    jobs = [(members, lib_flags[lib] + extra, cfg)
            for lib, members in groups.values()
            for cfg, extra in (('cross', []), ('native', NATIVE_DEFINES))]
    def run(job):
        members, flags, cfg = job
        codes = member_code(unity_text(members, info), flags, members)
        if codes is None:
            return set(members[1:])      # does not even preprocess: split it up
        return {src for src in members if codes[src] != info[src]['code_' + cfg]}
    with concurrent.futures.ThreadPoolExecutor(os.cpu_count()) as ex:
        return set().union(*ex.map(run, jobs)) if jobs else set()


def render(groups, kept_apart, info, max_lines):
    files = {}
    mk = [HEADER.replace('/*', '#').replace(' */', ''),
          '# Unity-build groups: see the "Unity build" block in the Makefile.\n',
          '# Budget %d source lines per group.\n' % max_lines]
    for src in sorted(kept_apart):
        mk.append('# own TU: %s (%s)\n' % (src, kept_apart[src]))
    by_lib = collections.OrderedDict()
    for g, (lib, members) in groups.items():
        by_lib.setdefault(lib, []).append(g)
    for lib, gs in by_lib.items():
        mk.append('\nUNITY_GROUPS_%s = %s\n' % (lib, ' '.join(gs)))
        for g in gs:
            members = groups[g][1]
            mk.append('UNITY_MEMBERS_%s = \\\n\t%s\n' % (g, ' \\\n\t'.join(members)))
            files[os.path.join(UNITY_DIR, g + '.c')] = unity_text(members, info)
    files[os.path.join(UNITY_DIR, 'unity.mk')] = ''.join(mk)
    return files


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--check', action='store_true',
                    help='report whether source/unity/ is up to date; write nothing')
    ap.add_argument('--max-lines', type=int, default=6000,
                    help='source-line budget per group (default %(default)s)')
    ap.add_argument('--target', default='armv8m', help='CROSS_TARGET to ask make about')
    ap.add_argument('-v', '--verbose', action='store_true')
    args = ap.parse_args()

    lib_flags, libs = make_info(args.target)
    jobs = [(s, lib_flags[lib]) for lib, ss in libs.items() for s in ss]
    srcs = [s for s, _ in jobs]
    with concurrent.futures.ThreadPoolExecutor(os.cpu_count()) as ex:
        cross = list(ex.map(lambda j: analyse(*j), jobs))
        native = list(ex.map(lambda j: analyse(j[0], j[1] + NATIVE_DEFINES), jobs))
    info = {}
    for a, b in zip(cross, native):
        if a['mode'] != b['mode']:
            sys.exit('gen_unity: %s picks USING_GLOBALS differently for the native build'
                     % a['src'])
        info[a['src']] = dict(a, code_cross=a['code'], code_native=b['code'],
                              **{k: a[k] | b[k] for k in
                                 ('hdr_macros', 'local_defs', 'local_undefs',
                                  'names', 'hdr_names')})
    # The rules above are the plan; this is the proof.  Preprocess every group
    # and keep out any member whose own code expands differently than alone.
    ungroupable = set()
    while True:
        groups, kept_apart = plan(libs, info, args.max_lines, ungroupable)
        bad = verify(groups, info, lib_flags)
        if not bad:
            break
        ungroupable |= bad
    files = render(groups, kept_apart, info, args.max_lines)

    old = {}
    udir = os.path.join(TOP, UNITY_DIR)
    if os.path.isdir(udir):
        for f in os.listdir(udir):
            p = os.path.join(UNITY_DIR, f)
            with open(os.path.join(TOP, p)) as fh:
                old[p] = fh.read()
    stale = sorted(p for p in set(old) | set(files) if old.get(p) != files.get(p))

    grouped = sum(len(m) for _, m in groups.values())
    summary = '%d sources -> %d groups holding %d, %d on their own' % (
        len(srcs), len(groups), grouped, len(srcs) - grouped)
    if args.check:
        if stale:
            for p in stale:
                sys.stdout.writelines(difflib.unified_diff(
                    old.get(p, '').splitlines(True), files.get(p, '').splitlines(True),
                    'a/' + p, 'b/' + p))
            print('gen_unity: source/unity/ is stale (%d files); run scripts/gen_unity.py' % len(stale))
            return 1
        print('gen_unity: up to date: ' + summary)
        return 0
    os.makedirs(udir, exist_ok=True)
    for p in stale:
        full = os.path.join(TOP, p)
        if p in files:
            with open(full, 'w') as fh:
                fh.write(files[p])
        else:
            os.remove(full)
    print('gen_unity: ' + summary + ('; %d files changed' % len(stale)))
    if args.verbose:
        for src, why in sorted(kept_apart.items()):
            print('  own TU: %s (%s)' % (src, why))
    return 0


if __name__ == '__main__':
    sys.exit(main())
