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
// serial traffic must never look like a key press. A press is only counted
// in a 1 ms tick when the line stays low, read continuously, for a whole
// PTT_WINDOW_US: longer than one UART character at 38400 baud (260 us), so
// the high stop bit of any character, and the idle level between
// characters, always shows. PTT_PressMs such ticks in a row key the radio;
// PTT_ReleaseMs ticks with the line high release it. The serial PTT lock
// (SerialConfigInProgress) forces the released state at once.

#ifndef PTT_H
#define PTT_H

#include <stdbool.h>
#include <stdint.h>

#define PTT_WINDOW_US         280u   // one character at 38400 baud is 260.4 us

#define PTT_DEBOUNCE_MIN_MS   1u
#define PTT_DEBOUNCE_MAX_MS   40u
#define PTT_PRESS_DEFAULT_MS  5u
#define PTT_RELEASE_DEFAULT_MS 3u

typedef struct {
	uint8_t count;     // consecutive ticks towards the other state
	bool    pressed;   // debounced state
} PttDebounce_t;

// One 1 ms tick of the debouncer. lowWholeWindow: the line was low for all
// of PTT_WINDOW_US; lineLow: a single read of the line. Returns the state.
bool PTT_Debounce(PttDebounce_t *d, bool lowWholeWindow, bool lineLow, bool serialLock,
                  uint8_t pressMs, uint8_t releaseMs);

// True if lineLow() stays true until elapsedUs() reaches PTT_WINDOW_US;
// returns at the first high read.
bool PTT_LowForWindow(bool (*lineLow)(void), uint32_t (*elapsedUs)(void));

// Firmware only: called from SystickHandler every 1 ms.
void PTT_Tick(void);
bool PTT_IsPressed(void);

#endif
