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
static volatile uint16_t gTail;   // written by the consumer only

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

void OUTQ_Drain(void)
{
	uint16_t t = gTail;
	while (t != gHead && UART_TxReady())
		UART_TxPut(gQ[(t++) & (OUTQ_SIZE - 1)]);
	gTail = t;
}

void OUTQ_Kick(void)
{
	IRQ_OFF();                 // at most 8 FIFO writes, a few us
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
}
