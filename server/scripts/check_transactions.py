"""Every transaction must open through gme::newTransaction; this is not a C++ compiler.

python scripts/check_transactions.py

Fails when server code opens a transaction with drogon's own newTransactionCoro(),
newTransaction() or newTransactionAsync() anywhere but gme/common/Transactions.hpp.
Drogon's "begin" is deferred: such a transaction reads first and only takes SQLite's
write lock at its first write, which fails at once with "database is locked" whenever
another process writing the save has written meanwhile (ticket #72).
gme::newTransaction takes the write lock with the first statement, where SQLite waits.
"""
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
ALLOWED = {'gimuserver/gme/common/Transactions.hpp'}
SOURCES = ('gimuserver', 'standalone_frontend', 'game_frontend')
TOKENS = re.compile(r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\\r\n])\'|//[^\n]*|/\*.*?\*/', re.S)
OPENS = re.compile(r'(?:->|\.)\s*newTransaction(?:Coro|Async)?\s*\(')


def scrub(source):
    """Comments and string literals blanked, newlines kept (line numbers stay right)."""
    return TOKENS.sub(lambda m: ''.join('\n' if c == '\n' else ' ' for c in m.group()), source)


def offenders():
    found = []
    for top in SOURCES:
        for path in sorted((ROOT / top).rglob('*')):
            if path.suffix not in ('.cpp', '.hpp', '.h', '.cc') or not path.is_file():
                continue
            rel = path.relative_to(ROOT).as_posix()
            if rel in ALLOWED:
                continue
            code = scrub(path.read_text(encoding='utf-8', errors='replace'))
            for match in OPENS.finditer(code):
                line = code.count('\n', 0, match.start()) + 1
                found.append(f'{rel}:{line}: opens a drogon transaction directly; '
                             'use gme::newTransaction / gme::newTransactionSync (Transactions.hpp)')
    return found


def main():
    found = offenders()
    for line in found:
        print(line)
    print(f'{len(found)} transaction(s) opened outside gme::newTransaction')
    return 1 if found else 0


if __name__ == '__main__':
    sys.exit(main())
