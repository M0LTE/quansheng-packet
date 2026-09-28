#!/bin/sh
# Build and run the host-side logic tests with the system gcc.
set -e
cd "$(dirname "$0")/../.."
out="${TMPDIR:-/tmp}/k5-packet-fw-host-test"
gcc -std=c2x -O2 -Wall -Wextra -Werror -fshort-enums -I . \
	-o "$out" \
	tests/host/test_host.c settings.c radio.c frequencies.c misc.c driver/eeprom.c
"$out"
