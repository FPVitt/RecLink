#!/usr/bin/env python3
"""Pack one portal asset for embedding: drop comments, then gzip (spec 10.3).

    www_pack.py <source> <out.gz>

Only comments and indentation are removed -- the sources keep them, the board does not need
them, and they cost ~3 KB of the gzip budget. JavaScript: whole lines whose first non-blank
characters are "//", and leading blanks (never inside a template literal; trailing comments
are left alone). CSS: /* ... */ blocks. HTML is only compressed. The gzip header carries no name or time (like gzip -n), so
the output is reproducible.
"""
import gzip
import re
import sys


def strip_js(src: str) -> str:
    out, in_tpl = [], False
    for line in src.split('\n'):
        if not in_tpl and line.lstrip().startswith('//'):
            continue
        out.append(line if in_tpl else line.lstrip())
        # Track template literals that span lines: an odd number of unescaped backticks flips it.
        if (len(re.findall(r'(?<!\\)`', line)) % 2) == 1:
            in_tpl = not in_tpl
    return '\n'.join(out)


def strip_css(src: str) -> str:
    return re.sub(r'/\*.*?\*/', '', src, flags=re.S)


def main() -> None:
    src_path, out_path = sys.argv[1], sys.argv[2]
    with open(src_path, encoding='utf-8') as f:
        src = f.read()
    if src_path.endswith('.js'):
        src = strip_js(src)
    elif src_path.endswith('.css'):
        src = strip_css(src)
    with open(out_path, 'wb') as f:
        f.write(gzip.compress(src.encode('utf-8'), compresslevel=9, mtime=0))


if __name__ == '__main__':
    main()
