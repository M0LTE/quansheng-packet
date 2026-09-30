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

#include <string.h>

#include "driver/bk4819.h"
#include "driver/eeprom.h"
#include "misc.h"
#include "packet.h"
#include "ptt.h"
#include "settings.h"

EEPROM_Config_t gEeprom = { 0 };

const uint8_t gTxTimeoutSeconds[7] = {5, 10, 15, 20, 30, 60, 120};

RegOverride_t gRegOverridesRam[REG_OVERRIDE_MAX];
uint8_t       gRegOverrideRamCount;

V2_Config_t   gV2;
bool          gSettingsBlockValid;

// Registers an override (or a v2 REG_WRITE) may never touch: soft reset (00), TX/RX and PA
// enables (30), GPIO outputs incl. PA enable, RX enable, LNA switch and
// LEDs (33), PA bias and gain (36), power and LDOs (37), frequency (38,
// 39), crystal trim (3B, 3C). Anything above 0x7F is not a register.
bool SETTINGS_RegOverrideAllowed(uint8_t reg)
{
	switch (reg) {
		case 0x00: case 0x30: case 0x33: case 0x36: case 0x37:
		case 0x38: case 0x39: case 0x3B: case 0x3C:
			return false;
		default:
			return reg <= 0x7F;
	}
}

static uint8_t ByteOr(uint8_t value, uint8_t max, uint8_t def)
{
	return (value <= max) ? value : def;
}

void SETTINGS_Decode(const uint8_t Data[16], const uint8_t T[8], EEPROM_Config_t *e)
{
	e->PTT_PRESS_MS       = (T[0] >= PTT_PRESS_MIN_MS   && T[0] <= PTT_DEBOUNCE_MAX_MS) ? T[0] : PTT_PRESS_DEFAULT_MS;
	e->PTT_RELEASE_MS     = (T[1] >= PTT_RELEASE_MIN_MS && T[1] <= PTT_DEBOUNCE_MAX_MS) ? T[1] : PTT_RELEASE_DEFAULT_MS;
	e->PA_ENABLE_DELAY_MS = (T[2] >= PA_ENABLE_DELAY_MIN_MS && T[2] <= PA_DELAY_MAX_MS) ? T[2] : PA_ENABLE_DELAY_DEFAULT;
	e->PA_BIAS_DELAY_MS   = ByteOr(T[3], PA_DELAY_MAX_MS, PA_BIAS_DELAY_DEFAULT);
	const uint16_t devWide   = Data[4] | (Data[5] << 8);
	const uint16_t devNarrow = Data[6] | (Data[7] << 8);
	e->BUSY_LEVEL       = ByteOr(Data[1], 9, 1) ? ByteOr(Data[1], 9, 1) : 1;
	e->TX_TIMEOUT       = ByteOr(Data[2], ARRAY_SIZE(gTxTimeoutSeconds) - 1, TX_TIMEOUT_DEFAULT_INDEX);
	e->DEVIATION_WIDE   = (devWide   <= PKT_DEVIATION_MAX) ? devWide   : PKT_DEVIATION_WIDE_DEFAULT;
	e->DEVIATION_NARROW = (devNarrow <= PKT_DEVIATION_MAX) ? devNarrow : PKT_DEVIATION_NARROW_DEFAULT;
	e->RX_GAIN          = Data[8];   // checked in SETTINGS_LoadCalibration
	e->RX_DAC_GAIN      = ByteOr(Data[9], PKT_RX_DAC_GAIN_MAX, PKT_RX_DAC_GAIN_DEFAULT);
	e->BACKLIGHT_TIME   = ByteOr(Data[10], 7, 3);
	e->BATTERY_TYPE     = ByteOr(Data[11], BATTERY_TYPE_2200_MAH, BATTERY_TYPE_1600_MAH);
	e->KEY_LOCK         = ByteOr(Data[12], 1, 0);
}

void SETTINGS_DecodeV2(const uint8_t b[16], bool valid, V2_Config_t *v)
{
	uint8_t blank[16];
	if (!valid || b[0] != SETTINGS_V2_VERSION) {
		memset(blank, 0xFF, sizeof(blank));
		b = blank;
		valid = false;
	}
	v->valid           = valid;
	v->SERIAL_LOCK_MS  = (b[1] <= SERIAL_LOCK_MAX_MS / 10) ? b[1] * 10u : SERIAL_LOCK_DEFAULT_MS;
	v->BUSY_SOURCE     = (b[2] >= 1 && b[2] <= 3) ? b[2] : BUSY_SOURCE_SQUELCH;
	v->BUSY_HANG_MS    = ByteOr(b[3], 250, BUSY_HANG_DEFAULT_MS);
	const uint16_t open  = b[4] | (b[5] << 8);
	const uint16_t close = b[6] | (b[7] << 8);
	v->BUSY_RSSI_OPEN  = (open <= 511) ? open : BUSY_RSSI_OPEN_DEFAULT;
	v->BUSY_RSSI_CLOSE = (close <= 511) ? close : BUSY_RSSI_CLOSE_DEFAULT;
	if (v->BUSY_RSSI_CLOSE > v->BUSY_RSSI_OPEN)
		v->BUSY_RSSI_CLOSE = v->BUSY_RSSI_OPEN;
	const uint32_t mask = b[8] | (b[9] << 8) | ((uint32_t)b[10] << 16) | ((uint32_t)b[11] << 24);
	v->DEFAULT_MASK    = (mask == 0xFFFFFFFFu) ? 0 : mask;
	const uint16_t hb  = b[12] | (b[13] << 8);
	v->DEFAULT_HEARTBEAT_MS = (hb >= 100 && hb <= 60000) ? hb : 0;
	v->DEFAULT_OPTIONS = ByteOr(b[14], 1, 0);
	v->TONE_CAL        = (b[15] >= 1 && b[15] <= 127) ? b[15] : 0;
}

uint8_t SETTINGS_FactoryRxGain(void)
{
	uint8_t g;
	EEPROM_ReadBuffer(0x1F8E, &g, 1);
	return (g <= PKT_RX_GAIN_MAX) ? g : PKT_RX_GAIN_FALLBACK;
}

void SETTINGS_InitEEPROM(void)
{
	uint8_t Data[16];

	// Packet firmware settings (see settings.h)
	EEPROM_ReadBuffer(SETTINGS_PKT_BLOCK, Data, 16);
	const bool blockValid = Data[0] == SETTINGS_PKT_VERSION;
	gSettingsBlockValid = blockValid;
	if (!blockValid)
		memset(Data, 0xFF, 16);   // blank, or left over from another firmware: all defaults

	// 1D50..1D57: key-up and key-down timing
	uint8_t T[8];
	EEPROM_ReadBuffer(SETTINGS_TIMING, T, 8);
	if (!blockValid)
		memset(T, 0xFF, 8);
	SETTINGS_Decode(Data, T, &gEeprom);
	gEeprom.BACKLIGHT_MIN    = 0;
	gEeprom.BACKLIGHT_MAX    = 10;

	// 1D60..1D6F: protocol v2 settings
	EEPROM_ReadBuffer(SETTINGS_V2_BLOCK, Data, 16);
	SETTINGS_DecodeV2(Data, blockValid, &gV2);

	// 0EA0..0EA7: S-meter levels
	EEPROM_ReadBuffer(0x0EA0, Data, 8);
	if((Data[1] < 200 && Data[1] > 90) && (Data[2] < Data[1]-9 && Data[1] < 160  && Data[2] > 50)) {
		gEeprom.S0_LEVEL = Data[1];
		gEeprom.S9_LEVEL = Data[2];
	}
	else {
		gEeprom.S0_LEVEL = 130;
		gEeprom.S9_LEVEL = 76;
	}

	// 0F40..0F47: TX frequency limits (no menu; set them over UART)
	EEPROM_ReadBuffer(0x0F40, Data, 8);
	gSetting_F_LOCK            = (Data[0] < F_LOCK_LEN) ? Data[0] : F_LOCK_DEF;
	gSetting_350TX             = (Data[1] < 2) ? Data[1] : false;
	gSetting_200TX             = (Data[3] < 2) ? Data[3] : false;
	gSetting_500TX             = (Data[4] < 2) ? Data[4] : false;
	gSetting_350EN             = (Data[5] < 2) ? Data[5] : true;

	// 1D58..1D5F: the operating channel (after the 350 MHz setting, which
	// decides what is receivable)
	memset(&gEeprom.Vfo, 0, sizeof(gEeprom.Vfo));
	EEPROM_ReadBuffer(SETTINGS_OPERATING, Data, 8);
	if (!blockValid || !SETTINGS_DecodeOperating(Data, &gEeprom.Vfo)) {
		SETTINGS_ImportOldFrequency(&gEeprom.Vfo);
		if (blockValid)
			SETTINGS_SaveOperating();   // once: the old layout is not read again
	}
}

bool SETTINGS_DecodeOperating(const uint8_t b[8], VFO_Info_t *v)
{
	const uint32_t f = b[0] | (b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
	if (!FREQUENCY_IsReceivable(f))
		return false;
	v->Frequency         = f;
	v->OUTPUT_POWER      = ByteOr(b[4], OUTPUT_POWER_HIGH, OUTPUT_POWER_LOW);
	v->CHANNEL_BANDWIDTH = ByteOr(b[5], BANDWIDTH_NARROW, BANDWIDTH_WIDE);
	v->STEP_SETTING      = ByteOr(b[6], STEP_N_ELEM - 1, STEP_12_5kHz);
	return true;
}

// The frequency the radio used in the upstream layout (the channel indices
// at 0x0E80 and the memory channel or band slot record they point at), if
// it is receivable; else 144.800 MHz. Nothing is written.
void SETTINGS_ImportOldFrequency(VFO_Info_t *v)
{
	uint8_t  d[8];
	uint32_t f;
	uint16_t base;

	v->Frequency         = RADIO_DEFAULT_FREQUENCY;
	v->OUTPUT_POWER      = OUTPUT_POWER_LOW;
	v->CHANNEL_BANDWIDTH = BANDWIDTH_WIDE;
	v->STEP_SETTING      = STEP_12_5kHz;

	EEPROM_ReadBuffer(0x0E80, d, 8);
	if (d[0] <= 199)
		base = d[0] * 16u;                        // memory channel
	else if (d[0] <= 206)
		base = 0x0C80 + (d[0] - 200) * 32u;       // band slot, VFO A
	else
		return;

	EEPROM_ReadBuffer(base, &f, sizeof(f));
	if (!FREQUENCY_IsReceivable(f))
		return;
	EEPROM_ReadBuffer(base + 8, d, 8);
	v->Frequency = f;
	if (d[4] != 0xFF) {
		v->CHANNEL_BANDWIDTH = (d[4] >> 1) & 1u;
		v->OUTPUT_POWER      = ByteOr((d[4] >> 2) & 3u, OUTPUT_POWER_HIGH, OUTPUT_POWER_LOW);
	}
	v->STEP_SETTING = ByteOr(d[6], STEP_N_ELEM - 1, STEP_12_5kHz);
}

void SETTINGS_LoadCalibration(void)
{
	EEPROM_ReadBuffer(0x1F40, gBatteryCalibration, 12);
	if (gBatteryCalibration[0] >= 5000)
	{
		gBatteryCalibration[0] = 1900;
		gBatteryCalibration[1] = 2000;
	}
	gBatteryCalibration[5] = 2300;

	struct
	{
		int16_t  BK4819_XtalFreqLow;
		uint16_t EEPROM_1F8A;
		uint16_t EEPROM_1F8C;
		uint8_t  VOLUME_GAIN;
		uint8_t  DAC_GAIN;
	} __attribute__((packed)) Misc;

	EEPROM_ReadBuffer(0x1F88, &Misc, 8);

	gEeprom.BK4819_XTAL_FREQ_LOW = (Misc.BK4819_XtalFreqLow >= -1000 && Misc.BK4819_XtalFreqLow <= 1000) ? Misc.BK4819_XtalFreqLow : 0;

	// The RX AF gain setting defaults to the factory volume calibration.
	if (gEeprom.RX_GAIN > PKT_RX_GAIN_MAX)
		gEeprom.RX_GAIN = (Misc.VOLUME_GAIN <= PKT_RX_GAIN_MAX) ? Misc.VOLUME_GAIN : PKT_RX_GAIN_FALLBACK;

	BK4819_WriteRegister(BK4819_REG_3B, 22656 + gEeprom.BK4819_XTAL_FREQ_LOW);
}

void SETTINGS_SaveSettings(void)
{
	uint8_t State[16];

	// First save over data that is not ours: blank the timing, operating
	// and v2 blocks too, so leftovers there never become settings.
	EEPROM_ReadBuffer(SETTINGS_PKT_BLOCK, State, 1);
	if (State[0] != SETTINGS_PKT_VERSION) {
		memset(State, 0xFF, sizeof(State));
		EEPROM_WriteBuffer(SETTINGS_TIMING, State);
		EEPROM_WriteBuffer(SETTINGS_OPERATING, State);
		EEPROM_WriteBuffer(SETTINGS_V2_BLOCK + 0, State);
		EEPROM_WriteBuffer(SETTINGS_V2_BLOCK + 8, State);
	}

	memset(State, 0xFF, sizeof(State));
	State[0]  = SETTINGS_PKT_VERSION;
	State[1]  = gEeprom.BUSY_LEVEL;
	State[2]  = gEeprom.TX_TIMEOUT;
	State[4]  = gEeprom.DEVIATION_WIDE & 0xFF;
	State[5]  = gEeprom.DEVIATION_WIDE >> 8;
	State[6]  = gEeprom.DEVIATION_NARROW & 0xFF;
	State[7]  = gEeprom.DEVIATION_NARROW >> 8;
	State[8]  = gEeprom.RX_GAIN;
	State[9]  = gEeprom.RX_DAC_GAIN;
	State[10] = gEeprom.BACKLIGHT_TIME;
	State[11] = gEeprom.BATTERY_TYPE;
	State[12] = gEeprom.KEY_LOCK;
	EEPROM_WriteBuffer(SETTINGS_PKT_BLOCK + 0, State + 0);
	EEPROM_WriteBuffer(SETTINGS_PKT_BLOCK + 8, State + 8);
	gSettingsBlockValid = true;
}

// Store the operating frequency, power, bandwidth and step. Over foreign
// data at 0x1D00 the settings block is written first (with the settings in
// use), as the first menu save does, so the operating channel counts.
void SETTINGS_SaveOperating(void)
{
	uint8_t b[8];

	EEPROM_ReadBuffer(SETTINGS_PKT_BLOCK, b, 1);
	if (b[0] != SETTINGS_PKT_VERSION)
		SETTINGS_SaveSettings();

	memset(b, 0xFF, sizeof(b));
	memcpy(b, &gVfo->Frequency, 4);
	b[4] = gVfo->OUTPUT_POWER;
	b[5] = gVfo->CHANNEL_BANDWIDTH;
	b[6] = gVfo->STEP_SETTING;
	EEPROM_WriteBuffer(SETTINGS_OPERATING, b);
}
