#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
make
mkdir -p logs
./build/sorting_center --parcels 10 --error-percent 0 --log logs/no_errors.log
./build/sorting_center --parcels 10 --error-percent 100 --retries 2 --log logs/all_errors.log
./build/sorting_center --parcels 20 --directions 1 --lines 4 --buffer-capacity 1 \
    --output-capacity 1 --unload-interval 30 --error-percent 0 --log logs/congestion.log
