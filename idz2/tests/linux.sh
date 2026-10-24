#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
if [ "$(uname -s)" != "Linux" ]; then
    echo "Этот сценарий нужно запускать в Linux" >&2
    exit 1
fi
make -B test
make sanitize
make tsan
make -B all
sh tests/demo.sh
