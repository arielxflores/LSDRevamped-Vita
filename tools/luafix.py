#!/usr/bin/env python3
"""Convert LSD Revamped's MoonSharp Lua to standard Lua 5.2:
  || expr        ->  function() return expr end     (MoonSharp short lambda)
  |a, b| expr    ->  function(a, b) return expr end
  !=             ->  ~=
Strings, comments and long brackets are left alone. The lambda body runs to the
first ',' / ')' / ']' / '}' at its own nesting depth (or a line end at depth 0).
"""
import re
import sys

_LONG = re.compile(r'\[(=*)\[')


def _skip_string(s, i):
    q = s[i]
    i += 1
    while i < len(s):
        c = s[i]
        if c == '\\':
            i += 2
            continue
        if c == q:
            return i + 1
        if c == '\n':
            return i
        i += 1
    return i


def _skip_long(s, i):
    m = _LONG.match(s, i)
    if not m:
        return None
    close = ']' + m.group(1) + ']'
    j = s.find(close, m.end())
    return len(s) if j < 0 else j + len(close)


def _skip_comment(s, i):
    # at '--'
    j = _skip_long(s, i + 2)
    if j is not None:
        return j
    k = s.find('\n', i)
    return len(s) if k < 0 else k


def _lambda_end(s, i):
    """index where an expression starting at i ends"""
    depth = 0
    while i < len(s):
        c = s[i]
        if c in '"\'':
            i = _skip_string(s, i)
            continue
        if c == '[' and _LONG.match(s, i):
            i = _skip_long(s, i)
            continue
        if s.startswith('--', i):
            return i
        if c in '([{':
            depth += 1
        elif c in ')]}':
            if depth == 0:
                return i
            depth -= 1
        elif c == ',' and depth == 0:
            return i
        elif c == '\n' and depth == 0:
            return i
        elif c == ';' and depth == 0:
            return i
        i += 1
    return i


def convert(src):
    out = []
    i = 0
    n = len(src)
    while i < n:
        c = src[i]
        if c in '"\'':
            j = _skip_string(src, i)
            out.append(src[i:j])
            i = j
            continue
        if src.startswith('--', i):
            j = _skip_comment(src, i)
            out.append(src[i:j])
            i = j
            continue
        if c == '[':
            j = _skip_long(src, i)
            if j is not None:
                out.append(src[i:j])
                i = j
                continue
        if src.startswith('!=', i):
            out.append('~=')
            i += 2
            continue
        if c == '|':
            k = src.find('|', i + 1)
            params = src[i + 1:k] if k > 0 else None
            if params is not None and re.fullmatch(r'[\sA-Za-z0-9_,]*', params):
                start = k + 1
                end = _lambda_end(src, start)
                body = convert(src[start:end]).strip()
                out.append('function(%s) return %s end' % (params.strip(), body))
                i = end
                continue
        out.append(c)
        i += 1
    return ''.join(out)


if __name__ == '__main__':
    for p in sys.argv[1:]:
        sys.stdout.write(convert(open(p, encoding='utf-8').read()))
