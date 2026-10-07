#!/bin/sh
set -eu

if [ "$(uname -s)" != "Linux" ]; then
    echo "Эту проверку нужно запускать в Linux." >&2
    exit 1
fi

cd "$(dirname "$0")/.."
uname -sr
gcc --version
make -B CC=gcc test
make CC=gcc SANITIZERS=undefined sanitize
