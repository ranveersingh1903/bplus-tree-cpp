#!/usr/bin/env bash
# Build, run all three B+ tree variants (each in its own process), then
# generate results/REPORT.md + results/plots/*.svg
#
#   ./run_all.sh --csv /Users/nileshmishra/Downloads/bplus_tree_data_10m.csv
#
# options: --limit N  --trees memory,disk,hybrid  --pool-mb M  --pool-sweep 32,128,512
#          --page-size B  --direct-io  --validate  --results-dir DIR
set -euo pipefail
cd "$(dirname "$0")"

CSV="/Users/nileshmishra/Downloads/bplus_tree_data_10m.csv"
TREES="memory,disk,hybrid"
POOL=128
SWEEP=""
RESULTS="results"
EXTRA=()
while [[ $# -gt 0 ]]; do
  case "$1" in
    --csv) CSV="$2"; shift 2 ;;
    --trees) TREES="$2"; shift 2 ;;
    --pool-mb) POOL="$2"; shift 2 ;;
    --pool-sweep) SWEEP="$2"; shift 2 ;;
    --results-dir) RESULTS="$2"; shift 2 ;;
    --limit|--page-size|--checkpoint|--hits|--misses|--ranges|--range-len) EXTRA+=("$1" "$2"); shift 2 ;;
    --direct-io|--validate|--no-pin-internal) EXTRA+=("$1"); shift ;;
    -h|--help) sed -n '2,10p' "$0"; exit 0 ;;
    *) echo "unknown option: $1"; exit 2 ;;
  esac
done

[[ -f "$CSV" ]] || { echo "CSV not found: $CSV"; exit 1; }
START=$(date +%s)
make -s all
mkdir -p "$RESULTS"
rm -f "$RESULTS"/*.json

IFS=',' read -ra TLIST <<< "$TREES"
for t in "${TLIST[@]}"; do
  args=(--tree "$t" --csv "$CSV" --results-dir "$RESULTS" ${EXTRA[@]+"${EXTRA[@]}"})
  [[ "$t" == "hybrid" ]] && args+=(--pool-mb "$POOL")
  echo; echo "\$ ./build/bptree_bench ${args[*]}"
  ./build/bptree_bench "${args[@]}"
done

if [[ -n "$SWEEP" ]]; then
  IFS=',' read -ra SLIST <<< "$SWEEP"
  for mb in "${SLIST[@]}"; do
    [[ "$mb" == "$POOL" && ",$TREES," == *",hybrid,"* ]] && continue
    ./build/bptree_bench --tree hybrid --pool-mb "$mb" --label "hybrid_${mb}MB" --csv "$CSV" \
        --results-dir "$RESULTS" ${EXTRA[@]+"${EXTRA[@]}"}
  done
fi

./build/bptree_report "$RESULTS"
echo; echo "All done in $(( ($(date +%s) - START) ))s  ->  $RESULTS/REPORT.md"
