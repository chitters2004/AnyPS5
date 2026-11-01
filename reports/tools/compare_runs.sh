#!/bin/bash
if [ $# -ne 3 ]; then
    echo "usage: compare_runs.sh <output directory> <baseline label> <candidate label>" >&2
    exit 2
fi
out=$1
echo "results that differ ($2 < > $3):"
diff "$out/$2.results" "$out/$3.results"
echo "per-test VUID counts that differ ($2 < > $3):"
diff "$out/$2.pertest" "$out/$3.pertest"
echo "end"
