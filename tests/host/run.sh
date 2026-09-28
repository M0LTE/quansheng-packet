#!/bin/sh
# Build and run the host-side logic tests with the system gcc.
set -e
cd "$(dirname "$0")/../.."
out="${TMPDIR:-/tmp}/k5-packet-fw-host-test"
CFLAGS="-std=c2x -O2 -Wall -Wextra -Werror -fshort-enums -I ."

gcc $CFLAGS -o "$out" \
	tests/host/test_host.c settings.c radio.c frequencies.c misc.c driver/eeprom.c
"$out"

gcc $CFLAGS -I tests/host -I external/CMSIS_5/Device/ARM/ARMCM0/Include -include tests/host/uart_shim.h -o "$out-uart" \
	tests/host/test_uart.c app/uart.c misc.c
"$out-uart"
