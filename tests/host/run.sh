#!/bin/sh
# Build and run the host-side logic tests with the system gcc.
#
#   tests/host/run.sh            run everything
#   tests/host/run.sh --vectors  also rewrite tests/vectors/protocol-v2.json
#
# The v2 test writes the golden vectors from the real firmware code on
# every run and fails if they differ from the committed file.
set -e
cd "$(dirname "$0")/../.."
out="${TMPDIR:-/tmp}/k5-packet-fw-host-test"
CFLAGS="-std=c2x -O2 -Wall -Wextra -Werror -fshort-enums -I ."
# the protocol tests also run under the address and undefined-behaviour
# sanitizers (misaligned access included), where the compiler has them
SAN="-fsanitize=address,undefined -fno-sanitize-recover=all"
echo 'int main(void){return 0;}' | gcc $SAN -x c -o /dev/null - 2>/dev/null || SAN=""
PROTO="$SAN -DHOST_TEST -I tests/host -I external/CMSIS_5/Device/ARM/ARMCM0/Include -include tests/host/uart_shim.h"
PROTO_SRC="tests/host/harness.c app/uart.c app/v2.c app/events.c app/params.c app/monitor.c outq.c settings.c radio.c frequencies.c misc.c driver/eeprom.c"

gcc $CFLAGS -o "$out" \
	tests/host/test_host.c settings.c radio.c frequencies.c misc.c driver/eeprom.c
"$out"

gcc $CFLAGS $PROTO -o "$out-uart" tests/host/test_uart.c $PROTO_SRC
"$out-uart"

gcc $CFLAGS $PROTO -o "$out-v2" tests/host/test_v2.c $PROTO_SRC
"$out-v2" "$out-vectors.json"
if [ "$1" = "--vectors" ]; then
	cp "$out-vectors.json" tests/vectors/protocol-v2.json
fi
if ! cmp -s "$out-vectors.json" tests/vectors/protocol-v2.json; then
	echo "FAIL: tests/vectors/protocol-v2.json differs from what the firmware code produces" >&2
	echo "      (run tests/host/run.sh --vectors if the change is intended)" >&2
	exit 1
fi
echo "golden vectors match tests/vectors/protocol-v2.json"

gcc $CFLAGS -DHOST_TEST -o "$out-outq" tests/host/test_outq.c outq.c
"$out-outq"

gcc $CFLAGS -o "$out-pttarb" tests/host/test_pttarb.c pttarb.c
"$out-pttarb"

gcc $CFLAGS -DPTT_HOST_TEST -o "$out-ptt" tests/host/test_ptt.c ptt.c
"$out-ptt"

gcc $CFLAGS -o "$out-sched" tests/host/test_sched.c scheduler.c misc.c
"$out-sched"
