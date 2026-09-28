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

// PTT input, sampled every 1 ms from the SysTick interrupt.
//
// The PTT line on the K1 connector is shared with the UART receive line, so
// serial traffic must never look like a key press, nor hold off a release.
//
// Press: a tick counts only when the line stays low, read continuously, for
// a whole PTT_WINDOW_US: longer than one UART character at 38400 baud
// (260 us), so the high stop bit of any character, and the idle level
// between characters, always shows. PTT_PressMs such ticks in a row key.
//
// Release: while keyed, a tick counts towards release when the line reads
// high and stays high for PTT_HIGH_HOLD_US, or when a check finds it is no
// longer low for a whole window (UART traffic started right after the
// release). A high spike followed by a whole low window is a held press
// and starts the count again. The window check runs on every 4th tick while
// keyed, and on every tick once a release has started. PTT_ReleaseMs
// counting ticks release.
//
// The serial PTT lock (SerialConfigInProgress) forces the released state.

#ifndef PTT_H
#define PTT_H

#include <stdbool.h>
#include <stdint.h>

#define PTT_WINDOW_US          280u   // one character at 38400 baud is 260.4 us
#define PTT_HIGH_HOLD_US       20u

#define PTT_PRESS_MIN_MS       1u
#define PTT_RELEASE_MIN_MS     2u
#define PTT_DEBOUNCE_MAX_MS    40u
#define PTT_PRESS_DEFAULT_MS   5u
#define PTT_RELEASE_DEFAULT_MS 5u

typedef struct {
	uint8_t count;     // consecutive ticks towards the other state
	bool    pressed;   // debounced state
} PttDebounce_t;

// One 1 ms tick of the debouncer.
//   lowWholeWindow: the line was low for all of PTT_WINDOW_US
//   releaseTick:    (keyed) this tick counts towards release
bool PTT_Debounce(volatile PttDebounce_t *d, bool lowWholeWindow, bool releaseTick, bool serialLock,
                  uint8_t pressMs, uint8_t releaseMs);

// True if lineLow() returns want for every read until elapsedUs() reaches
// us; returns false at the first read that differs.
bool PTT_LevelFor(bool (*lineLow)(void), uint32_t (*elapsedUs)(void), bool wantLow, uint32_t us);

// Called from SystickHandler every 1 ms.
void PTT_Tick(void);
bool PTT_IsPressed(void);

#ifdef PTT_HOST_TEST
// hardware hooks the host tests provide
bool     PTT_HwLineLow(void);
uint32_t PTT_HwTicks(void);       // a 48 MHz down-counter like SysTick->VAL
uint32_t PTT_HwPeriod(void);      // its reload period
bool     PTT_HwSerialLock(void);
uint8_t  PTT_HwPressMs(void);
uint8_t  PTT_HwReleaseMs(void);
void     PTT_HostReset(void);
#endif

#endif
