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

#ifndef DRIVER_BK4819_h
#define DRIVER_BK4819_h

#include <stdbool.h>
#include <stdint.h>

#include "driver/bk4819-regs.h"

enum BK4819_AF_Type_t
{
	BK4819_AF_MUTE      =  0u,  //
	BK4819_AF_FM        =  1u,  // FM
	BK4819_AF_ALAM      =  2u,  //
	BK4819_AF_BEEP      =  3u,  //
	BK4819_AF_BASEBAND1 =  4u,  // RAW
	BK4819_AF_BASEBAND2 =  5u,  // USB
	BK4819_AF_CTCO      =  6u,  // strange LF audio .. maybe the CTCSS LF line ?
	BK4819_AF_AM        =  7u,  // AM
	BK4819_AF_FSKO      =  8u,  // nothing
	BK4819_AF_UNKNOWN3  =  9u,  // BYP
	BK4819_AF_UNKNOWN4  = 10u,  // nothing at all
	BK4819_AF_UNKNOWN5  = 11u,  // distorted
	BK4819_AF_UNKNOWN6  = 12u,  // distorted
	BK4819_AF_UNKNOWN7  = 13u,  // interesting
	BK4819_AF_UNKNOWN8  = 14u,  // interesting
	BK4819_AF_UNKNOWN9  = 15u   // not a lot
};

typedef enum BK4819_AF_Type_t BK4819_AF_Type_t;

void     BK4819_Init(void);
uint16_t BK4819_ReadRegister(BK4819_REGISTER_t Register);
void     BK4819_WriteRegister(BK4819_REGISTER_t Register, uint16_t Data);
void     BK4819_SetRegValue(RegisterSpec s, uint16_t v);
void     BK4819_WriteU8(uint8_t Data);
void     BK4819_WriteU16(uint16_t Data);

void     BK4819_SetAGC(bool enable);
void     BK4819_InitAGC(void);

void     BK4819_ToggleGpioOut(BK4819_GPIO_PIN_t Pin, bool bSet);

void     BK4819_SetFilterBandwidth(const bool narrow);
void     BK4819_SetupPowerAmplifier(const uint8_t bias, const uint32_t frequency);
void     BK4819_SetFrequency(uint32_t Frequency);
void     BK4819_SetupSquelch(
			uint8_t SquelchOpenRSSIThresh,
			uint8_t SquelchCloseRSSIThresh,
			uint8_t SquelchOpenNoiseThresh,
			uint8_t SquelchCloseNoiseThresh,
			uint8_t SquelchCloseGlitchThresh,
			uint8_t SquelchOpenGlitchThresh);

void     BK4819_SetAF(BK4819_AF_Type_t AF);
void     BK4819_RX_TurnOn(void);
void     BK4819_PickRXFilterPathBasedOnFrequency(uint32_t Frequency);

void     BK4819_DisableDTMF(void);
void     BK4819_ExitTxMute(void);
void     BK4819_Sleep(void);
void     BK4819_PrepareDigitalTransmit(const uint8_t micGain, const uint16_t deviation);
void     BK4819_TxOn(void);
void     BK4819_ExitSubAu(void);

uint16_t BK4819_GetRSSI(void);
int16_t  BK4819_GetRSSI_dBm(void);

#endif
