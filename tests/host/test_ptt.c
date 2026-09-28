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

// Host tests of ptt.c, built with PTT_HOST_TEST: the real PTT_Tick runs
// against a simulated PTT/UART line and a simulated 48 MHz SysTick.
// Covers press and release timing, glitch and spike rejection, the serial
// lock, UART streams (zero and random bytes, every start phase) never
// keying, and UART traffic right after a release.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ptt.h"

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

// ------------------------------------------------ simulated line and clock

#define BIT_US (1e6 / 38400.0)

static double   t_us;           // simulated time
static int      mode;           // 0 = held low, 1 = high, 2 = UART stream, 3 = low with a high spike at each tick
static double   spike_start;
static uint8_t  stream[4096];
static unsigned stream_len;
static double   stream_start;
static bool     lock;
static uint8_t  press_ms = PTT_PRESS_DEFAULT_MS, release_ms = PTT_RELEASE_DEFAULT_MS;

static bool level_low_at(double t)
{
	switch (mode) {
		case 0: return true;
		case 1: return false;
		case 3: return !(t >= spike_start && t < spike_start + 5.0);   // 5 us high spike
		default: break;
	}
	const double rel = t - stream_start;
	if (rel < 0) return false;
	const unsigned idx = (unsigned)(rel / (10 * BIT_US));
	if (idx >= stream_len) return false;                  // idle high after the stream
	const unsigned bit = (unsigned)((rel - idx * 10 * BIT_US) / BIT_US);
	if (bit == 0) return true;                             // start bit
	if (bit == 9) return false;                            // stop bit
	return ((stream[idx] >> (bit - 1)) & 1) == 0;          // data, LSB first
}

// hardware hooks for ptt.c: each pin read takes 0.25 us
bool     PTT_HwLineLow(void)    { const bool l = level_low_at(t_us); t_us += 0.25; return l; }
uint32_t PTT_HwTicks(void)      { return 47999u - (uint32_t)((uint64_t)(t_us * 48.0) % 48000u); }
uint32_t PTT_HwPeriod(void)     { return 48000u; }
bool     PTT_HwSerialLock(void) { return lock; }
uint8_t  PTT_HwPressMs(void)    { return press_ms; }
uint8_t  PTT_HwReleaseMs(void)  { return release_ms; }

// one SysTick interrupt; ticks are 1 ms apart
static bool tick(void)
{
	const double start = t_us;
	spike_start = start;
	PTT_Tick();
	t_us = start + 1000.0;
	return PTT_IsPressed();
}

static void reset(void) { PTT_HostReset(); t_us = 0; lock = false; }

// ------------------------------------------------------------------ tests

static void test_press_release_timing(void)
{
	reset(); mode = 1;
	for (int i = 0; i < 10; i++) CHECK(!tick());

	mode = 0;
	for (unsigned i = 1; i < PTT_PRESS_DEFAULT_MS; i++) CHECK(!tick());
	CHECK(tick());                                          // 5th tick low: pressed

	mode = 1;
	for (unsigned i = 1; i < PTT_RELEASE_DEFAULT_MS; i++) CHECK(tick());
	CHECK(!tick());                                         // 5th tick high: released

	press_ms = 1; release_ms = 2;
	mode = 0; CHECK(tick());
	mode = 1; CHECK(tick()); CHECK(!tick());
	press_ms = PTT_PRESS_DEFAULT_MS; release_ms = PTT_RELEASE_DEFAULT_MS;
}

static void test_glitches_and_spikes(void)
{
	reset();
	// low 4, high 1, low 4: never 5 in a row
	for (int r = 0; r < 20; r++) {
		mode = 0; for (int i = 0; i < 4; i++) CHECK(!tick());
		mode = 1; CHECK(!tick());
	}
	// keyed: a 5 us high spike at every tick never releases
	mode = 0; for (unsigned i = 0; i < PTT_PRESS_DEFAULT_MS; i++) tick();
	CHECK(PTT_IsPressed());
	mode = 3;
	for (int i = 0; i < 200; i++) CHECK(tick());
	// keyed: short real highs (4 ticks) do not release either
	for (int r = 0; r < 20; r++) {
		mode = 1; for (int i = 0; i < 4; i++) CHECK(tick());
		mode = 0; CHECK(tick());
	}
}

static void test_serial_lock(void)
{
	reset(); mode = 0;
	for (unsigned i = 0; i < PTT_PRESS_DEFAULT_MS; i++) tick();
	CHECK(PTT_IsPressed());
	lock = true;
	CHECK(!tick());                                         // the lock releases at once
	for (int i = 0; i < 50; i++) CHECK(!tick());            // and blocks pressing
	lock = false;
	for (unsigned i = 1; i < PTT_PRESS_DEFAULT_MS; i++) CHECK(!tick());
	CHECK(tick());                                          // held after the lock: a press
}

static int run_stream(uint8_t pms)
{
	int presses = 0;
	press_ms = pms;
	for (int phase = 0; phase < 261; phase++) {
		reset(); mode = 2; stream_start = phase;
		const double end = stream_len * 10 * BIT_US + 2000;
		while (t_us < end)
			if (tick()) presses++;
	}
	press_ms = PTT_PRESS_DEFAULT_MS;
	return presses;
}

static void test_uart_never_keys(void)
{
	stream_len = 2000; memset(stream, 0x00, stream_len);   // worst case: low 9 bits of 10
	CHECK(run_stream(1) == 0);
	CHECK(run_stream(5) == 0);

	srand(1);
	for (unsigned i = 0; i < stream_len; i++) stream[i] = rand() & 0xFF;
	stream[0] = 0xAB; stream[1] = 0xCD;
	CHECK(run_stream(1) == 0);

	CHECK(PTT_WINDOW_US > 10 * BIT_US);
}

static void test_uart_after_release(void)
{
	// keyed, released, and the host starts sending right away
	srand(2);
	stream_len = 400;
	for (unsigned i = 0; i < stream_len; i++) stream[i] = rand() & 0xFF;
	int worst = 0;
	for (int phase = 0; phase < 261; phase += 7) {
		reset(); mode = 0;
		for (unsigned i = 0; i < PTT_PRESS_DEFAULT_MS; i++) tick();
		CHECK(PTT_IsPressed());
		mode = 2; stream_start = t_us + phase;
		int n = 0;
		while (tick() && n < 1000) n++;
		if (n + 1 > worst) worst = n + 1;
	}
	printf("  UART right after release: released within %d ms (release debounce %u ms)\n", worst, PTT_RELEASE_DEFAULT_MS);
	CHECK(worst <= (int)PTT_RELEASE_DEFAULT_MS + 5);

	// zero bytes after release: low most of the time, but never a whole window
	stream_len = 400; memset(stream, 0x00, stream_len);
	reset(); mode = 0;
	for (unsigned i = 0; i < PTT_PRESS_DEFAULT_MS; i++) tick();
	mode = 2; stream_start = t_us;
	int n = 0;
	while (tick() && n < 1000) n++;
	printf("  zero bytes right after release: released within %d ms\n", n + 1);
	CHECK(n + 1 <= (int)PTT_RELEASE_DEFAULT_MS + 5);
}

int main(void)
{
	test_press_release_timing();
	test_glitches_and_spikes();
	test_serial_lock();
	test_uart_never_keys();
	test_uart_after_release();
	if (failures) {
		printf("%d check(s) failed\n", failures);
		return 1;
	}
	printf("all PTT host tests passed\n");
	return 0;
}
