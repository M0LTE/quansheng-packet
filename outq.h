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

// UART output queue (protocol v2, 9.1). Every byte the radio sends goes
// through this ring, legacy replies included. The main loop is the only
// producer. Three things drain it into the UART's transmit FIFO: the UART
// TX interrupt, enabled only while the queue holds more than the FIFO
// takes (it keeps the line busy at wire speed however shallow the FIFO
// is); the main loop on every pass and whenever it queues a frame, so the
// first byte of a frame leaves at once; and the 1 ms SysTick handler, the
// fallback if the interrupt ever switches itself off. The bench measured
// only 1 to 2.3 bytes/ms with the tick alone (line rate 3.9): the FIFO
// takes about 2 bytes, not 8.
//
// Nothing here blocks except OUTQ_PutWait, used for legacy replies and
// v2 replies, which waits for room (the SysTick drain empties the queue
// at 3.9 bytes/ms) rather than drop a reply.

#ifndef OUTQ_H
#define OUTQ_H

#include <stdbool.h>
#include <stdint.h>

#define OUTQ_SIZE 512u          // power of two

uint16_t OUTQ_Used(void);
uint16_t OUTQ_Free(void);
bool     OUTQ_Put(const void *p, uint16_t n);       // false (nothing queued) if no room
void     OUTQ_PutWait(const void *p, uint16_t n);   // waits for room
void     OUTQ_Drain(void);                          // SysTick context
void     OUTQ_Isr(void);                            // UART TX interrupt
void     OUTQ_Kick(void);                           // main loop: drain now, interrupts off briefly
bool     OUTQ_Idle(void);                           // queue empty and the UART has nothing left to send
void     OUTQ_Reset(void);

// hardware hooks (driver/uart.c, or the host tests)
bool     UART_TxReady(void);                        // transmit FIFO has room for a byte
void     UART_TxPut(uint8_t b);
bool     UART_TxEmpty(void);                        // transmit FIFO empty
void     UART_TxIrq(bool on);                       // UART TX interrupt on or off

#endif
