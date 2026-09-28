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

// Forced in front of app/uart.c for the host build: the DMA channel becomes
// a host variable and the CMSIS core header is replaced, so the real UART
// command code runs on the host.

#ifndef UART_SHIM_H
#define UART_SHIM_H

#include <stdint.h>
#include "bsp/dp32g030/dma.h"

#undef DMA_CH0
extern volatile DMA_Channel_t host_dma_ch0;
#define DMA_CH0 (&host_dma_ch0)

#define ARMCM0_H
void NVIC_SystemReset(void);

#endif
