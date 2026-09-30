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

// Packet firmware: one operating frequency, simplex only (TX frequency = RX
// frequency), no CTCSS/DCS, no modulation choice, no memory channels.
// Frequency, power, bandwidth and step are stored in the settings
// (settings.h, 0x1D58).
typedef struct VFO_Info_t
{
	uint32_t       Frequency;
	uint16_t       StepFrequency;
	STEP_Setting_t STEP_SETTING;

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

extern uint8_t        gSqlRaw[6];
extern bool           gSqlRawActive;
extern uint8_t        gAgcFix;
extern bool           gAfcOn;
extern uint32_t       gTxCarrierOffMs;

#define  RADIO_DEFAULT_FREQUENCY  14480000u   // 144.800 MHz, 10 Hz units

void     RADIO_ConfigureChannel(void);
void     RADIO_ConfigureSquelchAndOutputPower(VFO_Info_t *pInfo);
void     RADIO_SetupRegisters(bool switchToForeground);
void     RADIO_SetRxAudio(void);
void     RADIO_ApplyRegOverrides(uint8_t phase);
void     RADIO_SetTxParameters(void);
void     RADIO_SetVfoState(VfoState_t State);
uint8_t  RADIO_TxBar(void);
void     RADIO_PrepareTX(void);
// app.c: RADIO_PrepareTX refused a key-up the PTT rules had allowed
void     APP_TxRefusedAtKeyUp(uint8_t reason);
void     RADIO_SendEndOfTransmission(void);

#endif
