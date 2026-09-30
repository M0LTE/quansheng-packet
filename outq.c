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

#include "outq.h"

#ifdef HOST_TEST
	#define IRQ_OFF()
	#define IRQ_ON()
#else
	#include "ARMCM0.h"
	#define IRQ_OFF() __disable_irq()
	#define IRQ_ON()  __enable_irq()
#endif

static uint8_t           gQ[OUTQ_SIZE];
static volatile uint16_t gHead;   // written by the producer only
static volatile uint16_t gTail;   // written by the consumers only (never concurrently)
static bool              gIrqOn;  // the UART TX interrupt is enabled

uint16_t OUTQ_Used(void) { return (uint16_t)(gHead - gTail); }
uint16_t OUTQ_Free(void) { return OUTQ_SIZE - OUTQ_Used(); }

bool OUTQ_Put(const void *p, uint16_t n)
{
	const uint8_t *b = p;
	if (n > OUTQ_Free())
		return false;
	uint16_t h = gHead;
	for (uint16_t i = 0; i < n; i++)
		gQ[(h++) & (OUTQ_SIZE - 1)] = b[i];
	gHead = h;                 // publish after the data
	return true;
}

void OUTQ_PutWait(const void *p, uint16_t n)
{
	const uint8_t *b = p;
	while (n) {
		uint16_t chunk = OUTQ_Free();
		if (chunk > n)
			chunk = n;
		OUTQ_Put(b, chunk);
		b += chunk;
		n -= chunk;
		OUTQ_Kick();
	}
}

// Fill the transmit FIFO from the queue, and keep the UART TX interrupt
// enabled exactly while there is more to send. Called from the UART TX
// interrupt, the SysTick handler (both at the lowest priority, so never at
// the same time) and the main loop with interrupts off.
void OUTQ_Drain(void)
{
	uint16_t t = gTail;
	while (t != gHead && UART_TxReady())
		UART_TxPut(gQ[(t++) & (OUTQ_SIZE - 1)]);
	gTail = t;

	const bool more = t != gHead;
	if (more != gIrqOn) {
		gIrqOn = more;
		UART_TxIrq(more);
	}
}

// The UART TX interrupt (the FIFO ran low). An interrupt that finds no room
// in the FIFO switches itself off, so a misbehaving flag can never starve
// the main loop; the SysTick drain switches it on again next ms.
void OUTQ_Isr(void)
{
	const uint16_t before = gTail;
	OUTQ_Drain();
	if (gTail == before && gIrqOn) {
		gIrqOn = false;
		UART_TxIrq(false);
	}
}

void OUTQ_Kick(void)
{
	if (gHead == gTail)
		return;
	IRQ_OFF();                 // at most a FIFO's worth of writes, a few us
	OUTQ_Drain();
	IRQ_ON();
}

bool OUTQ_Idle(void)
{
	return gHead == gTail && UART_TxEmpty();
}

void OUTQ_Reset(void)
{
	gHead = gTail = 0;
	gIrqOn = false;
}
