#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
make
mkdir -p logs
./build/sorting_center --parcels 10 --error-percent 0 --check --log logs/no_errors.log
./build/sorting_center --parcels 10 --error-percent 100 --retries 2 --check --log logs/all_errors.log
./build/sorting_center --parcels 20 --directions 1 --buffer-capacity 1 --output-capacity 1 --unload-interval 25 --error-percent 0 --check --log logs/congestion.log
