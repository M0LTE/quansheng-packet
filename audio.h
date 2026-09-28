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

#ifndef AUDIO_H
#define AUDIO_H

#include "bsp/dp32g030/gpio.h"
#include "driver/gpio.h"

// The speaker amplifier (and the K1 audio out). There are no beeps or voice
// prompts in this firmware: nothing but received audio reaches the TNC.
static inline void AUDIO_AudioPathOn(void) {
	GPIO_SetBit(&GPIOC->DATA, GPIOC_PIN_AUDIO_PATH);
}

static inline void AUDIO_AudioPathOff(void) {
	GPIO_ClearBit(&GPIOC->DATA, GPIOC_PIN_AUDIO_PATH);
}

#endif
