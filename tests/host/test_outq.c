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

// Host test of outq.c against a simulated UART: a transmit FIFO of a given
// depth, a shift register sending one byte per 256 us (39 kbaud), a TX
// interrupt that fires while the FIFO is empty and enabled, the 1 ms
// SysTick drain (which holds the CPU for up to 280 us for the PTT window),
// and a main loop that kicks the queue on every pass but stalls for 8 ms at
// a time (an EEPROM write). Bounds the drain rate: small replies leave at
// once, large ones at wire speed, whatever the FIFO depth, and a broken
// interrupt falls back to the tick without starving the main loop.

#include <stdio.h>
#include <string.h>

#include "outq.h"

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

#define BYTE_US 256

static int      depth;          // FIFO depth
static int      fifo_n;
static long     shift_end;      // the shift register is busy until then
static long     now;
static bool     irq_on;
static bool     irq_works;      // the interrupt fires at all
static bool     irq_stuck;      // it fires whenever enabled, FIFO full or not
static long     sent, last_sent_at;
static long     isr_calls;

bool UART_TxReady(void) { return fifo_n < depth; }
void UART_TxPut(uint8_t b) { (void)b; fifo_n++; }
bool UART_TxEmpty(void) { return fifo_n == 0 && now >= shift_end; }
void UART_TxIrq(bool on) { irq_on = on; }

static void line(void)
{
	if (now >= shift_end && fifo_n > 0) {
		fifo_n--;
		shift_end = now + BYTE_US;
		sent++;
		last_sent_at = shift_end;         // the byte has left at the end of its stop bit
	}
}

// run for us microseconds: main loop pass every pass_us unless stalled
static void run(long us, long pass_us, bool stalls)
{
	const long end = now + us;
	long cpu_free = now;                  // SysTick holds the CPU until then
	while (now < end) {
		line();
		if (now % 1000 == 0) {            // SysTick: drain, then the PTT window
			OUTQ_Drain();
			cpu_free = now + 280;
		}
		if (now >= cpu_free && irq_on && irq_works && (fifo_n == 0 || irq_stuck)) {
			OUTQ_Isr();
			isr_calls++;
			cpu_free = now + 5;           // the handler's own time
		}
		const bool stalled = stalls && (now % 20000) < 8000;
		if (!stalled && now >= cpu_free && now % pass_us == 0)
			OUTQ_Kick();
		now++;
	}
}

static void reset(int d)
{
	OUTQ_Reset();
	depth = d; fifo_n = 0; shift_end = 0; now = 1; irq_on = false;
	irq_works = true; irq_stuck = false; sent = 0; last_sent_at = 0; isr_calls = 0;
}

static long send(int n, long pass_us, bool stalls)
{
	uint8_t buf[256] = { 0 };
	const long t0 = now;
	const long s0 = sent;
	OUTQ_PutWait(buf, n);
	while (sent - s0 < n)
		run(100, pass_us, stalls);
	return last_sent_at - t0;
}

int main(void)
{
	const int depths[] = { 1, 2, 8 };
	for (unsigned k = 0; k < 3; k++) {
		const int d = depths[k];

		// a 12-byte reply: the last byte out within a few us of wire time
		reset(d);
		run(3000, 400, false);
		long t = send(12, 400, false);
		printf("  FIFO %d: 12-byte reply out in %.2f ms (wire %.2f)\n", d, t / 1000.0, 12 * BYTE_US / 1000.0);
		CHECK(t <= 12 * BYTE_US + 300);

		// a 144-byte 0x051C reply with the main loop stalling 8 ms in 20:
		// wire speed (about 37 ms), carried by the interrupt
		reset(d);
		run(2500, 400, true);
		t = send(144, 400, true);
		printf("  FIFO %d: 144-byte reply out in %.2f ms (wire %.2f)\n", d, t / 1000.0, 144 * BYTE_US / 1000.0);
		CHECK(t <= 144 * BYTE_US + 1000);
		CHECK(!irq_on);                   // off again once the queue is empty

		// the interrupt never fires: the main loop and the tick still keep up
		reset(d);
		irq_works = false;
		t = send(144, 200, false);
		CHECK(t <= 144 * BYTE_US + 2000);
		// and with the main loop stalled throughout, at least a FIFO per ms
		reset(d);
		irq_works = false;
		{
			uint8_t buf[64] = { 0 };
			OUTQ_PutWait(buf, 1);          // the kick in PutWait sends at once
			OUTQ_Put(buf, 63);             // queued without a kick
			const long t0 = now;
			while (sent < 64)
				run(100, 1000000, false);   // no main loop at all
			const double rate = 64.0 / ((last_sent_at - t0) / 1000.0);
			CHECK(rate >= (d < 4 ? d : 3.8) * 0.9);
		}

		// a stuck interrupt flag: bounded calls, the line still busy
		reset(d);
		irq_stuck = true;
		run(1000, 400, false);
		t = send(144, 400, false);
		printf("  FIFO %d: stuck flag: 144 bytes in %.2f ms, %ld interrupts\n", d, t / 1000.0, isr_calls);
		CHECK(isr_calls <= 144 + 2 * (t / 1000 + 2));
		CHECK(t <= 144 * BYTE_US + 3000);
	}

	if (failures) {
		printf("%d check(s) failed\n", failures);
		return 1;
	}
	printf("all output queue host tests passed\n");
	return 0;
}
