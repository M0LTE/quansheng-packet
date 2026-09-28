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

// Main screen keys (packet firmware):
//
//   0-9        enter a frequency (band slot) or a channel number (memory)
//   UP/DOWN    step the frequency, or go to the next memory channel
//   MENU       settings menu
//   EXIT       delete the last digit; held: cancel input, monitor off
//   F then 3   switch between frequency and memory channel mode
//   F then 6   cycle the TX power
//   F held     keypad lock on or off
//   SIDE1      monitor (squelch open) on or off
//   SIDE2      no function
//   PTT        transmit (the AIOC normally keys the radio instead)

#include <string.h>

#include "app/app.h"
#include "app/main.h"
#include "driver/bk4819.h"
#include "frequencies.h"
#include "functions.h"
#include "misc.h"
#include "radio.h"
#include "settings.h"
#include "ui/inputbox.h"
#include "ui/ui.h"

void MAIN_ToggleMonitor(void)
{
	if (gCurrentFunction != FUNCTION_MONITOR) { // enable the monitor
		RADIO_SetupRegisters(true);
		APP_StartListening(FUNCTION_MONITOR);
		return;
	}

	gMonitor = false;
	RADIO_SetupRegisters(true);
	gRequestDisplayScreen = gScreenToDisplay;
}

static void TogglePower(void)
{
	if (++gVfo->OUTPUT_POWER > OUTPUT_POWER_HIGH)
		gVfo->OUTPUT_POWER = OUTPUT_POWER_LOW;

	RADIO_ConfigureSquelchAndOutputPower(gVfo);
	gRequestSaveChannel   = true;
	gRequestDisplayScreen = gScreenToDisplay;
}

static void SwitchVfoMode(void)
{
	if (IS_MR_CHANNEL(gVfo->CHANNEL_SAVE))
	{	// swap to frequency mode
		gEeprom.ScreenChannel = gEeprom.FreqChannel;
		gRequestSaveVFO       = true;
		gVfoConfigureMode     = VFO_CONFIGURE_RELOAD;
		return;
	}

	const uint8_t Channel = RADIO_FindNextChannel(gEeprom.MrChannel, RADIO_CHANNEL_UP);
	if (Channel != 0xFF)
	{	// swap to channel mode
		gEeprom.ScreenChannel = Channel;
		gRequestSaveVFO       = true;
		gVfoConfigureMode     = VFO_CONFIGURE_RELOAD;
	}
}

static void ToggleKeypadLock(void)
{
	gEeprom.KEY_LOCK     = !gEeprom.KEY_LOCK;
	gRequestSaveSettings = true;
}

static void MAIN_Key_DIGITS(KEY_Code_t Key, bool bKeyPressed, bool bKeyHeld)
{
	if (bKeyHeld || bKeyPressed)
		return;                                 // use the key when released

	if (gWasFKeyPressed) {
		gWasFKeyPressed = false;
		gUpdateStatus   = true;

		if (Key == KEY_3)
			SwitchVfoMode();
		else if (Key == KEY_6)
			TogglePower();
		return;
	}

	gKeyInputCountdown = key_input_timeout_500ms;
	INPUTBOX_Append(Key);
	gRequestDisplayScreen = DISPLAY_MAIN;

	if (IS_MR_CHANNEL(gVfo->CHANNEL_SAVE)) { // user is entering channel number
		if (gInputBoxIndex != 3)
			return;

		gInputBoxIndex = 0;

		const uint16_t Channel = ((gInputBox[0] * 100) + (gInputBox[1] * 10) + gInputBox[2]) - 1;

		if (!RADIO_CheckValidChannel(Channel))
			return;

		gEeprom.MrChannel     = (uint8_t)Channel;
		gEeprom.ScreenChannel = (uint8_t)Channel;
		gRequestSaveVFO       = true;
		gVfoConfigureMode     = VFO_CONFIGURE_RELOAD;
		return;
	}

	// user is entering a frequency
	const bool isGigaF = gVfo->Frequency >= _1GHz_in_KHz;
	if (gInputBoxIndex < 6 + isGigaF)
		return;

	gInputBoxIndex = 0;
	uint32_t Frequency = StrToUL(INPUTBOX_GetAscii()) * 100;

	// clamp the frequency entered to some valid value
	if (Frequency < frequencyBandTable[0].lower) {
		Frequency = frequencyBandTable[0].lower;
	}
	else if (Frequency >= BX4819_band1.upper && Frequency < BX4819_band2.lower) {
		const uint32_t center = (BX4819_band1.upper + BX4819_band2.lower) / 2;
		Frequency = (Frequency < center) ? BX4819_band1.upper : BX4819_band2.lower;
	}
	else if (Frequency > frequencyBandTable[BAND_N_ELEM - 1].upper) {
		Frequency = frequencyBandTable[BAND_N_ELEM - 1].upper;
	}

	const FREQUENCY_Band_t band = FREQUENCY_GetBand(Frequency);

	if (gVfo->Band != band) {
		// switch to that band's slot first
		gEeprom.ScreenChannel = band + FREQ_CHANNEL_FIRST;
		gEeprom.FreqChannel   = band + FREQ_CHANNEL_FIRST;
		SETTINGS_SaveVfoIndices();
		RADIO_ConfigureChannel();
	}

	Frequency = FREQUENCY_RoundToStep(Frequency, gVfo->StepFrequency);

	if (Frequency >= BX4819_band1.upper && Frequency < BX4819_band2.lower)
	{	// clamp the frequency to the limit
		const uint32_t center = (BX4819_band1.upper + BX4819_band2.lower) / 2;
		Frequency = (Frequency < center) ? BX4819_band1.upper - gVfo->StepFrequency : BX4819_band2.lower;
	}

	gVfo->Frequency     = Frequency;
	gRequestSaveChannel = true;
}

static void MAIN_Key_EXIT(bool bKeyPressed, bool bKeyHeld)
{
	if (!bKeyHeld && bKeyPressed) { // exit key pressed
		if (gInputBoxIndex == 0)
			return;
		gInputBox[--gInputBoxIndex] = 10;
		gKeyInputCountdown = key_input_timeout_500ms;
		gRequestDisplayScreen = DISPLAY_MAIN;
	}
}

static void MAIN_Key_MENU(const bool bKeyPressed, const bool bKeyHeld)
{
	if (bKeyHeld || bKeyPressed)
		return;

	// menu key released
	const bool bFlag = !gInputBoxIndex;
	gInputBoxIndex   = 0;

	if (bFlag) {
		gFlagRefreshSetting   = true;
		gRequestDisplayScreen = DISPLAY_MENU;
	}
	else {
		gRequestDisplayScreen = DISPLAY_MAIN;
	}
}

static void MAIN_Key_F(bool bKeyPressed, bool bKeyHeld)
{
	if (gInputBoxIndex > 0)
		return;

	if (bKeyHeld) {
		if (bKeyPressed)
			ToggleKeypadLock();
		return;
	}

	if (bKeyPressed)
		return;

	// released after a short press
	if (gScreenToDisplay != DISPLAY_MAIN)
		return;

	gWasFKeyPressed = !gWasFKeyPressed; // toggle F function

	if (gWasFKeyPressed)
		gKeyInputCountdown = key_input_timeout_500ms;

	gUpdateStatus = true;
}

static void MAIN_Key_UP_DOWN(bool bKeyPressed, bool bKeyHeld, int8_t Direction)
{
	if (!bKeyPressed)
		return;                   // act on press and on repeat
	if (gInputBoxIndex > 0)
		return;

	const uint8_t Channel = gEeprom.ScreenChannel;

	if (IS_FREQ_CHANNEL(Channel)) { // step up/down in frequency
		const uint32_t frequency = APP_SetFrequencyByStep(gVfo, Direction);

		if (RX_freq_check(frequency) < 0) // frequency not allowed
			return;

		gVfo->Frequency = frequency;
		BK4819_SetFrequency(frequency);
		BK4819_RX_TurnOn();
		gRequestSaveChannel = true;
		(void)bKeyHeld;
		return;
	}

	const uint8_t Next = RADIO_FindNextChannel(Channel + Direction, Direction);
	if (Next == 0xFF || Channel == Next)
		return;

	gEeprom.MrChannel     = Next;
	gEeprom.ScreenChannel = Next;
	gRequestSaveVFO       = true;
	gVfoConfigureMode     = VFO_CONFIGURE_RELOAD;
}

void MAIN_Key_PTT(bool bKeyPressed)
{
	gInputBoxIndex = 0;

	if (!bKeyPressed || SerialConfigInProgress())
	{	// PTT released
		if (gCurrentFunction == FUNCTION_TRANSMIT) {
			// we are transmitting .. stop
			APP_EndTransmission();
			FUNCTION_Select(FUNCTION_FOREGROUND);
			RADIO_SetVfoState(VFO_STATE_NORMAL);

			if (gScreenToDisplay != DISPLAY_MENU)     // 1of11 .. don't close the menu
				gRequestDisplayScreen = DISPLAY_MAIN;
		}

		return;
	}

	// PTT pressed
	if (gCurrentFunction == FUNCTION_TRANSMIT)
		return;                   // already transmitting

	// request start TX
	gFlagPrepareTX      = true;
	gPttDebounceCounter = 0;

	if (gScreenToDisplay != DISPLAY_MENU)     // 1of11 .. don't close the menu
		gRequestDisplayScreen = DISPLAY_MAIN;

	gUpdateStatus  = true;
	gUpdateDisplay = true;
}

void MAIN_ProcessSideKey(KEY_Code_t Key, bool bKeyPressed, bool bKeyHeld)
{
	// act on release after a short press
	if (bKeyPressed || bKeyHeld)
		return;

	if (Key == KEY_SIDE1)
		MAIN_ToggleMonitor();
}

void MAIN_ProcessKeys(KEY_Code_t Key, bool bKeyPressed, bool bKeyHeld)
{
	switch (Key) {
		case KEY_0...KEY_9:
			MAIN_Key_DIGITS(Key, bKeyPressed, bKeyHeld);
			break;
		case KEY_MENU:
			MAIN_Key_MENU(bKeyPressed, bKeyHeld);
			break;
		case KEY_UP:
			MAIN_Key_UP_DOWN(bKeyPressed, bKeyHeld, 1);
			break;
		case KEY_DOWN:
			MAIN_Key_UP_DOWN(bKeyPressed, bKeyHeld, -1);
			break;
		case KEY_EXIT:
			MAIN_Key_EXIT(bKeyPressed, bKeyHeld);
			break;
		case KEY_F:
			MAIN_Key_F(bKeyPressed, bKeyHeld);
			break;
		default:
			break;
	}
}
