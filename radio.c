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

#include "audio.h"
#include "driver/bk4819.h"
#include "driver/eeprom.h"
#include "driver/system.h"
#include "frequencies.h"
#include "functions.h"
#include "helper/battery.h"
#include "misc.h"
#include "packet.h"
#include "radio.h"
#include "settings.h"

VFO_Info_t    *gVfo = &gEeprom.Vfo;
VfoState_t     gVfoState;

bool RADIO_CheckValidChannel(uint16_t channel)
{
	// return true if the memory channel appears valid
	if (!IS_MR_CHANNEL(channel))
		return false;

	return gMR_ChannelAttributes[channel].band <= BAND7_470MHz;
}

uint8_t RADIO_FindNextChannel(uint8_t Channel, int8_t Direction)
{
	for (unsigned int i = 0; IS_MR_CHANNEL(i); i++, Channel += Direction) {
		if (Channel == 0xFF) {
			Channel = MR_CHANNEL_LAST;
		} else if (!IS_MR_CHANNEL(Channel)) {
			Channel = MR_CHANNEL_FIRST;
		}

		if (RADIO_CheckValidChannel(Channel)) {
			return Channel;
		}
	}

	return 0xFF;
}

// Frequency for a band slot that has never been stored. Upstream used the
// bottom of the band (137.000 MHz on 2 m); a packet station is better off
// starting inside the amateur allocations.
static uint32_t DefaultFrequency(const uint8_t band)
{
	switch (band) {
		case BAND3_137MHz: return 14480000;   // 144.800 MHz
		case BAND6_400MHz: return 43350000;   // 433.500 MHz
		default:           return frequencyBandTable[band].lower;
	}
}

// Load the channel (memory channel or band slot) that gEeprom.ScreenChannel
// points at into gVfo.
void RADIO_ConfigureChannel(void)
{
	VFO_Info_t *pVfo = gVfo;

	if (!gSetting_350EN) {
		if (gEeprom.FreqChannel == FREQ_CHANNEL_FIRST + BAND5_350MHz)
			gEeprom.FreqChannel = FREQ_CHANNEL_FIRST + BAND6_400MHz;

		if (gEeprom.ScreenChannel == FREQ_CHANNEL_FIRST + BAND5_350MHz)
			gEeprom.ScreenChannel = FREQ_CHANNEL_FIRST + BAND6_400MHz;
	}

	uint8_t channel = gEeprom.ScreenChannel;

	if (IS_MR_CHANNEL(channel)) {
		channel = RADIO_FindNextChannel(channel, RADIO_CHANNEL_UP);
		if (channel == 0xFF) {
			channel = gEeprom.FreqChannel;
		} else {
			gEeprom.MrChannel = channel;
		}
	}
	else if (!IS_FREQ_CHANNEL(channel)) {
		channel = gEeprom.FreqChannel;
	}
	gEeprom.ScreenChannel = channel;

	uint8_t band;
	uint16_t base;
	if (IS_MR_CHANNEL(channel)) {
		band = gMR_ChannelAttributes[channel].band;
		base = channel * 16;
	}
	else {
		band = channel - FREQ_CHANNEL_FIRST;
		base = 0x0C80 + ((channel - FREQ_CHANNEL_FIRST) * 32);   // VFO A
	}

	memset(pVfo, 0, sizeof(*pVfo));
	pVfo->CHANNEL_SAVE = channel;

	uint8_t data[8];
	EEPROM_ReadBuffer(base + 8, data, sizeof(data));

	uint8_t tmp = data[6];
	if (tmp >= STEP_N_ELEM)
		tmp = STEP_12_5kHz;
	pVfo->STEP_SETTING  = tmp;
	pVfo->StepFrequency = gStepFrequencyTable[tmp];

	if (data[4] == 0xFF) {
		pVfo->CHANNEL_BANDWIDTH = BANDWIDTH_WIDE;
		pVfo->OUTPUT_POWER      = OUTPUT_POWER_LOW;
	}
	else {
		pVfo->CHANNEL_BANDWIDTH = (data[4] >> 1) & 1u;
		pVfo->OUTPUT_POWER      = (data[4] >> 2) & 3u;
		if (pVfo->OUTPUT_POWER > OUTPUT_POWER_HIGH)
			pVfo->OUTPUT_POWER = OUTPUT_POWER_LOW;
	}

	uint32_t frequency;
	EEPROM_ReadBuffer(base, &frequency, sizeof(frequency));
	if (frequency == 0xFFFFFFFF)
		frequency = DefaultFrequency(band);

	// fix a previously stored frequency outside its band
	band = FREQUENCY_GetBand(frequency);
	if (frequency < frequencyBandTable[band].lower)
		frequency = frequencyBandTable[band].lower;
	else if (frequency > frequencyBandTable[band].upper)
		frequency = frequencyBandTable[band].upper;
	else if (IS_FREQ_CHANNEL(channel))
		frequency = FREQUENCY_RoundToStep(frequency, pVfo->StepFrequency);

	if (!gSetting_350EN && frequency >= 35000000 && frequency < 40000000)
		frequency = 43300000;

	pVfo->Band      = FREQUENCY_GetBand(frequency);
	pVfo->Frequency = frequency;

	RADIO_ConfigureSquelchAndOutputPower(pVfo);
}

void RADIO_ConfigureSquelchAndOutputPower(VFO_Info_t *pInfo)
{
	// *******************************
	// squelch, from the calibration tables

	FREQUENCY_Band_t Band = FREQUENCY_GetBand(pInfo->Frequency);
	uint16_t Base = (Band < BAND4_174MHz) ? 0x1E60 : 0x1E00;

	if (gEeprom.SQUELCH_LEVEL == 0)
	{	// squelch == 0 (off)
		pInfo->SquelchOpenRSSIThresh    = 0;     // 0 ~ 255
		pInfo->SquelchOpenNoiseThresh   = 127;   // 127 ~ 0
		pInfo->SquelchCloseGlitchThresh = 255;   // 255 ~ 0

		pInfo->SquelchCloseRSSIThresh   = 0;     // 0 ~ 255
		pInfo->SquelchCloseNoiseThresh  = 127;   // 127 ~ 0
		pInfo->SquelchOpenGlitchThresh  = 255;   // 255 ~ 0
	}
	else
	{	// squelch >= 1
		Base += gEeprom.SQUELCH_LEVEL;                                        // my eeprom squelch-1
																			  // VHF   UHF
		EEPROM_ReadBuffer(Base + 0x00, &pInfo->SquelchOpenRSSIThresh,    1);  //  50    10
		EEPROM_ReadBuffer(Base + 0x10, &pInfo->SquelchCloseRSSIThresh,   1);  //  40     5

		EEPROM_ReadBuffer(Base + 0x20, &pInfo->SquelchOpenNoiseThresh,   1);  //  65    90
		EEPROM_ReadBuffer(Base + 0x30, &pInfo->SquelchCloseNoiseThresh,  1);  //  70   100

		EEPROM_ReadBuffer(Base + 0x40, &pInfo->SquelchCloseGlitchThresh, 1);  //  90    90
		EEPROM_ReadBuffer(Base + 0x50, &pInfo->SquelchOpenGlitchThresh,  1);  // 100   100

		uint16_t noise_open   = pInfo->SquelchOpenNoiseThresh;
		uint16_t noise_close  = pInfo->SquelchCloseNoiseThresh;

		uint16_t rssi_open    = pInfo->SquelchOpenRSSIThresh;
		uint16_t rssi_close   = pInfo->SquelchCloseRSSIThresh;
		uint16_t glitch_open  = pInfo->SquelchOpenGlitchThresh;
		uint16_t glitch_close = pInfo->SquelchCloseGlitchThresh;
		// make squelch more sensitive
		// note that 'noise' and 'glitch' values are inverted compared to 'rssi' values
		rssi_open   = (rssi_open   * 1) / 2;
		noise_open  = (noise_open  * 2) / 1;
		glitch_open = (glitch_open * 2) / 1;

		// ensure the 'close' threshold is lower than the 'open' threshold
		if (rssi_close == rssi_open && rssi_close >= 2)
			rssi_close -= 2;
		if (noise_close == noise_open && noise_close  <= 125)
			noise_close += 2;
		if (glitch_close == glitch_open && glitch_close <= 253)
			glitch_close += 2;

		pInfo->SquelchOpenRSSIThresh    = (rssi_open    > 255) ? 255 : rssi_open;
		pInfo->SquelchCloseRSSIThresh   = (rssi_close   > 255) ? 255 : rssi_close;
		pInfo->SquelchOpenGlitchThresh  = (glitch_open  > 255) ? 255 : glitch_open;
		pInfo->SquelchCloseGlitchThresh = (glitch_close > 255) ? 255 : glitch_close;

		pInfo->SquelchOpenNoiseThresh   = (noise_open   > 127) ? 127 : noise_open;
		pInfo->SquelchCloseNoiseThresh  = (noise_close  > 127) ? 127 : noise_close;
	}

	// *******************************
	// output power, from the calibration tables

	uint8_t Txp[3];
	EEPROM_ReadBuffer(0x1ED0 + (Band * 16) + (pInfo->OUTPUT_POWER * 3), Txp, 3);

	pInfo->TXP_CalculatedSetting = FREQUENCY_CalculateOutputPower(
		Txp[0],
		Txp[1],
		Txp[2],
		 frequencyBandTable[Band].lower,
		(frequencyBandTable[Band].lower + frequencyBandTable[Band].upper) / 2,
		 frequencyBandTable[Band].upper,
		pInfo->Frequency);
}

// RX audio gains (REG_48) from the settings. Written on every return to
// receive and on every squelch open.
void RADIO_SetRxAudio(void)
{
	BK4819_WriteRegister(BK4819_REG_48,
		PKT_REG_48_BASE |
		((gEeprom.RX_GAIN     & PKT_RX_GAIN_MAX)     << 4) |
		((gEeprom.RX_DAC_GAIN & PKT_RX_DAC_GAIN_MAX) << 0));
}

// The register override table (settings.h), last word on the registers for
// this phase.
void RADIO_ApplyRegOverrides(uint8_t phase)
{
	for (unsigned int i = 0; i < gRegOverrideCount; i++) {
		const RegOverride_t *o = &gRegOverrides[i];
		if (!(o->phase & phase))
			continue;
		uint16_t v = (BK4819_ReadRegister(o->reg) & o->andMask) | o->orValue;
		if (o->reg == BK4819_REG_40 && (v & PKT_REG_40_DEV_MASK) > PKT_DEVIATION_MAX)
			v = (v & ~PKT_REG_40_DEV_MASK) | PKT_DEVIATION_MAX;
		BK4819_WriteRegister(o->reg, v);
	}
}

// Set the chip up to receive on gVfo. Called at power-on, after every
// transmission, at every squelch close and after any setting change.
void RADIO_SetupRegisters(bool switchToForeground)
{
	// The audio path (speaker amplifier) stays on in this firmware, as in
	// upstream DIG mode, to keep the receive turnaround short.

	BK4819_ToggleGpioOut(BK4819_GPIO6_PIN2_GREEN, false);

	BK4819_SetFilterBandwidth(gVfo->CHANNEL_BANDWIDTH == BANDWIDTH_NARROW);

	BK4819_ToggleGpioOut(BK4819_GPIO5_PIN1_RED, false);

	BK4819_SetupPowerAmplifier(0, 0);

	BK4819_ToggleGpioOut(BK4819_GPIO1_PIN29_PA_ENABLE, false);

	while (1)
	{
		const uint16_t Status = BK4819_ReadRegister(BK4819_REG_0C);
		if ((Status & 1u) == 0) // INTERRUPT REQUEST
			break;

		BK4819_WriteRegister(BK4819_REG_02, 0);
		SYSTEM_DelayMs(1);
	}
	BK4819_WriteRegister(BK4819_REG_3F, 0);

	// mic gain 0.5dB/step 0 to 31
	BK4819_WriteRegister(BK4819_REG_7D, PKT_REG_7D_BASE | (gEeprom.MIC_GAIN & PKT_MIC_GAIN_MAX));

	const uint32_t Frequency = gVfo->Frequency;
	BK4819_SetFrequency(Frequency);

	BK4819_SetupSquelch(
		gVfo->SquelchOpenRSSIThresh,    gVfo->SquelchCloseRSSIThresh,
		gVfo->SquelchOpenNoiseThresh,   gVfo->SquelchCloseNoiseThresh,
		gVfo->SquelchCloseGlitchThresh, gVfo->SquelchOpenGlitchThresh);

	BK4819_PickRXFilterPathBasedOnFrequency(Frequency);

	BK4819_ToggleGpioOut(BK4819_GPIO0_PIN28_RX_ENABLE, true);

	RADIO_SetRxAudio();

	// Scrambler, VOX and compander stay off.
	BK4819_WriteRegister(BK4819_REG_31, BK4819_ReadRegister(BK4819_REG_31) & ~PKT_REG_31_OFF_MASK);

	// Upstream DIG left REG_7E<15> (AGC fix) set after every transmission,
	// and only reset it when the modulation changed, so receive ran with a
	// frozen AGC. Always return to automatic AGC here.
	BK4819_SetAGC(true);

	// enable/disable BK4819 selected interrupts
	BK4819_WriteRegister(BK4819_REG_3F, BK4819_REG_3F_SQUELCH_FOUND | BK4819_REG_3F_SQUELCH_LOST);

	RADIO_ApplyRegOverrides(REG_OVERRIDE_RX);

	FUNCTION_Init();

	if (switchToForeground)
		FUNCTION_Select(FUNCTION_FOREGROUND);
}

void RADIO_SetTxParameters(void)
{
	BK4819_ToggleGpioOut(BK4819_GPIO0_PIN28_RX_ENABLE, false);

	const bool narrow = gVfo->CHANNEL_BANDWIDTH == BANDWIDTH_NARROW;
	BK4819_SetFilterBandwidth(narrow);

	BK4819_SetFrequency(gVfo->Frequency);

	// Scrambler, VOX and compander stay off.
	BK4819_WriteRegister(BK4819_REG_31, BK4819_ReadRegister(BK4819_REG_31) & ~PKT_REG_31_OFF_MASK);

	BK4819_PrepareDigitalTransmit(gEeprom.MIC_GAIN,
		narrow ? gEeprom.DEVIATION_NARROW : gEeprom.DEVIATION_WIDE);

	BK4819_PickRXFilterPathBasedOnFrequency(gVfo->Frequency);

	BK4819_ToggleGpioOut(BK4819_GPIO1_PIN29_PA_ENABLE, true);

	SYSTEM_DelayMs(5);

	BK4819_SetupPowerAmplifier(gVfo->TXP_CalculatedSetting, gVfo->Frequency);

	SYSTEM_DelayMs(10);

	BK4819_ExitSubAu();   // no CTCSS/DCS

	RADIO_ApplyRegOverrides(REG_OVERRIDE_TX);
}

void RADIO_SetVfoState(VfoState_t State)
{
	gVfoState = State;
	gVFOStateResumeCountdown_500ms = (State == VFO_STATE_NORMAL) ? 0 : vfo_state_resume_countdown_500ms;
	gUpdateDisplay = true;
}

void RADIO_PrepareTX(void)
{
	VfoState_t State = VFO_STATE_NORMAL;  // default to OK to TX

	if (TX_freq_check(gVfo->Frequency) != 0) {
		// TX frequency not allowed
		State = VFO_STATE_TX_DISABLE;
	} else if (SerialConfigInProgress()) {
		// config upload/download in progress
		State = VFO_STATE_TX_DISABLE;
	} else if (gBatteryDisplayLevel == 0) {
		State = VFO_STATE_BAT_LOW;
	} else if (gBatteryDisplayLevel > 6) {
		// over voltage (above about 8.9 V)
		State = VFO_STATE_VOLTAGE_HIGH;
	}

	if (State != VFO_STATE_NORMAL) {
		// TX not allowed
		RADIO_SetVfoState(State);
		return;
	}

	FUNCTION_Select(FUNCTION_TRANSMIT);

	gTxTimerCountdown_500ms = 2u * gTxTimeoutSeconds[gEeprom.TX_TIMEOUT];
	gTxTimeoutReached       = false;
}

void RADIO_SendEndOfTransmission(void)
{
	// No roger beep, no DTMF, no tail tone: straight back to receive.
	RADIO_SetupRegisters(false);
}
