#!/bin/bash
# Compares the game asset index with CodeWalker's GameFileCache (oracle `index` mode) and explains the differences.
#   tests/cmp_index.sh "<game folder with GTA5.exe>"
# Needs the oracle built (cd tests/oracle && dotnet build -c Release) and the keys in ~/.ragekit/keys.
export PATH=/c/msys64/ucrt64/bin:$PATH
cd "$(dirname "$0")/.."
G="${1:-C:/Program Files/Rockstar Games/Grand Theft Auto V Legacy}"
OUT="${TEMP:-/tmp}/rk_index.tsv"
./build/Release/rk.exe index dump "$G" "$OUT" || exit 1
dotnet run -c Release --no-build --project tests/oracle -- index "$G" "$OUT" 2>&1 | grep -v '^codewalker:'
echo "---- rk index explain (why the picks differ)"
./build/Release/rk.exe index explain "$OUT.diffs" --game "$G"
