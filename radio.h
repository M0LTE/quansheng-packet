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

#ifndef RADIO_H
#define RADIO_H

#include <stdbool.h>
#include <stdint.h>

#include "frequencies.h"

enum {
	RADIO_CHANNEL_UP   = 0x01u,
	RADIO_CHANNEL_DOWN = 0xFFu,
};

enum {
	BANDWIDTH_WIDE = 0,
	BANDWIDTH_NARROW
};

enum VfoState_t
{
	VFO_STATE_NORMAL = 0,
	VFO_STATE_BAT_LOW,
	VFO_STATE_TX_DISABLE,
	VFO_STATE_TIMEOUT,
	VFO_STATE_VOLTAGE_HIGH,
	_VFO_STATE_LAST_ELEMENT
};
typedef enum VfoState_t VfoState_t;

// Packet firmware: one VFO, simplex only (TX frequency = RX frequency), no
// CTCSS/DCS, no modulation choice. Channel memories keep the upstream EEPROM
// layout; the offset, tone, modulation and scrambler fields in them are
// ignored and left as they are.
typedef struct VFO_Info_t
{
	uint32_t       Frequency;
	uint16_t       StepFrequency;
	STEP_Setting_t STEP_SETTING;

	uint8_t        CHANNEL_SAVE;   // memory channel (0-199) or band slot (200-206)
	uint8_t        Band;

	uint8_t        SquelchOpenRSSIThresh;
	uint8_t        SquelchOpenNoiseThresh;
	uint8_t        SquelchCloseGlitchThresh;
	uint8_t        SquelchCloseRSSIThresh;
	uint8_t        SquelchCloseNoiseThresh;
	uint8_t        SquelchOpenGlitchThresh;

	uint8_t        OUTPUT_POWER;
	uint8_t        TXP_CalculatedSetting;
	uint8_t        CHANNEL_BANDWIDTH;
} VFO_Info_t;

extern VFO_Info_t    *gVfo;
extern VfoState_t     gVfoState;

bool     RADIO_CheckValidChannel(uint16_t channel);
uint8_t  RADIO_FindNextChannel(uint8_t ChNum, int8_t Direction);
void     RADIO_ConfigureChannel(void);
void     RADIO_ConfigureSquelchAndOutputPower(VFO_Info_t *pInfo);
void     RADIO_SetupRegisters(bool switchToForeground);
void     RADIO_SetRxAudio(void);
void     RADIO_ApplyRegOverrides(uint8_t phase);
void     RADIO_SetTxParameters(void);
void     RADIO_SetVfoState(VfoState_t State);
void     RADIO_PrepareTX(void);
void     RADIO_SendEndOfTransmission(void);

#endif
