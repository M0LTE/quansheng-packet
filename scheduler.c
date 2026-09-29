/* Copyright 2023 Dual Tachyon
 * https://github.com/DualTachyon
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

#include "functions.h"
#include "misc.h"
#include "outq.h"
#include "ptt.h"

#define DECREMENT(cnt) \
	do {               \
		if (cnt > 0)   \
			cnt--;     \
	} while (0)

#define DECREMENT_AND_TRIGGER(cnt, flag) \
	do {                                 \
		if (cnt > 0)                     \
			if (--cnt == 0)              \
				flag = true;             \
	} while (0)

static volatile uint32_t gGlobalSysTickCounter;
static uint8_t           gTicks1ms;

void SystickHandler(void);

// we come here every 1 ms: the clock, the UART output, the serial PTT lock
// and PTT every tick, everything else every 10 ms
void SystickHandler(void)
{
	g_ms++;

	OUTQ_Drain();

	DECREMENT(gSerialLockMs);
	DECREMENT(gReloadQuietMs);

	PTT_Tick();

	if (++gTicks1ms < 10)
		return;
	gTicks1ms = 0;

	gGlobalSysTickCounter++;

	gNextTimeslice = true;

	if ((gGlobalSysTickCounter % 50) == 0) {
		gNextTimeslice_500ms = true;

		DECREMENT_AND_TRIGGER(gTxTimerCountdown_500ms, gTxTimeoutReached);
	}

	DECREMENT(boot_counter_10ms);
}
