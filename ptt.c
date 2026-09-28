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

#include "ptt.h"

bool PTT_Debounce(PttDebounce_t *d, bool lowWholeWindow, bool lineLow, bool serialLock,
                  uint8_t pressMs, uint8_t releaseMs)
{
	if (serialLock) {
		d->pressed = false;
		d->count   = 0;
		return false;
	}

	if (!d->pressed) {
		if (!lowWholeWindow) {
			d->count = 0;
		}
		else if (++d->count >= pressMs) {
			d->pressed = true;
			d->count   = 0;
		}
	}
	else {
		if (lineLow) {
			d->count = 0;
		}
		else if (++d->count >= releaseMs) {
			d->pressed = false;
			d->count   = 0;
		}
	}

	return d->pressed;
}

bool PTT_LowForWindow(bool (*lineLow)(void), uint32_t (*elapsedUs)(void))
{
	do {
		if (!lineLow())
			return false;
	} while (elapsedUs() < PTT_WINDOW_US);

	return true;
}

#ifndef PTT_HOST_TEST

#include "ARMCM0.h"
#include "bsp/dp32g030/gpio.h"
#include "driver/gpio.h"
#include "misc.h"
#include "settings.h"

static PttDebounce_t gPtt;
static uint32_t      gWindowStart;

static bool LineLow(void)
{
	return !GPIO_CheckBit(&GPIOC->DATA, GPIOC_PIN_PTT);
}

// SysTick counts down from LOAD at 48 MHz
static uint32_t ElapsedUs(void)
{
	const uint32_t now    = SysTick->VAL;
	const uint32_t period = SysTick->LOAD + 1;
	const uint32_t ticks  = (gWindowStart >= now) ? gWindowStart - now : gWindowStart + period - now;
	return ticks / 48;
}

void PTT_Tick(void)
{
	const bool low = LineLow();
	bool lowWholeWindow = false;

	// only a candidate press costs the 280 us busy read
	if (low && !gPtt.pressed && !SerialConfigInProgress()) {
		gWindowStart   = SysTick->VAL;
		lowWholeWindow = PTT_LowForWindow(LineLow, ElapsedUs);
	}

	// the settings are loaded after SysTick starts: use the defaults until then
	const uint8_t pressMs   = gEeprom.PTT_PRESS_MS   ? gEeprom.PTT_PRESS_MS   : PTT_PRESS_DEFAULT_MS;
	const uint8_t releaseMs = gEeprom.PTT_RELEASE_MS ? gEeprom.PTT_RELEASE_MS : PTT_RELEASE_DEFAULT_MS;

	PTT_Debounce(&gPtt, lowWholeWindow, low, SerialConfigInProgress(), pressMs, releaseMs);
}

bool PTT_IsPressed(void)
{
	return gPtt.pressed;
}

#endif
