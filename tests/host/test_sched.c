/* Copyright 2026 packet-fw contributors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 *     Unless required by applicable law or agreed to in writing, software
 *     distributed under the License is distributed on an "AS IS" BASIS,
 *     WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *     See the License for the specific language governing permissions and
 *     limitations under the License.
 */

// Host test of scheduler.c: the 1 ms SysTick handler keeps the ms clock,
// drains the UART output, counts the serial PTT lock and the reload quiet
// time down, samples PTT every tick, and keeps the 10 ms and 500 ms slices
// and their counters.

#include <stdio.h>

#include "misc.h"
#include "ptt.h"

void SystickHandler(void);

static int ptt_ticks;
static uint16_t lock_seen_by_ptt;
void PTT_Tick(void) { ptt_ticks++; lock_seen_by_ptt = gSerialLockMs; }
static int drains;
void OUTQ_Drain(void) { drains++; }

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

int main(void)
{
	int slices10 = 0, slices500 = 0;
	boot_counter_10ms = 250;
	gTxTimerCountdown_500ms = 10;       // 5 s
	gSerialLockMs = 20;
	gReloadQuietMs = SERIAL_RELOAD_QUIET_MS;

	for (int ms = 1; ms <= 5000; ms++) {
		SystickHandler();
		if (gNextTimeslice)       { slices10++;  gNextTimeslice = false; }
		if (gNextTimeslice_500ms) { slices500++; gNextTimeslice_500ms = false; }
		if (ms == 2490) CHECK(boot_counter_10ms == 1);
		if (ms == 4999) CHECK(!gTxTimeoutReached);
		if (ms == 1)  CHECK(lock_seen_by_ptt == 19);     // counted down before PTT sees it
		if (ms == 19) CHECK(gSerialLockMs == 1);
		if (ms == 20) CHECK(gSerialLockMs == 0);
		if (ms == 999) CHECK(gReloadQuietMs == 1);
		CHECK(g_ms == (uint32_t)ms);
	}

	CHECK(ptt_ticks == 5000);
	CHECK(slices10 == 500);
	CHECK(slices500 == 10);
	CHECK(boot_counter_10ms == 0);
	CHECK(gTxTimeoutReached);
	CHECK(gSerialLockMs == 0);
	CHECK(gReloadQuietMs == 0);
	CHECK(drains == 5000);                                  // the UART output every tick

	if (failures) {
		printf("%d check(s) failed\n", failures);
		return 1;
	}
	printf("all scheduler host tests passed\n");
	return 0;
}
