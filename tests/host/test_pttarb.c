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

// Host tests of pttarb.c, the serial PTT lock rules (protocol v2, 5.3):
// key, late key (at most LATE_KEY_MAX_MS), refusal and latch, release,
// the latch after a timeout or a serial frame, and a main loop that runs
// late. A 1 ms simulated clock drives the lock as the SysTick handler does.

#include <stdio.h>
#include <string.h>

#include "misc.h"
#include "pttarb.h"

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

static PttArb_t   arb;
static PttArbIn_t in;
static PttArbOut_t out;

static void reset(void)
{
	memset(&in, 0, sizeof(in));
	in.now = 1000;
	PTTARB_Init(&arb, 0);
}

// the 1 ms tick: the lock counts down
static void tick(void)
{
	in.now++;
	if (in.lockNow) in.lockNow--;
}

// a press edge seen by the tick at the current time
static void press(void)
{
	in.pressed     = true;
	in.pressCount++;
	in.lockAtPress = in.lockNow;
	in.tPressEdge  = in.now;
	in.tPress      = in.now - 4;          // 5 ms debounce
}

static void release(void) { in.pressed = false; }

static uint8_t step(void)
{
	PTTARB_Step(&arb, &in, &out);
	return out.action;
}

static void test_key_and_release(void)
{
	reset();
	CHECK(step() == ARB_NONE);
	press();
	CHECK(step() == ARB_KEY && !out.late && out.lockDelay == 0 && out.tPress == in.tPress);
	CHECK(arb.state == ARB_KEYED);
	for (int i = 0; i < 100; i++) { tick(); CHECK(step() == ARB_NONE); }
	release();
	CHECK(step() == ARB_UNKEY);
	CHECK(step() == ARB_NONE && arb.state == ARB_IDLE);
}

static void test_late_key(void)
{
	// every lock from 1 to 30 ms at the press keys exactly when it runs out
	for (uint16_t lock = 1; lock <= LATE_KEY_MAX_MS; lock++) {
		reset();
		in.lockNow = lock;
		press();
		CHECK(step() == ARB_NONE && arb.state == ARB_PENDING);
		uint16_t waited = 0;
		while (in.lockNow) {
			tick();
			waited++;
			const uint8_t a = step();
			if (in.lockNow) CHECK(a == ARB_NONE);
			else CHECK(a == ARB_KEY && out.late && out.lockDelay == lock);
		}
		CHECK(waited == lock);
	}

	// released before the lock ran out: no key at all
	reset();
	in.lockNow = 20;
	press();
	CHECK(step() == ARB_NONE);
	for (int i = 0; i < 10; i++) { tick(); step(); }
	release();
	CHECK(step() == ARB_NONE && arb.state == ARB_IDLE);
	for (int i = 0; i < 30; i++) { tick(); CHECK(step() == ARB_NONE); }
}

static void test_refused(void)
{
	// lock above 30 ms at the press: refused, latched, until released
	reset();
	in.lockNow = LATE_KEY_MAX_MS + 1;
	press();
	CHECK(step() == ARB_REFUSE && out.reason == TXR_LOCK && out.detail == LATE_KEY_MAX_MS + 1);
	CHECK(arb.state == ARB_LATCHED);
	for (int i = 0; i < 200; i++) { tick(); CHECK(step() == ARB_NONE); }   // held after the lock: nothing
	release();
	CHECK(step() == ARB_NONE && arb.state == ARB_IDLE);
	press();                                                                  // pressed again: keys
	CHECK(step() == ARB_KEY && !out.late);

	// the v1 bench case: a 1.5 s lock never keys late any more
	reset();
	in.lockNow = 1500;
	press();
	CHECK(step() == ARB_REFUSE && out.detail == 1500);
	for (int i = 0; i < 2000; i++) { tick(); CHECK(step() == ARB_NONE); }

	// TX not allowed: refused with the reason, latched
	reset();
	in.bar = TXR_TX_BAND;
	press();
	CHECK(step() == ARB_REFUSE && out.reason == TXR_TX_BAND && out.detail == 0);
	in.bar = TXR_NONE;
	for (int i = 0; i < 50; i++) { tick(); CHECK(step() == ARB_NONE); }
	release(); step();
	press();
	CHECK(step() == ARB_KEY);

	// a bar that appears while a late key waits: refused when the lock ends
	reset();
	in.lockNow = 10;
	press();
	CHECK(step() == ARB_NONE);
	in.bar = TXR_BATTERY_EMPTY;
	while (in.lockNow) { tick(); step(); }
	CHECK(out.action == ARB_REFUSE && out.reason == TXR_BATTERY_EMPTY);
}

static void test_latch(void)
{
	// TX timeout or a serial frame while keyed: latched until release
	reset();
	press();
	CHECK(step() == ARB_KEY);
	PTTARB_Latch(&arb);
	CHECK(arb.state == ARB_LATCHED);
	for (int i = 0; i < 100; i++) { tick(); CHECK(step() == ARB_NONE); }
	release();
	CHECK(step() == ARB_NONE && arb.state == ARB_IDLE);    // no second unkey

	// a frame during a pending late key cancels it
	reset();
	in.lockNow = 15;
	press();
	step();
	in.lockNow = 20;                                       // the frame re-arms the lock
	PTTARB_Latch(&arb);
	for (int i = 0; i < 100; i++) { tick(); CHECK(step() == ARB_NONE); }

	// latch when idle does nothing
	reset();
	PTTARB_Latch(&arb);
	CHECK(arb.state == ARB_IDLE);
	press();
	CHECK(step() == ARB_KEY);
}

static void test_late_main_loop(void)
{
	// the decision uses the lock at the edge, not when the loop looks
	reset();
	in.lockNow = 40;
	press();
	for (int i = 0; i < 45; i++) tick();                   // loop busy for 45 ms
	CHECK(in.lockNow == 0);
	CHECK(step() == ARB_REFUSE && out.reason == TXR_LOCK && out.detail == 40);

	reset();
	in.lockNow = 25;
	press();
	for (int i = 0; i < 40; i++) tick();                   // loop late, lock already out
	CHECK(step() == ARB_KEY && out.late && out.lockDelay == 40);

	// press, release and press again between two steps while keyed:
	// unkey first, then the new press
	reset();
	press();
	CHECK(step() == ARB_KEY);
	release();
	press();
	CHECK(step() == ARB_UNKEY);
	CHECK(step() == ARB_KEY);

	// a press that came and went between two steps is ignored
	reset();
	press();
	release();
	CHECK(step() == ARB_NONE && arb.state == ARB_IDLE);
	CHECK(step() == ARB_NONE);
}

int main(void)
{
	test_key_and_release();
	test_late_key();
	test_refused();
	test_latch();
	test_late_main_loop();
	if (failures) {
		printf("%d check(s) failed\n", failures);
		return 1;
	}
	printf("all PTT lock rule host tests passed\n");
	return 0;
}
