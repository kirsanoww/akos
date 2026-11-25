#!/bin/sh
set -eu
python3 scripts/run_demo.py --parcels 40 --error-percent 0 --check
python3 scripts/run_demo.py --parcels 17 --error-percent 100 --retries 3 --check
