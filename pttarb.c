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

#include "misc.h"
#include "pttarb.h"

void PTTARB_Init(PttArb_t *a, uint8_t pressCount)
{
	a->state = ARB_IDLE;
	a->seen  = pressCount;
}

void PTTARB_Latch(PttArb_t *a)
{
	if (a->state == ARB_PENDING || a->state == ARB_KEYED)
		a->state = ARB_LATCHED;
}

static void Refuse(PttArb_t *a, PttArbOut_t *out, uint8_t reason, uint16_t detail)
{
	a->state    = ARB_LATCHED;
	out->action = ARB_REFUSE;
	out->reason = reason;
	out->detail = detail;
}

static uint16_t Late(uint32_t ms)
{
	return (ms > 0xFFFF) ? 0xFFFF : (uint16_t)ms;
}

static void Key(PttArb_t *a, const PttArbIn_t *in, PttArbOut_t *out, bool late)
{
	a->state       = ARB_KEYED;
	out->action    = ARB_KEY;
	out->late      = late;
	out->lockDelay = late ? (uint16_t)(in->now - a->tEdge) : 0;
}

void PTTARB_Step(PttArb_t *a, const PttArbIn_t *in, PttArbOut_t *out)
{
	const bool newPress = in->pressCount != a->seen;

	out->action    = ARB_NONE;
	out->reason    = TXR_NONE;
	out->detail    = 0;
	out->lockDelay = 0;
	out->late      = false;
	out->tPress    = a->tPress;

	// a release, seen or implied by a newer press, ends whatever this press was
	if (a->state != ARB_IDLE && (!in->pressed || newPress)) {
		const bool keyed = a->state == ARB_KEYED;
		a->state = ARB_IDLE;
		if (keyed) {
			out->action = ARB_UNKEY;
			return;             // a newer press is taken on the next step
		}
	}

	if (a->state == ARB_IDLE) {
		if (!newPress)
			return;
		a->seen = in->pressCount;
		if (!in->pressed)
			return;             // pressed and released between two steps
		a->tPress   = in->tPress;
		a->tEdge    = in->tPressEdge;
		out->tPress = in->tPress;

		// what the lock would be now had no frame come since the edge
		const uint32_t elapsed  = in->now - in->tPressEdge;
		const uint32_t expected = (in->lockAtPress > elapsed) ? in->lockAtPress - elapsed : 0;

		if (in->lockAtPress > LATE_KEY_MAX_MS)
			Refuse(a, out, TXR_LOCK, in->lockAtPress);
		else if (in->lockNow > expected)
			Refuse(a, out, TXR_LOCK, in->lockNow);       // a frame after the press
		else if (in->bar != TXR_NONE)
			Refuse(a, out, in->bar, 0);
		else if (elapsed > LATE_KEY_MAX_MS)
			Refuse(a, out, TXR_LATE, Late(elapsed));
		else if (in->lockNow > 0)
			a->state = ARB_PENDING;
		else
			Key(a, in, out, in->lockAtPress > 0);
		return;
	}

	if (a->state == ARB_PENDING && in->lockNow == 0) {
		const uint32_t elapsed = in->now - a->tEdge;
		if (in->bar != TXR_NONE)
			Refuse(a, out, in->bar, 0);
		else if (elapsed > LATE_KEY_MAX_MS)
			Refuse(a, out, TXR_LATE, Late(elapsed));
		else
			Key(a, in, out, true);
	}
}
