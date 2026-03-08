#!/usr/bin/env python3
"""
Simple K&R -> ANSI function signature converter.
- Scans C files under a directory (default osfmk/src/mach_services)
- Converts K&R style function definitions like:

func(a, b)
    int a;
    char *b;
{
    ...
}

into:

func(int a, char *b)
{
    ...
}

- Makes a .orig backup of each file it modifies.
- Conservative: only converts when number of params in header matches following declarations,
  and all declarations are simple (no comma-declarations per line).

Use with caution; review diffs after running.
"""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1] / 'osfmk' / 'src' / 'mach_services'

FUNC_HDR_RE = re.compile(r'^([A-Za-z_][A-Za-z0-9_\t \*]*?)\(([A-Za-z0-9_, \t]*)\)\s*$')
DECL_RE = re.compile(r'^\s*([A-Za-z_][A-Za-z0-9_\s\*]+)\s+([A-Za-z_][A-Za-z0-9_]*)\s*;\s*$')

def process_file(p: Path):
    s = p.read_text()
    lines = s.splitlines()
    out = []
    i = 0
    changed = False
    while i < len(lines):
        m = FUNC_HDR_RE.match(lines[i])
        if m and i+1 < len(lines):
            func_name = m.group(1).strip()
            args = [a.strip() for a in m.group(2).split(',') if a.strip()!='']
            # Collect following decls
            decls = []
            j = i+1
            while j < len(lines):
                dm = DECL_RE.match(lines[j])
                if dm:
                    decls.append((dm.group(1).strip(), dm.group(2).strip()))
                    j += 1
                    continue
                break
            # If there are declarations and counts match args, and next non-empty is '{' or starts with '{'
            if decls and len(decls) == len(args):
                # Ensure the next line after decls is '{' or starts the function body
                if j < len(lines) and re.match(r'^\s*\{', lines[j]):
                    # Build ANSI signature
                    params = []
                    for (typ, name) in decls:
                        params.append(typ + ' ' + name)
                    new_hdr = func_name + '(' + ', '.join(params) + ')'
                    out.append(new_hdr)
                    changed = True
                    i = j  # skip decls
                    continue
        out.append(lines[i])
        i += 1
    if changed:
        bak = p.with_suffix(p.suffix + '.orig')
        bak.write_text(s)
        p.write_text('\n'.join(out) + '\n')
    return changed


def main():
    files = list(ROOT.rglob('*.c'))
    changed_files = []
    for f in files:
        try:
            if process_file(f):
                changed_files.append(str(f))
        except Exception as e:
            print(f'Error processing {f}: {e}', file=sys.stderr)
    print(f'Processed {len(files)} files. Modified: {len(changed_files)}')
    for cf in changed_files:
        print(cf)

if __name__ == '__main__':
    main()
