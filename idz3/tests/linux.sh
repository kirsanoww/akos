#!/bin/sh
set -eu
if [ "$(uname -s)" != Linux ]; then
    printf '%s\n' 'Запустите этот сценарий под Linux или в Docker' >&2
    exit 1
fi
make test
make sanitize
sh tests/demo.sh
