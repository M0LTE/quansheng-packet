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

// Host tests of the PTT debouncer (ptt.c, built with PTT_HOST_TEST): press
// and release timing, glitch rejection, the serial lock, and that UART
// traffic at 38400 baud, even all zero bytes, never looks like a press.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ptt.h"

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

// ------------------------------------------------ simulated line and clock

#define BIT_US (1e6 / 38400.0)

static double   t_us;          // simulated time
static double   window_start;
static int      mode;          // 0 = held low, 1 = high, 2 = UART stream
static uint8_t  stream[4096];
static unsigned stream_len;
static double   stream_start;

static bool level_low_at(double t)
{
	if (mode == 0) return true;
	if (mode == 1) return false;
	const double rel = t - stream_start;
	if (rel < 0) return false;
	const unsigned idx = (unsigned)(rel / (10 * BIT_US));
	if (idx >= stream_len) return false;                  // idle high after the frame
	const unsigned bit = (unsigned)((rel - idx * 10 * BIT_US) / BIT_US);
	if (bit == 0) return true;                             // start bit
	if (bit == 9) return false;                            // stop bit
	return ((stream[idx] >> (bit - 1)) & 1) == 0;          // data, LSB first
}

// each read of the pin takes a little time, as on the MCU
static bool lineLow(void) { const bool l = level_low_at(t_us); t_us += 0.25; return l; }
static uint32_t elapsedUs(void) { return (uint32_t)(t_us - window_start); }

// one 1 ms SysTick tick, as PTT_Tick does it
static bool tick(PttDebounce_t *d, bool lock, uint8_t pressMs, uint8_t releaseMs)
{
	const double tick_start = t_us;
	const bool low = lineLow();
	bool whole = false;
	if (low && !d->pressed && !lock) {
		window_start = t_us;
		whole = PTT_LowForWindow(lineLow, elapsedUs);
	}
	const bool p = PTT_Debounce(d, whole, low, lock, pressMs, releaseMs);
	t_us = tick_start + 1000.0;
	return p;
}

// ------------------------------------------------------------------ tests

static void test_press_release_timing(void)
{
	PttDebounce_t d = {0};
	t_us = 0; mode = 1;
	for (int i = 0; i < 10; i++) CHECK(!tick(&d, false, 5, 3));

	mode = 0;
	for (int i = 1; i <= 4; i++) CHECK(!tick(&d, false, 5, 3));
	CHECK(tick(&d, false, 5, 3));                           // 5th tick low: pressed

	mode = 1;
	CHECK(tick(&d, false, 5, 3));
	CHECK(tick(&d, false, 5, 3));
	CHECK(!tick(&d, false, 5, 3));                          // 3rd tick high: released
}

static void test_glitches(void)
{
	PttDebounce_t d = {0};
	t_us = 0;
	// low 4, high 1, low 4: never 5 in a row
	for (int r = 0; r < 20; r++) {
		mode = 0; for (int i = 0; i < 4; i++) CHECK(!tick(&d, false, 5, 3));
		mode = 1; CHECK(!tick(&d, false, 5, 3));
	}
	// pressed, then short high glitches of 2 ticks do not release
	mode = 0; for (int i = 0; i < 5; i++) tick(&d, false, 5, 3);
	CHECK(d.pressed);
	for (int r = 0; r < 20; r++) {
		mode = 1; CHECK(tick(&d, false, 5, 3)); CHECK(tick(&d, false, 5, 3));
		mode = 0; CHECK(tick(&d, false, 5, 3));
	}
}

static void test_serial_lock(void)
{
	PttDebounce_t d = {0};
	t_us = 0; mode = 0;
	for (int i = 0; i < 5; i++) tick(&d, false, 5, 3);
	CHECK(d.pressed);
	CHECK(!tick(&d, true, 5, 3));                           // lock releases at once
	for (int i = 0; i < 50; i++) CHECK(!tick(&d, true, 5, 3)); // and blocks pressing
	for (int i = 1; i <= 4; i++) CHECK(!tick(&d, false, 5, 3));
	CHECK(tick(&d, false, 5, 3));                           // held after the lock: a press
}

static void run_stream(const char *name, int pressMs)
{
	// sweep the stream start over a whole character at 1 us steps
	int presses = 0, window_passes = 0;
	for (int phase = 0; phase < 261; phase++) {
		PttDebounce_t d = {0};
		mode = 2; stream_start = phase; t_us = 0;
		const double end = stream_len * 10 * BIT_US + 2000;
		while (t_us < end) {
			const double s = t_us;
			if (level_low_at(s)) {
				window_start = s; t_us = s;
				if (PTT_LowForWindow(lineLow, elapsedUs)) window_passes++;
				t_us = s;
			}
			if (tick(&d, false, pressMs, 3)) presses++;
		}
	}
	printf("  %s: %u bytes, press %d ms: %d presses, %d full low windows\n",
		name, stream_len, pressMs, presses, window_passes);
	CHECK(presses == 0);
	CHECK(window_passes == 0);
}

static void test_uart_never_keys(void)
{
	// the worst case: a long run of 0x00 (low for 9 of every 10 bits)
	stream_len = 2000; memset(stream, 0x00, stream_len);
	run_stream("zero bytes", 1);
	run_stream("zero bytes", 5);

	// a frame header and random payload
	srand(1);
	stream_len = 2000;
	for (unsigned i = 0; i < stream_len; i++) stream[i] = rand() & 0xFF;
	stream[0] = 0xAB; stream[1] = 0xCD;
	run_stream("random bytes", 1);

	// a real press (break, held low) passes the window
	mode = 0; t_us = 0; window_start = 0;
	CHECK(PTT_LowForWindow(lineLow, elapsedUs));
	CHECK(PTT_WINDOW_US > 10 * BIT_US);
}

int main(void)
{
	test_press_release_timing();
	test_glitches();
	test_serial_lock();
	test_uart_never_keys();
	if (failures) {
		printf("%d check(s) failed\n", failures);
		return 1;
	}
	printf("all PTT host tests passed\n");
	return 0;
}
