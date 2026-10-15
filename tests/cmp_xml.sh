#!/bin/sh
# Checks ragekit's CodeWalker XML writers (include/ragekit/xml.h) against CodeWalker.
#   tests/cmp_xml.sh <list.txt> <work dir>
# list.txt: one .ydr/.ydd/.ybn/.ytd path per line (file names must be unique). Needs build/Release (rk_xml_test) and the oracle
# (dotnet; override the CodeWalker.Core.csproj with ORACLE_CW=path). Steps:
#   1 oracle cwxmllist     CodeWalker's own XML + DDS for every file             -> <work>/cw
#   2 rk_xml_test gen      ragekit's XML + DDS, compared with step 1 (text)      -> <work>/rk
#   3 oracle xmlinlist     CodeWalker imports ragekit's XML and saves binaries    -> <work>/imp
#   4 rk_xml_test summary  ragekit reads original and re-import, compares them
set -e
LIST="$1"; WORK="$2"
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$HERE/.."
export PATH="/c/msys64/ucrt64/bin:$PATH"
dotnet build -c Release "$ROOT/tests/oracle" ${ORACLE_CW:+-p:CodeWalkerCore="$ORACLE_CW"} >/dev/null
ORACLE="$ROOT/tests/oracle/bin/Release/net9.0/oracle.exe"
TEST="$ROOT/build/Release/rk_xml_test.exe"
mkdir -p "$WORK"
rm -rf "$WORK/cw" "$WORK/rk" "$WORK/imp"
"$TEST" selftest
"$TEST" synth "$WORK/synth"
cp "$LIST" "$WORK/list.txt"
for f in "$WORK"/synth/*; do echo "$(cd "$WORK/synth" && pwd -W 2>/dev/null || pwd)/$(basename "$f")" >> "$WORK/list.txt"; done
LIST="$WORK/list.txt"
"$ORACLE" cwxmllist "$LIST" "$WORK/cw" | tail -3
"$TEST" gen "$LIST" "$WORK/rk" "$WORK/cw" | tail -80 || true
"$ORACLE" xmlinlist "$WORK/rk/xmlin.txt" "$WORK/imp" | tail -5
"$TEST" summary "$LIST" "$WORK/imp" | tail -60 || true
