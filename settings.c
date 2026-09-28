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
#include "settings.h"

EEPROM_Config_t gEeprom = { 0 };

const uint8_t gTxTimeoutSeconds[7] = {5, 10, 15, 20, 30, 60, 120};

RegOverride_t gRegOverrides[REG_OVERRIDE_MAX];
uint8_t       gRegOverrideCount;

// Registers an override may never touch: soft reset (00), TX/RX and PA
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

static void LoadRegOverrides(bool blockValid)
{
	gRegOverrideCount = 0;
	if (!blockValid)
		return;

	for (unsigned int i = 0; i < REG_OVERRIDE_MAX; i++) {
		uint8_t e[8];
		EEPROM_ReadBuffer(SETTINGS_REG_OVERRIDES + i * 8, e, 8);
		const uint8_t phase = e[0] & (REG_OVERRIDE_TX | REG_OVERRIDE_RX);
		if (e[0] == 0xFF || phase == 0 || e[1] == 0xFF)
			break;                          // end of the list
		if (!SETTINGS_RegOverrideAllowed(e[1]))
			continue;                       // refused, skipped
		RegOverride_t *o = &gRegOverrides[gRegOverrideCount++];
		o->phase   = phase;
		o->reg     = e[1];
		o->andMask = e[2] | (e[3] << 8);
		o->orValue = e[4] | (e[5] << 8);
	}
}

static uint8_t ByteOr(uint8_t value, uint8_t max, uint8_t def)
{
	return (value <= max) ? value : def;
}

void SETTINGS_InitEEPROM(void)
{
	uint8_t Data[16];

	// Packet firmware settings (see settings.h)
	EEPROM_ReadBuffer(SETTINGS_PKT_BLOCK, Data, 16);
	const bool blockValid = Data[0] == SETTINGS_PKT_VERSION;
	if (!blockValid)
		memset(Data, 0xFF, 16);   // blank, or left over from another firmware: all defaults
	LoadRegOverrides(blockValid);
	const uint16_t devWide   = Data[4] | (Data[5] << 8);
	const uint16_t devNarrow = Data[6] | (Data[7] << 8);
	gEeprom.SQUELCH_LEVEL    = ByteOr(Data[1], 9, 1);
	gEeprom.TX_TIMEOUT       = ByteOr(Data[2], ARRAY_SIZE(gTxTimeoutSeconds) - 1, TX_TIMEOUT_DEFAULT_INDEX);
	gEeprom.MIC_GAIN         = ByteOr(Data[3], PKT_MIC_GAIN_MAX, PKT_MIC_GAIN_DEFAULT);
	gEeprom.DEVIATION_WIDE   = (devWide   <= PKT_DEVIATION_MAX) ? devWide   : PKT_DEVIATION_WIDE_DEFAULT;
	gEeprom.DEVIATION_NARROW = (devNarrow <= PKT_DEVIATION_MAX) ? devNarrow : PKT_DEVIATION_NARROW_DEFAULT;
	gEeprom.RX_GAIN          = Data[8];   // checked in SETTINGS_LoadCalibration
	gEeprom.RX_DAC_GAIN      = ByteOr(Data[9], PKT_RX_DAC_GAIN_MAX, PKT_RX_DAC_GAIN_DEFAULT);
	gEeprom.BACKLIGHT_TIME   = ByteOr(Data[10], 7, 3);
	gEeprom.BATTERY_TYPE     = ByteOr(Data[11], BATTERY_TYPE_2200_MAH, BATTERY_TYPE_1600_MAH);
	gEeprom.KEY_LOCK         = ByteOr(Data[12], 1, 0);
	gEeprom.BACKLIGHT_MIN    = 0;
	gEeprom.BACKLIGHT_MAX    = 10;

	// 0E80..0E87: channel indices (upstream layout, VFO A)
	EEPROM_ReadBuffer(0x0E80, Data, 8);
	gEeprom.ScreenChannel = IS_VALID_CHANNEL(Data[0]) ? Data[0] : (FREQ_CHANNEL_FIRST + BAND3_137MHz);
	gEeprom.MrChannel     = IS_MR_CHANNEL(Data[1])    ? Data[1] : MR_CHANNEL_FIRST;
	gEeprom.FreqChannel   = IS_FREQ_CHANNEL(Data[2])  ? Data[2] : (FREQ_CHANNEL_FIRST + BAND3_137MHz);

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

	// 0D60..0E27: memory channel attributes
	EEPROM_ReadBuffer(0x0D60, gMR_ChannelAttributes, sizeof(gMR_ChannelAttributes));
	for(uint16_t i = 0; i < sizeof(gMR_ChannelAttributes); i++) {
		ChannelAttributes_t *att = &gMR_ChannelAttributes[i];
		if(att->__val == 0xff){
			att->__val = 0;
			att->band = 0xf;
		}
	}
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

void SETTINGS_SaveVfoIndices(void)
{
	uint8_t State[8];

	EEPROM_ReadBuffer(0x0E80, State, sizeof(State));
	State[0] = gEeprom.ScreenChannel;
	State[1] = gEeprom.MrChannel;
	State[2] = gEeprom.FreqChannel;
	EEPROM_WriteBuffer(0x0E80, State);
}

void SETTINGS_SaveSettings(void)
{
	uint8_t State[16];

	// First save over data that is not ours: blank the override table too,
	// so leftovers there never become register overrides.
	EEPROM_ReadBuffer(SETTINGS_PKT_BLOCK, State, 1);
	if (State[0] != SETTINGS_PKT_VERSION) {
		memset(State, 0xFF, sizeof(State));
		for (unsigned int i = 0; i < REG_OVERRIDE_MAX; i++)
			EEPROM_WriteBuffer(SETTINGS_REG_OVERRIDES + i * 8, State);
	}

	memset(State, 0xFF, sizeof(State));
	State[0]  = SETTINGS_PKT_VERSION;
	State[1]  = gEeprom.SQUELCH_LEVEL;
	State[2]  = gEeprom.TX_TIMEOUT;
	State[3]  = gEeprom.MIC_GAIN;
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
}

// Store frequency, power, bandwidth and step for the channel or band slot in
// use. The other fields of the 16-byte record (offset, tones, modulation,
// scrambler, name) are left as they are.
void SETTINGS_SaveChannel(const VFO_Info_t *pVFO)
{
	const uint8_t Channel = pVFO->CHANNEL_SAVE;
	uint16_t Offset = Channel * 16;
	uint8_t  Head[8];
	uint8_t  Tail[8];

	if (IS_FREQ_CHANNEL(Channel)) // a band slot, VFO A
		Offset = 0x0C80 + (Channel - FREQ_CHANNEL_FIRST) * 32;

	EEPROM_ReadBuffer(Offset + 0, Head, 8);
	EEPROM_ReadBuffer(Offset + 8, Tail, 8);

	// A record never written before (flags byte 0xFF) gets zeros in the
	// fields this firmware does not use: no offset, no tones, FM, no
	// scrambler, so it reads cleanly in other firmwares and CHIRP.
	const bool blank = Tail[4] == 0xFF;
	if (blank) {
		memset(Head, 0, sizeof(Head));
		memset(Tail, 0, sizeof(Tail));
	}

	memcpy(Head, &pVFO->Frequency, 4);
	EEPROM_WriteBuffer(Offset + 0, Head);

	Tail[4] = (Tail[4] & ~((3u << 2) | (1u << 1))) | (pVFO->OUTPUT_POWER << 2) | (pVFO->CHANNEL_BANDWIDTH << 1);
	Tail[6] = pVFO->STEP_SETTING;
	EEPROM_WriteBuffer(Offset + 8, Tail);
}
