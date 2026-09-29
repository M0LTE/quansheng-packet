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

bool PTT_Debounce(volatile PttDebounce_t *d, bool lowWholeWindow, bool releaseTick,
                  uint8_t pressMs, uint8_t releaseMs)
{
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
		if (releaseTick) {
			if (++d->count >= releaseMs) {
				d->pressed = false;
				d->count   = 0;
			}
		}
		else if (lowWholeWindow) {
			d->count = 0;          // still held
		}
	}

	return d->pressed;
}

bool PTT_LevelFor(bool (*lineLow)(void), uint32_t (*elapsedUs)(void), bool wantLow, uint32_t us)
{
	do {
		if (lineLow() != wantLow)
			return false;
	} while (elapsedUs() < us);

	return true;
}

// ------------------------------------------------------------- hardware --

#ifndef PTT_HOST_TEST

#include "ARMCM0.h"
#include "bsp/dp32g030/gpio.h"
#include "driver/gpio.h"
#include "misc.h"
#include "settings.h"

static inline bool     PTT_HwLineLow(void)    { return !GPIO_CheckBit(&GPIOC->DATA, GPIOC_PIN_PTT); }
static inline uint32_t PTT_HwTicks(void)      { return SysTick->VAL; }
static inline uint32_t PTT_HwPeriod(void)     { return SysTick->LOAD + 1; }
static inline uint16_t PTT_HwLock(void)       { return gSerialLockMs; }
static inline uint32_t PTT_HwNow(void)        { return g_ms; }
// the settings are loaded after SysTick starts: defaults until then
static inline uint8_t  PTT_HwPressMs(void)    { return gEeprom.PTT_PRESS_MS   ? gEeprom.PTT_PRESS_MS   : PTT_PRESS_DEFAULT_MS; }
static inline uint8_t  PTT_HwReleaseMs(void)  { return gEeprom.PTT_RELEASE_MS ? gEeprom.PTT_RELEASE_MS : PTT_RELEASE_DEFAULT_MS; }

#endif

// shared between the SysTick interrupt (writer) and the main loop (reader)
static volatile PttDebounce_t gPtt;
static uint32_t               gWindowStart;

static volatile PttState_t    gState;
static uint32_t               gFirstTick;

#ifdef PTT_HOST_TEST
	#define IRQ_OFF()
	#define IRQ_ON()
void PTT_HostReset(void) { gPtt.pressed = false; gPtt.count = 0; gState.pressed = false; gState.candidate = false; }
#else
	#define IRQ_OFF() __disable_irq()
	#define IRQ_ON()  __enable_irq()
#endif

static bool LineLow(void)
{
	return PTT_HwLineLow();
}

// SysTick counts down from LOAD at 48 MHz
static uint32_t ElapsedUs(void)
{
	const uint32_t now    = PTT_HwTicks();
	const uint32_t ticks  = (gWindowStart >= now) ? gWindowStart - now : gWindowStart + PTT_HwPeriod() - now;
	return ticks / 48;
}

void PTT_Tick(void)
{
	static uint8_t n;
	const bool low  = LineLow();
	bool lowWholeWindow = false;
	bool releaseTick    = false;

	n++;

	if (!gPtt.pressed) {
		// only a candidate press costs the 280 us busy read
		if (low) {
			gWindowStart   = PTT_HwTicks();
			lowWholeWindow = PTT_LevelFor(LineLow, ElapsedUs, true, PTT_WINDOW_US);
		}
	}
	else if (!low) {
		// high: released, unless it was only a spike in a held press
		gWindowStart = PTT_HwTicks();
		if (PTT_LevelFor(LineLow, ElapsedUs, false, PTT_HIGH_HOLD_US)) {
			releaseTick = true;
		}
		else {
			gWindowStart   = PTT_HwTicks();
			lowWholeWindow = PTT_LevelFor(LineLow, ElapsedUs, true, PTT_WINDOW_US);
			releaseTick    = !lowWholeWindow;     // UART traffic, not a press
		}
	}
	else if (gPtt.count > 0 || (n & 3) == 0) {
		// low: check it is still a real press (while releasing, and on
		// every 4th tick otherwise, so UART traffic that starts right
		// after a release cannot hold the radio keyed; about 7% CPU
		// while transmitting)
		gWindowStart   = PTT_HwTicks();
		lowWholeWindow = PTT_LevelFor(LineLow, ElapsedUs, true, PTT_WINDOW_US);
		releaseTick    = !lowWholeWindow;
	}

	const bool    wasPressed = gPtt.pressed;
	const uint8_t before     = gPtt.count;

	PTT_Debounce(&gPtt, lowWholeWindow, releaseTick, PTT_HwPressMs(), PTT_HwReleaseMs());

	// edge times for the v2 events and the lock rules
	const uint32_t now = PTT_HwNow();
	if (gPtt.pressed != wasPressed) {
		const uint32_t first = (before == 0) ? now : gFirstTick;
		if (gPtt.pressed) {
			gState.tPress      = first;
			gState.tPressEdge  = now;
			gState.lockAtPress = PTT_HwLock();
			gState.pressCount++;
		}
		else
			gState.tRelease = first;
	}
	else if (before == 0 && gPtt.count == 1)
		gFirstTick = now;

	gState.pressed   = gPtt.pressed;
	gState.candidate = !gPtt.pressed && gPtt.count > 0;
}

void PTT_GetState(PttState_t *s)
{
	IRQ_OFF();          // a dozen loads
	s->pressed     = gState.pressed;
	s->candidate   = gState.candidate;
	s->pressCount  = gState.pressCount;
	s->lockAtPress = gState.lockAtPress;
	s->tPress      = gState.tPress;
	s->tPressEdge  = gState.tPressEdge;
	s->tRelease    = gState.tRelease;
	IRQ_ON();
}

bool PTT_IsPressed(void)
{
	return gPtt.pressed;
}
