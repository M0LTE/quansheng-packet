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

// Host test of scheduler.c: the 1 ms SysTick handler samples PTT every
// tick and keeps the 10 ms and 500 ms slices and their counters.

#include <stdio.h>

#include "misc.h"
#include "ptt.h"

void SystickHandler(void);

static int ptt_ticks;
void PTT_Tick(void) { ptt_ticks++; }

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

int main(void)
{
	int slices10 = 0, slices500 = 0;
	boot_counter_10ms = 250;
	gTxTimerCountdown_500ms = 10;       // 5 s
	gSerialConfigCountDown_500ms = SERIAL_PTT_LOCK_500ms;

	for (int ms = 1; ms <= 5000; ms++) {
		SystickHandler();
		if (gNextTimeslice)       { slices10++;  gNextTimeslice = false; }
		if (gNextTimeslice_500ms) { slices500++; gNextTimeslice_500ms = false; }
		if (ms == 2490) CHECK(boot_counter_10ms == 1);
		if (ms == 4999) CHECK(!gTxTimeoutReached);
	}

	CHECK(ptt_ticks == 5000);
	CHECK(slices10 == 500);
	CHECK(slices500 == 10);
	CHECK(boot_counter_10ms == 0);
	CHECK(gTxTimeoutReached);
	CHECK(gSerialConfigCountDown_500ms == 0);

	if (failures) {
		printf("%d check(s) failed\n", failures);
		return 1;
	}
	printf("all scheduler host tests passed\n");
	return 0;
}
