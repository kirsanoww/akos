#!/bin/sh
set -eu
mkdir -p logs
for mode in named unnamed independent; do
    for scenario in success errors; do
        if [ "$scenario" = success ]; then
            set -- --parcels 40 --error-percent 0
        else
            set -- --parcels 17 --error-percent 100 --retries 3
        fi
        file="logs/${mode}_${scenario}.txt"
        if [ "$mode" = independent ]; then
            python3 scripts/run_independent.py "$@" --check --log "$file" > /dev/null
        else
            "./build/center_$mode" "$@" --check --log "$file" > /dev/null
        fi
        printf '%s\n' "$mode / $scenario"
        tail -n 2 "$file"
    done
done
