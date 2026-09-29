/* Copyright 2023 Dual Tachyon
 * https://github.com/DualTachyon
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

#ifndef APP_UART_H
#define APP_UART_H

#include <stdbool.h>
#include <stdint.h>

// A frame still incomplete this long after the last byte arrived is taken
// as truncated and dropped (protocol v2, 2.3).
#define UART_GAP_MS    5u
#define UART_BODY_MAX  150u     // largest body the radio sends

extern uint32_t gRxMs;          // when the command being handled was seen complete
extern uint16_t gRxUs;

bool     UART_IsCommandAvailable(void);
void     UART_HandleCommand(void);
void     UART_Poll(void);

// Outgoing frames: build the body in UART_FrameBody(), then send it.
uint8_t *UART_FrameBody(void);
void     UART_SendFrameBody(uint16_t id, uint16_t n);
bool     UART_SendStampedHeader(uint16_t n, uint32_t *ms, uint16_t *us);
void     UART_SendStampedBody(uint16_t id, uint16_t n);

#endif

