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

// What a PTT press does, given the serial PTT lock (protocol v2, 5.3).
// Pure logic, stepped from the main loop with the debounced PTT state
// (ptt.h) and the lock; the caller keys, unkeys and reports.
//
//   press edge, lock 0, no bar         key
//   press edge, 0 < lock <= 30 ms      key when the lock reaches 0, if
//                                      still pressed (a late key)
//   press edge, lock > 30 ms           refuse (LOCK), latch until release
//   press edge, TX not allowed         refuse (reason), latch until release
//   release                            unkey, clear a pending key or latch
//   TX timeout or serial frame         latch until release (PTTARB_Latch)
//
// The lock that counts is the one at the press edge, recorded by the 1 ms
// tick, so a late main loop does not change the decision.

#ifndef PTTARB_H
#define PTTARB_H

#include <stdbool.h>
#include <stdint.h>

// TX_REFUSED reasons
enum {
	TXR_NONE = 0,
	TXR_LOCK,
	TXR_TX_BAND,
	TXR_BATTERY_EMPTY,
	TXR_OVER_VOLTAGE,
	TXR_REDUCED_SERVICE,
};

enum { ARB_IDLE, ARB_PENDING, ARB_KEYED, ARB_LATCHED };
enum { ARB_NONE, ARB_KEY, ARB_UNKEY, ARB_REFUSE };

typedef struct {
	uint8_t  state;
	uint8_t  seen;          // the press count acted on
	uint32_t tPress;        // first tick counted towards the press
	uint32_t tEdge;         // tick the press was confirmed
} PttArb_t;

typedef struct {
	bool     pressed;       // debounced PTT
	uint8_t  pressCount;    // press edges so far
	uint16_t lockAtPress;   // lock remaining at the latest press edge
	uint32_t tPress;
	uint32_t tPressEdge;
	uint16_t lockNow;
	uint32_t now;
	uint8_t  bar;           // TXR_* other than LOCK if TX is not allowed now
} PttArbIn_t;

typedef struct {
	uint8_t  action;        // ARB_*
	uint8_t  reason;        // ARB_REFUSE: TXR_*
	uint16_t detail;        // ARB_REFUSE, LOCK: lock remaining at the press
	uint32_t tPress;
	uint16_t lockDelay;     // ARB_KEY: ms from the press edge to the key
	bool     late;          // ARB_KEY: keyed after waiting for the lock
} PttArbOut_t;

void PTTARB_Init(PttArb_t *a, uint8_t pressCount);
void PTTARB_Step(PttArb_t *a, const PttArbIn_t *in, PttArbOut_t *out);
void PTTARB_Latch(PttArb_t *a);

#endif
