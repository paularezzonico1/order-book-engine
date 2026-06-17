#!/usr/bin/env bash
#
# sweep.sh — run the benchmark across a grid of book depths and order counts,
# emitting one CSV file. Each row is one (depth, commands) configuration with
# throughput and latency percentiles.
#
# Usage:
#   bench/sweep.sh [path-to-obe_bench] [output.csv]
#
# Defaults assume a Release build at build-rel/obe_bench and write to
# bench/results/sweep_<timestamp>.csv.
#
# Example:
#   cmake -S . -B build-rel -DCMAKE_BUILD_TYPE=Release && cmake --build build-rel
#   bench/sweep.sh
#   column -s, -t bench/results/sweep_*.csv   # pretty-print

set -euo pipefail

BENCH="${1:-build-rel/obe_bench}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
RESULTS_DIR="${SCRIPT_DIR}/results"
OUT="${2:-${RESULTS_DIR}/sweep_$(date +%Y%m%d_%H%M%S).csv}"

# Parameter grid: book depth (price levels around the mid) x order volume.
DEPTHS=(1 5 20 50 100 250)
COMMANDS=(100000 1000000)

if [[ ! -x "${BENCH}" ]]; then
    echo "error: benchmark binary not found/executable at '${BENCH}'" >&2
    echo "build it first: cmake --build build-rel --target obe_bench" >&2
    exit 1
fi

mkdir -p "${RESULTS_DIR}"

# Write the CSV header once (the binary knows the column layout).
"${BENCH}" --header > "${OUT}"

echo "sweeping depths={${DEPTHS[*]}} x commands={${COMMANDS[*]}} -> ${OUT}"
for n in "${COMMANDS[@]}"; do
    for d in "${DEPTHS[@]}"; do
        echo "  depth=${d} commands=${n}"
        "${BENCH}" --csv --depth "${d}" --commands "${n}" >> "${OUT}"
    done
done

echo "done. results in ${OUT}"
echo
column -s, -t "${OUT}" 2>/dev/null || cat "${OUT}"
