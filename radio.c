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
#include "pttarb.h"
#include "radio.h"
#include "settings.h"
#include "app/monitor.h"

VFO_Info_t    *gVfo = &gEeprom.Vfo;
VfoState_t     gVfoState;

// RAM-only diagnostics set over the v2 protocol (docs/protocol-v2.md 7)
uint8_t        gSqlRaw[6];          // RSSI open/close, noise open/close, glitch open/close
bool           gSqlRawActive;
uint8_t        gAgcFix = 0xFF;      // 0xFF automatic, else REG_7E<14:12> fixed
bool           gAfcOn  = true;
uint32_t       gTxCarrierOffMs;     // g_ms when the carrier went off

// The operating channel: gVfo's frequency, power, bandwidth and step, as
// loaded from the settings (or set by the keypad, menu or protocol), made
// valid, with the band, the squelch thresholds and the TX power worked out
// from the factory calibration.
void RADIO_ConfigureChannel(void)
{
	VFO_Info_t *pVfo = gVfo;

	if (pVfo->STEP_SETTING >= STEP_N_ELEM)
		pVfo->STEP_SETTING = STEP_12_5kHz;
	pVfo->StepFrequency = gStepFrequencyTable[pVfo->STEP_SETTING];

	if (pVfo->OUTPUT_POWER > OUTPUT_POWER_HIGH)
		pVfo->OUTPUT_POWER = OUTPUT_POWER_DEFAULT;
	if (pVfo->CHANNEL_BANDWIDTH > BANDWIDTH_NARROW)
		pVfo->CHANNEL_BANDWIDTH = BANDWIDTH_WIDE;

	if (!FREQUENCY_IsReceivable(pVfo->Frequency))
		pVfo->Frequency = RADIO_DEFAULT_FREQUENCY;
	pVfo->Band = FREQUENCY_GetBand(pVfo->Frequency);

	RADIO_ConfigureSquelchAndOutputPower(pVfo);
}

void RADIO_ConfigureSquelchAndOutputPower(VFO_Info_t *pInfo)
{
	// *******************************
	// the chip's squelch thresholds, from the calibration tables: only a
	// carrier detector (the busy events); they never mute the audio

	FREQUENCY_Band_t Band = FREQUENCY_GetBand(pInfo->Frequency);
	uint16_t Base = (Band < BAND4_174MHz) ? 0x1E60 : 0x1E00;

	{	// the busy detector level, 1 to 9, indexes the factory squelch tables
		Base += gEeprom.BUSY_LEVEL;                                           // my eeprom squelch-1
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

	// BUSY_SQL_RAW (protocol v2) replaces the table values until
	// BUSY_SQL_LEVEL is set
	if (gSqlRawActive) {
		pInfo->SquelchOpenRSSIThresh    = gSqlRaw[0];
		pInfo->SquelchCloseRSSIThresh   = gSqlRaw[1];
		pInfo->SquelchOpenNoiseThresh   = gSqlRaw[2];
		pInfo->SquelchCloseNoiseThresh  = gSqlRaw[3];
		pInfo->SquelchOpenGlitchThresh  = gSqlRaw[4];
		pInfo->SquelchCloseGlitchThresh = gSqlRaw[5];
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
// receive.
void RADIO_SetRxAudio(void)
{
	BK4819_WriteRegister(BK4819_REG_48,
		PKT_REG_48_BASE |
		((gEeprom.RX_GAIN     & PKT_RX_GAIN_MAX)     << 4) |
		((gEeprom.RX_DAC_GAIN & PKT_RX_DAC_GAIN_MAX) << 0));
}

// The RAM register overrides (settings.h), last word on the registers for
// this phase.
void RADIO_ApplyRegOverrides(uint8_t phase)
{
	for (unsigned int i = 0; i < gRegOverrideRamCount; i++) {
		const RegOverride_t *o = &gRegOverridesRam[i];
		if (!(o->phase & phase))
			continue;
		uint16_t v = (BK4819_ReadRegister(o->reg) & o->andMask) | o->orValue;
		if (o->reg == BK4819_REG_40 && (v & PKT_REG_40_DEV_MASK) > PKT_DEVIATION_MAX)
			v = (v & ~PKT_REG_40_DEV_MASK) | PKT_DEVIATION_MAX;
		BK4819_WriteRegister(o->reg, v);
	}
}

// Set the chip up to receive on gVfo, audio open. Called at power-on, after
// every transmission and after any setting change.
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

	// mic gain: fixed at the maximum (packet.h)
	BK4819_WriteRegister(BK4819_REG_7D, PKT_REG_7D);

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
	if (gAgcFix <= 7)   // AGC_FIX diagnostic (protocol v2)
		BK4819_WriteRegister(BK4819_REG_7E, (BK4819_ReadRegister(BK4819_REG_7E) & ~0xF000u) | PKT_REG_7E_AGC_FIX | ((uint16_t)gAgcFix << 12));

	// No chip interrupts (REG_3F stays 0): the squelch result is polled as a
	// detector and never gates anything. Receive audio is always open: the
	// flat FM demodulator output to the AF output.
	BK4819_SetAF(BK4819_AF_FM);
	BK4819_SetRegValue(afcDisableRegSpec, !gAfcOn);   // AFC on unless the diagnostic says off
	BK4819_WriteRegister(BK4819_REG_3D, PKT_REG_3D_RX);

	RADIO_ApplyRegOverrides(REG_OVERRIDE_RX);

	FUNCTION_Init();

	if (switchToForeground)
		FUNCTION_Select(FUNCTION_FOREGROUND);

	MON_AfterRxSetup();   // the speaker amplifier on; a running level tone survives
}

void RADIO_SetTxParameters(void)
{
	BK4819_ToggleGpioOut(BK4819_GPIO0_PIN28_RX_ENABLE, false);

	const bool narrow = gVfo->CHANNEL_BANDWIDTH == BANDWIDTH_NARROW;
	BK4819_SetFilterBandwidth(narrow);

	BK4819_SetFrequency(gVfo->Frequency);

	// Scrambler, VOX and compander stay off.
	BK4819_WriteRegister(BK4819_REG_31, BK4819_ReadRegister(BK4819_REG_31) & ~PKT_REG_31_OFF_MASK);

	BK4819_PrepareDigitalTransmit(narrow ? gEeprom.DEVIATION_NARROW : gEeprom.DEVIATION_WIDE);

	BK4819_PickRXFilterPathBasedOnFrequency(gVfo->Frequency);

	BK4819_ToggleGpioOut(BK4819_GPIO1_PIN29_PA_ENABLE, true);

	// PA ramp: upstream waited 5 ms then 10 ms; both are settings now
	SYSTEM_DelayMs(gEeprom.PA_ENABLE_DELAY_MS);   // at least 1 ms

	BK4819_SetupPowerAmplifier(gVfo->TXP_CalculatedSetting, gVfo->Frequency);

	if (gEeprom.PA_BIAS_DELAY_MS)
		SYSTEM_DelayMs(gEeprom.PA_BIAS_DELAY_MS);

	BK4819_ExitSubAu();   // no CTCSS/DCS

	RADIO_ApplyRegOverrides(REG_OVERRIDE_TX);
}

void RADIO_SetVfoState(VfoState_t State)
{
	gVfoState = State;
	gVFOStateResumeCountdown_500ms = (State == VFO_STATE_NORMAL) ? 0 : vfo_state_resume_countdown_500ms;
	gUpdateDisplay = true;
}

// Why a transmission is not allowed now (TXR_*), or TXR_NONE.
uint8_t RADIO_TxBar(void)
{
	if (gReducedService)
		return TXR_REDUCED_SERVICE;
	if (TX_freq_check(gVfo->Frequency) != 0)
		return TXR_TX_BAND;
	if (gBatteryDisplayLevel == 0)
		return TXR_BATTERY_EMPTY;
	if (gBatteryDisplayLevel > 6)      // over voltage (above about 8.9 V)
		return TXR_OVER_VOLTAGE;
	return TXR_NONE;
}

void RADIO_PrepareTX(void)
{
	uint8_t bar = RADIO_TxBar();
	if (bar == TXR_NONE && SerialConfigInProgress())
		bar = TXR_LOCK;   // the caller checked; a frame came in between

	if (bar != TXR_NONE) {
		// TX not allowed
		static const VfoState_t state[] = {
			[TXR_LOCK]            = VFO_STATE_TX_DISABLE,
			[TXR_TX_BAND]         = VFO_STATE_TX_DISABLE,
			[TXR_BATTERY_EMPTY]   = VFO_STATE_BAT_LOW,
			[TXR_OVER_VOLTAGE]    = VFO_STATE_VOLTAGE_HIGH,
			[TXR_REDUCED_SERVICE] = VFO_STATE_BAT_LOW,
		};
		RADIO_SetVfoState(state[bar]);
		APP_TxRefusedAtKeyUp(bar);
		return;
	}

	FUNCTION_Select(FUNCTION_TRANSMIT);

	gTxTimerCountdown_500ms = 2u * gTxTimeoutSeconds[gEeprom.TX_TIMEOUT];
	gTxTimeoutReached       = false;
}

void RADIO_SendEndOfTransmission(void)
{
	// No roger beep, no DTMF, no tail tone. Drop the carrier first (the same
	// PA writes RADIO_SetupRegisters makes, just earlier: upstream changed
	// the filters and bandwidth while still keyed), then set up receive.
	BK4819_SetupPowerAmplifier(0, 0);
	BK4819_ToggleGpioOut(BK4819_GPIO1_PIN29_PA_ENABLE, false);
	BK4819_ToggleGpioOut(BK4819_GPIO5_PIN1_RED, false);
	// and take the chip out of TX at once (REG_30 still held the TX enables
	// until RADIO_SetupRegisters reached the receiver turn-on)
	BK4819_WriteRegister(BK4819_REG_30, 0);
	gTxCarrierOffMs = g_ms;

	RADIO_SetupRegisters(false);
}
