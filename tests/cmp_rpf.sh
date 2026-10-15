#!/bin/bash
# Compares `rk rpf ls -r --tsv --hash --nested` with the oracle's CodeWalker listing of the same archive.
#   tests/cmp_rpf.sh <rpf (unix path)> <rpf (windows path)> <tag> "<game folder with GTA5.exe>"
# Writes $TEMP/rk_<tag>.tsv, $TEMP/cw_<tag>.tsv and $TEMP/diff_<tag>.txt; prints IDENTICAL or the first differences.
export PATH=/c/msys64/ucrt64/bin:$PATH
cd "$(dirname "$0")/.."
G="$4"
./build/Release/rk.exe rpf ls "$1" -r --tsv --hash --nested 2>"$TEMP/rk_$3.err" | tr -d '\015' | sort > "$TEMP/rk_$3.tsv"
dotnet run -c Release --no-build --project tests/oracle -- rpf "$G" "$2" --hash --nested 2>"$TEMP/cw_$3.err" | tr -d '\015' | sort > "$TEMP/cw_$3.tsv"
wc -l "$TEMP/rk_$3.tsv" "$TEMP/cw_$3.tsv"
if diff "$TEMP/rk_$3.tsv" "$TEMP/cw_$3.tsv" > "$TEMP/diff_$3.txt"; then echo IDENTICAL; else echo DIFF; wc -l "$TEMP/diff_$3.txt"; head -10 "$TEMP/diff_$3.txt"; fi
