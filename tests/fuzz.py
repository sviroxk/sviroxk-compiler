#!/usr/bin/env python3
"""Ограниченный фаззер компилятора SC (из аудита 2026-11-12, sc-audit-crashes/tools/fuzz.py).

Usage: tests/fuzz.py SC_BIN N SEED OUTDIR

Строит N входов детерминированно из SEED (мутации примеров репозитория, случайные
байты, глубокие/длинные конструкции, случайные программы по грамматике) и
компилирует каждый. Чистый исход — код 0, или 1/2 с текстом ошибки в stderr.
Всё остальное (код 3, сигнал, зависание > 60 с, ошибка без текста, отчёт
ASan/UBSan, необработанное исключение) — провал: вход сохраняется в OUTDIR, код выхода 1.
"""
import glob
import os
import random
import re
import subprocess
import sys
import tempfile
from multiprocessing import Pool

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SC, N, SEED, OUT = os.path.abspath(sys.argv[1]), int(sys.argv[2]), int(sys.argv[3]), sys.argv[4]
# Зависание — это бесконечная работа, а не «медленно». Самый тяжёлый вход (литерал
# массива из 100000 элементов) локально идёт 1,5–1,8 с, но на раннере CI под ASan
# с 4 процессами на 2 ядрах однажды превысил 10 с (PR #64). 60 с отделяет зависание
# от медленной машины; процессов — по числу ядер.
TIMEOUT_SECONDS = 60
WORKERS = max(1, min(4, os.cpu_count() or 1))


def load_corpus():
    seeds = []
    patterns = ['examples/*.svx', 'tests/fixtures/*.svx', 'docs/conformance/*.svx', 'stdlib/**/*.svx']
    for pattern in patterns:
        for path in sorted(glob.glob(os.path.join(ROOT, pattern), recursive=True)):
            with open(path, encoding='utf-8', errors='replace') as source:
                seeds.append(source.read())
    with open(os.path.join(ROOT, 'docs/spec.md'), encoding='utf-8') as spec:
        seeds += [m.group(2) for m in re.finditer(r'```(sc|sc-error)\n(.*?)```', spec.read(), re.S)]
    return seeds


TOK = re.compile(r'\s+|//[^\n]*|"(?:[^"\\\n]|\\.)*"|\'(?:[^\'\\\n]|\\.)*\'|0[xXbB][0-9a-fA-F_]+|'
                 r'\d[\d_]*(?:\.\d+)?(?:[eE][+-]?\d+)?|[A-Za-z_][A-Za-z_0-9]*|\.\.=|\.\.|\*\*|//|<<=|>>=|'
                 r'[-+*/%&|^<>=!]=|&&|\|\||\+\+|--|<<|>>|->|.', re.S)
NUMBERS = ['0', '-1', '1', '2147483647', '2147483648', '-2147483648', '-2147483649', '4294967295',
           '9223372036854775807', '9223372036854775808', '-9223372036854775808', '18446744073709551615',
           '18446744073709551616', '1' + '0' * 400, '1e308', '1e309', '0x', '0b', '0xFFFFFFFFFFFFFFFFFFFF',
           '1_', '0.', '1e', '255', '256', '65536', '9' * 5000, '3.4028236e38', '-0', '08']
STRINGS = ['""', '"\\', '"abc', '"\\x"', '"%s%n%d"', '"' + 'A' * 4096 + '"', '"' + '\\n' * 2000 + '"']


def mutate_tokens(r, text, seeds, pool):
    t = TOK.findall(text) or ['']
    i, j = r.randrange(len(t)), r.randrange(len(t))
    op = r.randrange(8)
    if op == 0:
        del t[i:i + r.randint(1, 3)]
    elif op == 1:
        t[i], t[j] = t[j], t[i]
    elif op == 2:
        a, b = sorted((i, j))
        t[a:a] = t[a:min(b, a + 8) + 1]
    elif op == 3:
        t.insert(i, ' ' + r.choice(pool) + ' ')
    elif op == 4:
        t[i] = r.choice(pool)
    elif op == 5:
        other = TOK.findall(r.choice(seeds))
        if other:
            a = r.randrange(len(other))
            t[i:i] = other[a:a + r.randint(1, 30)]
    elif op == 6:
        numbers = [k for k, x in enumerate(t) if x[:1].isdigit()]
        t[r.choice(numbers) if numbers else i] = r.choice(NUMBERS)
    else:
        t.insert(i, r.choice(STRINGS))
    return ''.join(t)


def mutate_bytes(r, data):
    b = bytearray(data)
    for _ in range(r.randint(1, 8)):
        p = r.randrange(len(b) + 1)
        op = r.randrange(4)
        if op == 0 and b:
            b[min(p, len(b) - 1)] = r.randrange(256)
        elif op == 1:
            b[p:p] = bytes(r.randrange(256) for _ in range(r.randint(1, 4)))
        elif op == 2 and b:
            del b[p:p + r.randint(1, 4)]
        else:
            b[p:p] = r.choice([b'\x00', b'\xff\xfe', b'\xd0', b'\r', b'\\', b'"', b"'", b'/*', b'\xe2\x80\xae'])
    return bytes(b)


def deep(r):
    n = r.choice([50, 255, 256, 257, 1000, 10000, 10001, 100000])
    return r.choice([
        lambda: 'print(' + '(' * n + '1' + ')' * n + ');',
        lambda: 'int x = ' + '-' * n + '1;',
        lambda: 'int x = ' + '+'.join(['1'] * n) + ';',
        lambda: 'if (1) {' * n + 'print(1);' + '}' * n,
        lambda: 'fn int f(int a) { return a; }\nprint(' + 'f(' * n + '1' + ')' * n + ');',
        lambda: 'int x = ' + '!' * n + '1;',
        lambda: 'int[] a = [' + ','.join(['1'] * n) + ']; print(a[0]);',
        lambda: 'int x = 1 ' + '? 1 : 2 ' * n + ';',
    ])()


TYPES = ['int', 'i8', 'u8', 'i16', 'u32', 'i64', 'u64', 'char', 'f32', 'f64', 'bool', 'str', 'ptr']
LITERALS = {'f32': ['0.0', '1.5', '-2.25'], 'f64': ['1e10', '3.0', '0.1'], 'bool': ['true', 'false'],
            'char': ["'a'", "'0'"], 'str': ['"hi"', '""'], 'ptr': ['null']}


def program(r):
    names = []

    def expr(t, d=0):
        if d > 3 or r.random() < 0.3:
            same = [n for n, nt in names if nt == t]
            if same and r.random() < 0.6:
                return r.choice(same)
            return r.choice(LITERALS.get(t, ['0', '1', '7', '-1', '127', '255', '2147483647']))
        if t in ('str', 'ptr'):
            return expr(t, 4)
        ops = ['&&', '||'] if t == 'bool' else ['+', '-', '*', '/'] if t[0] == 'f' else \
            ['+', '-', '*', '/', '%', '**', '//', '&', '|', '^', '<<', '>>']
        k = r.randrange(4)
        if k == 0:
            return '(%s ? %s : %s)' % (expr('bool', d + 1), expr(t, d + 1), expr(r.choice(TYPES[:10]), d + 1))
        if k == 1:
            return '(%s) %s' % (t, expr(r.choice(TYPES[:10]), d + 1))
        return '(%s %s %s)' % (expr(t, d + 1), r.choice(ops), expr(t, d + 1))

    def stmt(d=0):
        k = r.randrange(7)
        t = r.choice(TYPES)
        if k < 3:
            name = 'v%d' % len(names)
            names.append((name, t))
            return '%s %s = %s;' % (t, name, expr(t))
        if k == 3 and d < 3:
            return 'if (%s) {\n%s\n} else {\n%s\n}' % (expr('bool'), stmt(d + 1), stmt(d + 1))
        if k == 4 and d < 3:
            return 'for i%d in %s..%s {\n%s\n}' % (d, expr('int', 3), expr('int', 3), stmt(d + 1))
        if k == 5:
            return 'ptr d%d = new %s[%s];\nd%d.push(%s);\nprint(d%d.len());' % (
                d, t, r.choice(['0', '2', '-1']), d, expr(t), d)
        return 'print(%s);' % expr(t)

    return '\n'.join(stmt() for _ in range(r.randint(1, 8))) + '\n'


def make_inputs():
    r = random.Random(SEED)
    seeds = load_corpus()
    pool = sorted(set(x for s in seeds for x in TOK.findall(s) if x.strip()))
    for index in range(N):
        k = r.random()
        if k < 0.35:
            text = r.choice(seeds)
            for _ in range(r.randint(1, 4)):
                text = mutate_tokens(r, text, seeds, pool)
            yield index, text.encode('utf-8', 'surrogatepass')
        elif k < 0.45:
            yield index, mutate_bytes(r, r.choice(seeds).encode())
        elif k < 0.52:
            yield index, bytes(r.randrange(256) for _ in range(r.randint(0, 400)))
        elif k < 0.62:
            yield index, deep(r).encode()
        else:
            yield index, program(r).encode()


def classify(status, err):
    if status is None:
        return 'зависание дольше %d с' % TIMEOUT_SECONDS
    if 'Sanitizer' in err or 'runtime error:' in err or 'terminate called' in err:
        return 'санитайзер/исключение: ' + err.strip().splitlines()[0][:120]
    if status == 3:
        return 'внутренняя ошибка (код 3): ' + (err.strip().splitlines() or [''])[0][:120]
    if status < 0 or status >= 128:
        return 'сигнал (код %d)' % status
    if status == 0 or (status in (1, 2) and err.strip()):
        return None
    return 'код %d без текста ошибки' % status


def run_one(job):
    index, data = job
    with tempfile.TemporaryDirectory(prefix='sc-fuzz-') as work:
        path = os.path.join(work, 'in.svx')
        with open(path, 'wb') as source:
            source.write(data)
        env = dict(os.environ, ASAN_OPTIONS='detect_leaks=0:allocator_may_return_null=1',
                   UBSAN_OPTIONS='halt_on_error=1')
        try:
            done = subprocess.run([SC, path], cwd=work, stdout=subprocess.DEVNULL,
                                  stderr=subprocess.PIPE, timeout=TIMEOUT_SECONDS, env=env)
            status, err = done.returncode, done.stderr.decode('utf-8', 'replace')
        except subprocess.TimeoutExpired:
            status, err = None, ''
    return index, data, classify(status, err)


def main():
    os.makedirs(OUT, exist_ok=True)
    failures = []
    with Pool(WORKERS) as pool:
        for index, data, problem in pool.imap_unordered(run_one, make_inputs(), chunksize=4):
            if problem:
                saved = os.path.join(OUT, '%05d.svx' % index)
                with open(saved, 'wb') as out:
                    out.write(data)
                failures.append((index, problem, saved))
    print('fuzz: %d входов (seed %d), чисто %d, проблем %d' % (N, SEED, N - len(failures), len(failures)))
    for index, problem, saved in sorted(failures)[:10]:
        print('  #%d: %s — %s' % (index, problem, saved))
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
