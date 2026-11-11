#!/bin/sh
# Hidden, muted test run: ./run_test.sh <seconds> ; extra STG_* env vars pass through.
cd "$(dirname "$0")"
S=${1:-15}
STG_HIDDEN=1 STG_MUTE=1 STG_QUIT=$((S*1000)) STG_REGFILE=${STG_REGFILE:-test_registry.txt} timeout $((S+20)) ./Generations.exe
echo "exit $?"
tail -n ${TAIL:-40} generations.log
