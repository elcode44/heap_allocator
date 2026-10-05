#!/bin/sh
# Runs real programs on libmyalloc.so and checks two things for each one:
#   1. its output is IDENTICAL to a normal run (so our malloc did not break the program)
#   2. our allocator was really used (MYALLOC_STATS shows up in a log file)
# Usage: make test-preload     (or: sh test_preload.sh [path/to/lib.so])

LIB=${1:-./libmyalloc.so}
[ -f "$LIB" ] || { echo "build the library first: make lib"; exit 1; }
LIB=$(realpath "$LIB")
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
fail=0
count=0

# check NAME 'shell command'  -> runs it normally, then again with the library preloaded
check() {
    name=$1
    cmd=$2
    count=$((count + 1))

    sh -c "$cmd" > "$WORK/expected.out" 2>"$WORK/expected.err"
    expected_status=$?

    : > "$WORK/stats.log"
    LD_PRELOAD="$LIB" MYALLOC_STATS="$WORK/stats.log" timeout 120 sh -c "$cmd" > "$WORK/actual.out" 2>"$WORK/actual.err"
    actual_status=$?

    if [ $expected_status -ne $actual_status ]; then
        echo "FAIL: $name (exit status $actual_status, normal run gave $expected_status)"
        head -3 "$WORK/actual.err"
        fail=$((fail + 1))
    elif ! cmp -s "$WORK/expected.out" "$WORK/actual.out"; then
        echo "FAIL: $name (output differs from a normal run)"
        fail=$((fail + 1))
    elif ! grep -q "myalloc" "$WORK/stats.log"; then
        echo "FAIL: $name (ran, but our allocator was not used)"
        fail=$((fail + 1))
    else
        # every process that ran (the shell, the program, its children) appends one line, so show
        # how many processes used us, the most arenas any of them needed, and the most memory any
        # of them still had mapped from the OS at exit
        summary=$(awk '{ n++; for (i = 1; i <= NF; i++) { if ($i == "used:" && $(i+1) + 0 > a) a = $(i+1) + 0; if ($i == "OS:" && $(i+1) + 0 > m) m = $(i+1) + 0 } } END { print n " processes, up to " a " arena(s), up to " m " KB mapped at exit" }' "$WORK/stats.log")
        echo "PASS: $name   ($summary)"
    fi
}

# lots of small allocations (directory entries, strings)
check "ls -R /usr/include"            "ls -R /usr/include | md5sum"
check "find /usr/lib"                 "find /usr/lib -type f | sort | md5sum"

# big buffers that grow with realloc, plus large allocations
seq 1 300000 | awk '{ print (($1 * 7919) % 100003) " line " $1 }' > "$WORK/numbers.txt"
check "sort 300k lines"               "sort $WORK/numbers.txt | md5sum"
check "awk word counts"               "awk '{ c[\$1]++ } END { n = 0; for (k in c) n += c[k]; print n, length(c) }' $WORK/numbers.txt"
check "sed + gzip pipeline"           "sed 's/line/LINE/' $WORK/numbers.txt | gzip -6 | gunzip | md5sum"
check "tar create + extract"          "tar -cf $WORK/inc.tar -C /usr include && mkdir -p $WORK/x && tar -xf $WORK/inc.tar -C $WORK/x && find $WORK/x -type f | wc -l"

# interpreters: these allocate heavily and use every function (realloc, calloc, aligned allocs)
check "perl hash of 200k keys"        "perl -e 'my %h; \$h{\$_ * 31} = \$_ for 1..200000; my \$s = 0; \$s += \$_ for values %h; print \"\$s\\n\"'"
if command -v python3 >/dev/null 2>&1; then
    cat > "$WORK/work.py" <<'PY'
import hashlib, json, threading, random
random.seed(1)
data = {str(i): [i, str(i) * 3, {"k": i % 7}] for i in range(60000)}
text = json.dumps(data)
back = json.loads(text)
assert back == data
blob = bytearray()
for i in range(2000):
    blob += bytes([i % 256]) * 997          # grows by realloc many times
print(hashlib.sha256(bytes(blob)).hexdigest())
big = [bytearray(300000) for _ in range(20)]  # large blocks (mmap path)
print(sum(len(b) for b in big))
results = []
def worker(n):
    s = 0
    for i in range(200000):
        s += len(str(i * n))
    results.append(s)
threads = [threading.Thread(target=worker, args=(n,)) for n in range(1, 9)]
[t.start() for t in threads]; [t.join() for t in threads]
print(sorted(results)[:3], len(text))
PY
    check "python3 json/threads/bytearray" "python3 $WORK/work.py"
fi

# a compiler: forks and execs child processes, and uses a lot of memory
cat > "$WORK/hello.c" <<'C'
#include <stdio.h>
int main(void){ for (int i = 0; i < 3; i++) printf("hello %d\n", i); return 0; }
C
check "gcc compile + run"             "gcc -O2 -o $WORK/hello $WORK/hello.c && $WORK/hello"

echo
if [ $fail -eq 0 ]; then
    echo "ALL $count REAL-PROGRAM CHECKS PASSED"
else
    echo "$fail of $count CHECKS FAILED"
    exit 1
fi
