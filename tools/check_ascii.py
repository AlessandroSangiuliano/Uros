#!/usr/bin/env python3
import sys

def check_file(path):
    with open(path, 'rb') as f:
        data = f.read()
    for i, b in enumerate(data):
        if b < 32 and b not in (9,10,13):
            return (i, b)
        if b > 126:
            return (i, b)
    return None

if __name__ == '__main__':
    if len(sys.argv) < 2:
        print('Usage: check_ascii.py <file> [<file> ...]')
        sys.exit(2)
    failures = []
    for path in sys.argv[1:]:
        res = check_file(path)
        if res is not None:
            idx, b = res
            print(f"NON-ASCII in {path}: byte=0x{b:02x} at offset {idx}")
            failures.append((path, idx, b))
    if failures:
        sys.exit(1)
    print('OK: all files ASCII')
    sys.exit(0)
