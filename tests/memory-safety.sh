#!/usr/bin/env bash
# Called after test.sh verifies ASan availability; no new toolchain dependency.
set -Eeuo pipefail
SC_BIN=$1
ROOT=$2
mkdir -p "$ROOT"

# Each case runs in its own directory; fixtures cannot touch repository files.
# Modes: clean (exact stdout), runtime (SC diagnostic), detector (unsafe self-test).
check_case() {
    local name=$1 mode=$2 expected=$3 leaks=${4:-0}
    local work="$ROOT/$name" status=0
    mkdir -p "$work"
    cat > "$work/source.svx"
    if ! timeout --signal=TERM 10s "$SC_BIN" "$work/source.svx" >"$work/module.ll" 2>"$work/compile.err"; then
        echo "FAIL: memory safety '$name' did not compile" >&2
        cat "$work/compile.err" >&2
        exit 1
    fi
    sed -E 's/^(define [^{]*)\{$/\1 sanitize_address {/' "$work/module.ll" > "$work/asan.ll"
    if ! clang -no-pie -fsanitize=address -O0 "$work/asan.ll" -o "$work/program" -lm 2>"$work/build.err"; then
        echo "FAIL: memory safety '$name' ASan build failed" >&2
        cat "$work/build.err" >&2
        exit 1
    fi
    (cd "$work" && ASAN_OPTIONS="detect_leaks=$leaks:halt_on_error=1:abort_on_error=0:exitcode=86" \
        timeout --signal=TERM 10s ./program </dev/null >stdout.txt 2>stderr.txt) || status=$?
    case "$mode" in
        clean)
            printf '%s\n' "$expected" > "$work/expected.txt"
            if (( status != 0 )) || [[ -s "$work/stderr.txt" ]] ||
               ! cmp -s "$work/expected.txt" "$work/stdout.txt"; then
                echo "FAIL: memory safety '$name': expected exact output, no sanitizer report, status 0 (got $status)" >&2
                cat "$work/stdout.txt" "$work/stderr.txt" >&2
                exit 1
            fi ;;
        runtime)
            # SC diagnostic goes to stderr (D-007); any sanitizer report there is a failure.
            if (( status != 1 )) || grep -q "Sanitizer" "$work/stderr.txt" ||
               ! grep -Fq "$expected" "$work/stderr.txt" ||
               grep -Fq "SC runtime error" "$work/stdout.txt"; then
                echo "FAIL: memory safety '$name': expected SC runtime diagnostic, not an ASan fault (status $status)" >&2
                cat "$work/stdout.txt" "$work/stderr.txt" >&2
                exit 1
            fi ;;
        detector)
            # These programs are intentionally unsafe: test ASan, not a language guarantee.
            if (( status != 86 )) || ! grep -Fq "ERROR: AddressSanitizer: $expected" "$work/stderr.txt"; then
                echo "FAIL: ASan detector '$name' missed '$expected' (status $status)" >&2
                cat "$work/stderr.txt" >&2
                exit 1
            fi ;;
        *) echo "FAIL: unknown memory safety mode: $mode" >&2; exit 1 ;;
    esac
    echo "PASS: memory safety $mode '$name'"
}

# Strings live in functions: a leaked local is unreachable, so LeakSanitizer sees it.
check_case replace-string clean 'after' 1 <<'SC'
fn void run() { string text = replace("before", "before", "after"); print(text); }
run();
SC
# D-014: string variables own their heap copy and free it at block end; leak detection on.
check_case string-copy-assign-release clean $'hello\njello\nZello\nhello' 1 <<'SC'
fn void run() {
    string a = "hello";
    string b = a;
    b = replace(b, "h", "j");
    print(a); print(b);
    ptr chars = data(b);
    chars[0] = 'Z';
    print(b); print(a);
}
run();
SC
check_case string-function-release clean $'hi sam\nhi \nchanged\nkeep' 1 <<'SC'
fn string greet(string name) {
    string prefix = "hi ";
    if (len(name) == 0) { return prefix; }
    string result = replace("hi X", "X", name);
    return result;
}
fn void show(string s) { s = "changed"; print(s); }
string first = greet("sam");
string second = greet("");
print(first); print(second);
string kept = "keep";
show(kept);
print(kept);
SC
check_case string-loop-release clean $'3\n42\n2' 1 <<'SC'
fn int count(int n) {
    int total = 0;
    int i = 0;
    while (i < n) {
        string s = str(i);
        i++;
        if (i == 2) { continue; }
        if (i == 5) { break; }
        total += len(s);
    }
    return total;
}
print(count(10));
if (true) { string inner = str(42); print(inner); }
repeat (3) { string w = str(1); }
for j in 0..3 { string w2 = replace("aa", "a", "b"); if (j == 1) { break; } }
loop { string q = str(3); if (len(q) == 1) { break; } }
int seen = 0;
string[] names = ["a", "b"];
for nm in names { string copy = nm; seen += len(copy); }
print(seen);
SC
check_case string-input-release clean $'0\n0' 1 <<'SC'
fn void run() {
    string line = input_str("");
    print(len(line));
    file output = open("text.txt", "w");
    close(output);
    file input = open("text.txt", "r");
    string empty = read(input);
    close(input);
    print(len(empty));
}
run();
SC
check_case string-field-outlives-variable clean $'7\n7' 1 <<'SC'
struct Box { string text = "x"; }
Box box;
string[] items = ["a"];
if (true) { string local = str(7); box.text = local; items[0] = local; }
print(box.text); print(items[0]);
SC
# D-016: строки в полях структур освобождаются вместе со структурой на всех выходах из блока.
check_case struct-string-fields-release clean $'Ann\nBob\nt\n5\n37\nAnn\nBob\nt\nlocal\nlocal\n0\n0\n2\nglobal' 1 <<'SC'
struct Tag { string label = "t"; int n = 0; }
struct Person { string name; int age = 0; Tag tag; ptr extra = null; }
fn int total(Person p) { p.name = "changed"; p.tag.label = str(99); return len(p.name) + p.age; }
fn void run() {
    Person a = { name = "Ann"; age = 30; tag = { n = 0; }; };
    Person b = a;
    b.name = "Bob";
    b.tag.label = str(5);
    print(a.name);
    print(b.name);
    print(a.tag.label);
    print(b.tag.label);
    print(total(a));
    print(a.name);
    Person c;
    c = b;
    c = c;
    c.tag = a.tag;
    print(c.name);
    print(c.tag.label);
    string s = "local";
    Tag t = { label = s; n = 1; };
    Person d = { name = s; tag = t; };
    s = "other";
    print(d.name);
    print(d.tag.label);
    for i in 0..3 {
        Person item = { name = str(i); tag = { n = 1; }; };
        if (i == 1) { continue; }
        if (i == 2) { break; }
        print(item.name);
    }
    Person e;
    print(len(e.name));
}
fn int early(int k) { Person p = { name = str(k); tag = { n = 1; }; }; if (k > 1) { return len(p.name); } return 0; }
run();
print(early(10));
Person g = { name = "global"; tag = { n = 2; }; };
Person h = g;
print(h.name);
SC
check_case string-temporaries-release clean $'42\na+b\n5\n7\n3\n10\n1\n1\n99' 1 <<'SC'
fn int size(string s) { return len(s); }
fn string make(int n) { return str(n); }
fn int early(int n) { if (len(str(n)) > 1) { return len(replace(str(n), "1", "22")); } return 0; }
fn void run() {
    print(str(42));
    print(replace("a-b", "-", "+"));
    print(size(str(12345)));
    print(make(7));
    make(8);
    print(early(15));
    int i = 0;
    while (len(str(i)) < 2) { i++; if (len(str(i)) == 1) { continue; } }
    print(i);
    bool same = str(5) == "5" && replace("x", "x", "y") == "y";
    print(same);
    string t = true ? str(1) : str(2);
    print(t);
    input_str("");
    defer print(str(99));
    loop { if (len(str(3)) == 1) { break; } }
}
run();
SC
check_case string-move-release clean $'12345\nagain\n5\n12345\n12345\nx\ny' 1 <<'SC'
fn string pass(string s) { return move(s); }
fn int take(string s) { return len(s); }
fn void run() {
    string a = str(12345);
    string b = move(a);
    print(b);
    a = "again";
    print(a);
    print(take(move(a)));
    string c = pass(move(b));
    print(c);
    print(move(c));
    string d = "x";
    if (true) { string e = move(d); print(e); }
    d = "y"; d = move(d); print(d);
}
run();
SC
check_case struct-return-move-release clean $'12\n12\n7\n13\n12\n2\n5\nm\n99\nDan\n5\n0\n2\n0' 1 <<'SC'
struct Tag { string label = "t"; }
struct Person { string name; int age = 0; Tag tag; }
struct Pt { int x = 0; }
fn Person make(int n) { Person p = { name = str(n); age = n; tag = { label = "m"; }; }; return p; }
fn Person pass(Person p) { p.age += 1; return p; }
fn Person pick(int k) { Person a = make(1); Person b = make(2); if (k > 0) { return b; } return a; }
fn Tag tagOf(Person p) { return p.tag; }
fn int size(Person p) { return len(p.name); }
fn Person rename(Person p, string n) { p.name = n; return move(p); }
Person g = make(5);
fn Person global() { return g; }
fn void run() {
    Person a = make(12);
    print(a.name);
    Person b = move(a);
    print(b.name);
    a = make(7);
    print(a.name);
    Person c = pass(b);
    print(c.age);
    print(b.age);
    Person k = pick(1);
    print(k.age);
    print(size(make(12345)));
    make(3);
    Tag tg = tagOf(c);
    print(tg.label);
    c = make(99);
    c.tag = tagOf(a);
    print(c.name);
    Person d = rename(move(c), "Dan");
    print(d.name);
    Person e = global();
    e.name = "changed";
    print(g.name);
    for i in 0..3 { Person t = make(i); if (i == 1) { continue; } print(t.name); }
    Pt p;
    Pt q = move(p);
    print(q.x);
}
run();
SC
check_case struct-array-field-release clean $'13
7
3
109
3
3
30
30
0
9
4
42
2.500000
0
2
7
10
7
5
1' 1 <<'SC'
struct Stats { int[] scores; string name; }
struct Team { Stats s; f64[] codes; }
fn Stats make(int n) { Stats t = { name = "m"; }; for i in 0..n { t.scores.push(i * 10); } return t; }
fn int total(Stats t) { int sum = 0; for i in 0..t.scores.len() { sum += t.scores[i]; } t.scores.push(1); return sum; }
fn void run() {
    Stats a;
    a.scores.push(5);
    a.scores.push(7);
    a.scores[0] += 1;
    print(a.scores[0] + a.scores[1]);
    Stats b = a;
    b.scores[1] = 100;
    b.scores.push(3);
    print(a.scores[1]);
    print(b.scores.len());
    print(total(b));
    print(b.scores.len());
    Stats c = move(b);
    print(c.scores.pop());
    Stats d = make(4);
    print(d.scores[3]);
    print(total(make(3)));
    make(2);
    d.scores.clear();
    d.scores.clear();
    print(d.scores.len());
    d.scores.push(9);
    print(d.scores[0]);
    d.scores.resize(3);
    print(d.scores.cap());
    Team t;
    t.s.scores.push(42);
    t.codes.push(2.5);
    Team u = t;
    u.s.scores[0] = 1;
    print(t.s.scores[0]);
    print(u.codes[0]);
    for i in 0..3 { Stats e = make(i); if (i == 1) { continue; } print(e.scores.len()); }
    d = a;
    print(d.scores[1]);
    drop d.scores;
    d.scores = [7, 8];
    print(d.scores[1] + d.scores.len());
    Stats g = d;
    print(g.scores[0]);
    ptr v = new int[3];
    v.clear();
    v.push(4);
    print(v.len() + v[0]);
    drop v;
    drop t.s.scores;
    t.s.scores = [1];
    print(t.s.scores[0]);
}
run();
SC
check_case dynamic-grow-release clean $'5\n10\n50\n4' 1 <<'SC'
ptr values = new int[0];
values.push(10); values.push(20); values.push(30);
values.push(40); values.push(50);
print(values.len()); print(values[0]); print(values[4]);
values.pop(); print(values.len());
del values;
SC
check_case empty-release clean '0' 1 <<'SC'
ptr values = new int[0];
print(values.len());
del values;
SC
check_case file-roundtrip-eof clean $'first\nsecond\n0\nfirst' 1 <<'SC'
file output = open("roundtrip.txt", "w");
write(output, "first\nsecond"); close(output);
file input = open("roundtrip.txt", "r");
str first = read(input); str second = read(input); str eof = read(input);
print(first); print(second); print(len(eof)); print(first);
close(input);
SC
check_case dynamic-negative-read runtime 'SC runtime error: array index out of bounds' <<'SC'
ptr values = new int[2];
int index = -1;
print(values[index]);
SC
check_case dynamic-upper-write runtime 'SC runtime error: array index out of bounds' <<'SC'
ptr values = new int[2];
i64 index = values.len();
values[index] = 9;
SC
check_case struct-array-field-cleared-read runtime 'SC runtime error: array index out of bounds' <<'SC'
struct S { int[] a; }
S s;
s.a.push(1);
s.a.clear();
print(s.a[0]);
SC
check_case missing-file runtime "SC runtime error: cannot open file 'missing/file.txt'" <<'SC'
file input = open("missing/file.txt", "r");
str line = read(input);
print(line);
SC
# Self-tests of the ASan detector. Direct use after del / double del is a compile
# error (test.sh: use-after-del, double-del), and so is del of data(s); a pointer that
# arrives as a function parameter is not tracked (spec §9), so these reach run time.
check_case use-after-delete detector heap-use-after-free <<'SC'
fn void release(ptr p) { del p; }
string text = str(123456);
ptr chars = data(text);
release(chars);
print(chars[0]);
SC
check_case double-delete detector attempting\ double-free <<'SC'
fn void release(ptr p) { del p; }
string text = str(123456);
ptr chars = as_ptr(text);
release(chars);
release(chars);
SC
check_case raw-pointer-overrun detector heap-buffer-overflow <<'SC'
string text = str(7);
ptr chars = data(text);
print(chars[4]);
SC

echo 'PASS: memory safety suite (14 clean, 4 controlled runtime errors, 3 ASan detector self-tests)'
