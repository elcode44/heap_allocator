#!/bin/sh
# One command to measure the allocator:   sh run_bench.sh      (or: make bench-threads)
#
# Runs the SAME benchmark program (bench_threads) on three allocators, at 1, 2, 4 and 8 threads,
# several times each, and prints a table of the median throughput:
#   glibc          the C library's normal malloc (no preload)
#   myalloc        this allocator, one arena per thread
#   myalloc-1lock  this allocator with every thread forced into ONE arena (= one global lock)
#
# Settings (environment variables):
#   THREADS="1 2 4 8"                  thread counts to test
#   REPEATS=5                          how many times each measurement is repeated (median is reported)
#   OPS=                               ops per thread (default: workload specific)
#   WORKLOADS="private handoff realloc"
#   OUT=results                        folder for results.csv, machine.txt and summary.md
#
# Honest-benchmark rules built in: allocators are interleaved inside every repetition (so a noisy
# moment hits all three), the machine is recorded, and thread counts above the number of cores are
# flagged, because past that point the numbers measure the OS scheduler, not the allocator.

THREADS=${THREADS:-"1 2 4 8"}
REPEATS=${REPEATS:-5}
WORKLOADS=${WORKLOADS:-"private handoff realloc"}
OUT=${OUT:-results}
OPS=${OPS:-}

cd "$(dirname "$0")" || exit 1
mkdir -p "$OUT"

echo "building..."
make -s lib libmyalloc_single.so bench_threads || { echo "build failed"; exit 1; }

CORES=$(nproc)
{
    echo "date:      $(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "cpu:       $(grep -m1 'model name' /proc/cpuinfo 2>/dev/null | cut -d: -f2- | sed 's/^ *//')"
    echo "cores:     $CORES (logical)"
    echo "kernel:    $(uname -sr)"
    echo "glibc:     $(ldd --version 2>/dev/null | head -1)"
    echo "compiler:  $(gcc --version | head -1)"
    echo "alignment: $(grep -m1 'define MY_ALLOC_ALIGNMENT' include/malloc.h | awk '{print $3}') bytes"
    echo "repeats:   $REPEATS (median reported)"
} > "$OUT/machine.txt"
cat "$OUT/machine.txt"
echo

echo "allocator,workload,threads,run,mops,rss_kb" > "$OUT/results.csv"

run_one() {   # run_one <allocator-name> <preload-lib-or-empty> <workload> <threads> <run>
    name=$1; lib=$2; w=$3; t=$4; r=$5
    if [ -n "$lib" ]; then
        line=$(LD_PRELOAD="$lib" ./bench_threads "$w" "$t" $OPS) || { echo "run failed: $name $w $t" >&2; return 1; }
    else
        line=$(./bench_threads "$w" "$t" $OPS) || { echo "run failed: $name $w $t" >&2; return 1; }
    fi
    # line = <workload> <threads> <total ops> <seconds> <mops> <rss kb>
    echo "$name,$w,$t,$r,$(echo "$line" | awk '{print $5 "," $6}')" >> "$OUT/results.csv"
}

for r in $(seq 1 "$REPEATS"); do
    echo "repetition $r of $REPEATS..."
    for w in $WORKLOADS; do
        for t in $THREADS; do
            [ "$w" = "handoff" ] && [ "$t" -lt 2 ] && continue
            run_one glibc ""                        "$w" "$t" "$r"
            run_one myalloc "$PWD/libmyalloc.so"    "$w" "$t" "$r"
            run_one single  "$PWD/libmyalloc_single.so" "$w" "$t" "$r"
        done
    done
done

# stats <allocator> <workload> <threads> <column>  ->  "median min max"
stats() {
    grep "^$1,$2,$3," "$OUT/results.csv" | cut -d, -f"$4" | sort -g | awk '
        { a[NR] = $1 }
        END {
            if (NR == 0) { print "0 0 0"; exit }
            med = (NR % 2) ? a[(NR + 1) / 2] : (a[NR / 2] + a[NR / 2 + 1]) / 2
            print med, a[1], a[NR]
        }'
}

{
    echo "| workload | threads | glibc (M ops/s) | myalloc (M ops/s) | myalloc, 1 lock (M ops/s) | myalloc / glibc | myalloc / 1 lock | peak RSS glibc / myalloc (MB) |"
    echo "|---|---|---|---|---|---|---|---|"
    for w in $WORKLOADS; do
        for t in $THREADS; do
            [ "$w" = "handoff" ] && [ "$t" -lt 2 ] && continue
            set -- $(stats glibc "$w" "$t" 5);  gm=$1; glo=$2; ghi=$3
            set -- $(stats myalloc "$w" "$t" 5); mm=$1; mlo=$2; mhi=$3
            set -- $(stats single "$w" "$t" 5);  sm=$1; slo=$2; shi=$3
            set -- $(stats glibc "$w" "$t" 6);   grss=$1
            set -- $(stats myalloc "$w" "$t" 6); mrss=$1
            flag=""
            [ "$t" -gt "$CORES" ] && flag=" *"
            awk -v w="$w" -v t="$t" -v flag="$flag" \
                -v gm="$gm" -v glo="$glo" -v ghi="$ghi" -v mm="$mm" -v mlo="$mlo" -v mhi="$mhi" \
                -v sm="$sm" -v slo="$slo" -v shi="$shi" -v grss="$grss" -v mrss="$mrss" 'BEGIN {
                printf "| %s | %d%s | %.2f (%.2f-%.2f) | %.2f (%.2f-%.2f) | %.2f (%.2f-%.2f) | %.2fx | %.2fx | %.1f / %.1f |\n",
                    w, t, flag, gm, glo, ghi, mm, mlo, mhi, sm, slo, shi,
                    (gm > 0 ? mm / gm : 0), (sm > 0 ? mm / sm : 0), grss / 1024, mrss / 1024 }'
        done
    done
    echo
    echo "Cells show the median of $REPEATS runs, with (min-max) in brackets. Higher M ops/s is better."
    echo "\"myalloc / glibc\" above 1.00x means this allocator is faster than glibc on that row."
    echo "* = more threads than the $CORES logical core(s) on this machine: that row measures OS scheduling, not scaling."
} > "$OUT/summary.md"

echo
cat "$OUT/summary.md"
echo
echo "raw data: $OUT/results.csv   machine: $OUT/machine.txt   table: $OUT/summary.md"
