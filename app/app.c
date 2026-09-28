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

#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "app/app.h"
#include "app/main.h"
#include "app/menu.h"
#include "app/uart.h"
#include "ARMCM0.h"
#include "audio.h"
#include "board.h"
#include "bsp/dp32g030/gpio.h"
#include "driver/backlight.h"
#include "driver/bk4819.h"
#include "driver/gpio.h"
#include "driver/keyboard.h"
#include "driver/st7565.h"
#include "driver/system.h"
#include "frequencies.h"
#include "functions.h"
#include "helper/battery.h"
#include "misc.h"
#include "packet.h"
#include "radio.h"
#include "settings.h"
#include "ui/battery.h"
#include "ui/inputbox.h"
#include "ui/main.h"
#include "ui/menu.h"
#include "ui/status.h"
#include "ui/ui.h"

static bool flagSaveVfo;
static bool flagSaveSettings;
static bool flagSaveChannel;

static void ProcessKey(KEY_Code_t Key, bool bKeyPressed, bool bKeyHeld);

void (*ProcessKeysFunctions[])(KEY_Code_t Key, bool bKeyPressed, bool bKeyHeld) = {
	[DISPLAY_MAIN] = &MAIN_ProcessKeys,
	[DISPLAY_MENU] = &MENU_ProcessKeys,
};

static_assert(ARRAY_SIZE(ProcessKeysFunctions) == DISPLAY_N_ELEM);

// Carrier squelch only: no CTCSS/DCS, no tail tone, no scanning, no dual
// watch. FOREGROUND -> INCOMING -> RECEIVE while the squelch is open, and
// back to FOREGROUND (with a full receiver set-up) when it closes.

static void CheckForIncoming(void)
{
	if (g_SquelchLost && gCurrentFunction != FUNCTION_INCOMING)
		FUNCTION_Select(FUNCTION_INCOMING);
}

static void HandleIncoming(void)
{
	if (!g_SquelchLost) {	// squelch is closed
		FUNCTION_Select(FUNCTION_FOREGROUND);
		gUpdateDisplay = true;
		return;
	}

	APP_StartListening(gMonitor ? FUNCTION_MONITOR : FUNCTION_RECEIVE);
}

static void HandleReceive(void)
{
	if (g_SquelchLost)
		return;

	// end of reception
	RADIO_SetupRegisters(true);
	gUpdateDisplay = true;
}

static void FunctionNop(void)
{
}

static void (*HandleFunction_fn_table[])(void) = {
	[FUNCTION_FOREGROUND] = &CheckForIncoming,
	[FUNCTION_TRANSMIT]   = &FunctionNop,
	[FUNCTION_MONITOR]    = &FunctionNop,
	[FUNCTION_INCOMING]   = &HandleIncoming,
	[FUNCTION_RECEIVE]    = &HandleReceive,
};

static_assert(ARRAY_SIZE(HandleFunction_fn_table) == FUNCTION_N_ELEM);

void APP_StartListening(FUNCTION_Type_t function)
{
	AUDIO_AudioPathOn();
	gEnableSpeaker = true;

	RADIO_SetRxAudio();

	BK4819_SetAF(BK4819_AF_FM);                  // flat FM demodulator output
	BK4819_SetRegValue(afcDisableRegSpec, 0);    // enable AFC
	BK4819_WriteRegister(BK4819_REG_3D, PKT_REG_3D_RX);

	FUNCTION_Select(function);

	if (function == FUNCTION_MONITOR)
	{	// squelch is disabled
		if (gScreenToDisplay != DISPLAY_MENU)     // 1of11 .. don't close the menu
			GUI_SelectNextDisplay(DISPLAY_MAIN);
	}
	else
		gUpdateDisplay = true;

	gUpdateStatus = true;
}

uint32_t APP_SetFrequencyByStep(VFO_Info_t *pInfo, int8_t direction)
{
	const uint32_t lower = frequencyBandTable[pInfo->Band].lower;
	const uint32_t upper = frequencyBandTable[pInfo->Band].upper;
	uint32_t Frequency = FREQUENCY_RoundToStep(pInfo->Frequency + (direction * pInfo->StepFrequency), pInfo->StepFrequency);

	if (Frequency >= upper)
		Frequency =  lower;
	else if (Frequency < lower)
		Frequency = FREQUENCY_RoundToStep(upper - pInfo->StepFrequency, pInfo->StepFrequency);

	return Frequency;
}

static void CheckRadioInterrupts(void)
{
	while (BK4819_ReadRegister(BK4819_REG_0C) & 1u) { // BK chip interrupt request
		// clear interrupts
		BK4819_WriteRegister(BK4819_REG_02, 0);

		// only the squelch interrupts are enabled (REG_3F)
		const uint16_t interrupts = BK4819_ReadRegister(BK4819_REG_02);

		if (interrupts & BK4819_REG_02_SQUELCH_LOST) {
			g_SquelchLost = true;
			BK4819_ToggleGpioOut(BK4819_GPIO6_PIN2_GREEN, true);
		}

		if (interrupts & BK4819_REG_02_SQUELCH_FOUND) {
			g_SquelchLost = false;
			BK4819_ToggleGpioOut(BK4819_GPIO6_PIN2_GREEN, false);
		}
	}
}

void APP_EndTransmission(void)
{
	// back to RX mode
	gTxTimerCountdown_500ms = 0;
	RADIO_SendEndOfTransmission();

	if (gMonitor) {
		 //turn the monitor back on
		gFlagReconfigureVfos = true;
	}
}

void APP_Update(void)
{
	if (gCurrentFunction == FUNCTION_TRANSMIT && (gTxTimeoutReached || SerialConfigInProgress()))
	{	// transmitter timed out or must de-key
		gTxTimeoutReached = false;

		APP_EndTransmission();
		FUNCTION_Select(FUNCTION_FOREGROUND);

		// PTT must be released before the next transmission
		if (gPttIsPressed)
			gPttWasPressed = true;

		RADIO_SetVfoState(VFO_STATE_TIMEOUT);

		GUI_DisplayScreen();
	}

	if (gReducedService)
		return;

	if (gCurrentFunction != FUNCTION_TRANSMIT)
		HandleFunction_fn_table[gCurrentFunction]();
}

// called every 10ms
static void CheckKeys(void)
{
// -------------------- PTT ------------------------
	if (gPttIsPressed)
	{
		if (GPIO_CheckBit(&GPIOC->DATA, GPIOC_PIN_PTT) || SerialConfigInProgress())
		{	// PTT released or serial comms config in progress
			if (++gPttDebounceCounter >= 3 || SerialConfigInProgress())	    // 30ms
			{	// stop transmitting
				ProcessKey(KEY_PTT, false, false);
				gPttIsPressed = false;
				if (gKeyReading1 != KEY_INVALID)
					gPttWasReleased = true;
			}
		}
		else
			gPttDebounceCounter = 0;
	}
	else if (!GPIO_CheckBit(&GPIOC->DATA, GPIOC_PIN_PTT) && !SerialConfigInProgress())
	{	// PTT pressed
		if (++gPttDebounceCounter >= 3)	    // 30ms
		{	// start transmitting
			boot_counter_10ms   = 0;
			gPttDebounceCounter = 0;
			gPttIsPressed       = true;
			ProcessKey(KEY_PTT, true, false);
		}
	}
	else
		gPttDebounceCounter = 0;

// --------------------- OTHER KEYS ----------------------------

	// scan the hardware keys
	KEY_Code_t Key = KEYBOARD_Poll();

	if (Key != KEY_INVALID) // any key pressed
		boot_counter_10ms = 0;   // cancel boot screen if any key pressed

	if (gKeyReading0 != Key) // new key pressed
	{
		if (gKeyReading0 != KEY_INVALID && Key != KEY_INVALID)
			ProcessKey(gKeyReading1, false, gKeyBeingHeld);  // key pressed without releasing previous key

		gKeyReading0     = Key;
		gDebounceCounter = 0;
		return;
	}

	gDebounceCounter++;

	if (gDebounceCounter == key_debounce_10ms) // debounced new key pressed
	{
		if (Key == KEY_INVALID) //all non PTT keys released
		{
			if (gKeyReading1 != KEY_INVALID) // some button was pressed before
			{
				ProcessKey(gKeyReading1, false, gKeyBeingHeld); // process last button released event
				gKeyReading1 = KEY_INVALID;
			}
		}
		else // process new key pressed
		{
			gKeyReading1 = Key;
			ProcessKey(Key, true, false);
		}

		gKeyBeingHeld = false;
		return;
	}

	if (gDebounceCounter < key_repeat_delay_10ms || Key == KEY_INVALID) // the button is not held long enough for repeat yet, or not really pressed
		return;

	if (gDebounceCounter == key_repeat_delay_10ms) //initial key repeat with longer delay
	{
		if (Key != KEY_PTT)
		{
			gKeyBeingHeld = true;
			ProcessKey(Key, true, true); // key held event
		}
	}
	else //subsequent fast key repeats
	{
		if (Key == KEY_UP || Key == KEY_DOWN) // fast key repeats for up/down buttons
		{
			gKeyBeingHeld = true;
			if ((gDebounceCounter % key_repeat_10ms) == 0)
				ProcessKey(Key, true, true); // key held event
		}

		if (gDebounceCounter < 0xFFFF)
			return;

		gDebounceCounter = key_repeat_delay_10ms+1;
	}
}

void APP_TimeSlice10ms(void)
{
	gNextTimeslice = false;

	if (UART_IsCommandAvailable()) {
		__disable_irq();
		UART_HandleCommand();
		__enable_irq();
	}

	if (gReducedService)
		return;

	CheckRadioInterrupts();

	if (gUpdateDisplay) {
		gUpdateDisplay = false;
		GUI_DisplayScreen();
	}

	if (gUpdateStatus)
		UI_DisplayStatus();

	CheckKeys();
}

static void cancelUserInputModes(void)
{
	if (gWasFKeyPressed || gKeyInputCountdown > 0 || gInputBoxIndex > 0)
	{
		gWasFKeyPressed     = false;
		gInputBoxIndex      = 0;
		gKeyInputCountdown  = 0;
		gUpdateStatus       = true;
		gUpdateDisplay      = true;
	}
}

// this is called once every 500ms
void APP_TimeSlice500ms(void)
{
	gNextTimeslice_500ms = false;

	if (gKeypadLocked > 0)
		if (--gKeypadLocked == 0)
			gUpdateDisplay = true;

	if (gKeyInputCountdown > 0)
		if (--gKeyInputCountdown == 0)
			cancelUserInputModes();

	if (gMenuCountdown > 0 && --gMenuCountdown == 0 && gScreenToDisplay == DISPLAY_MENU)
	{	// exit menu mode
		gInputBoxIndex = 0;
		gWasFKeyPressed = false;
		gUpdateStatus  = true;
		gUpdateDisplay = true;
		GUI_SelectNextDisplay(DISPLAY_MAIN);
	}

	if (gBacklightCountdown_500ms > 0 && --gBacklightCountdown_500ms == 0 && gEeprom.BACKLIGHT_TIME < 7)
		BACKLIGHT_TurnOff();

	if (gReducedService)
	{
		BOARD_ADC_GetBatteryInfo(&gBatteryCurrentVoltage, &gBatteryCurrent);

		if (gBatteryCurrent > 500 || gBatteryCalibration[3] < gBatteryCurrentVoltage)
			NVIC_SystemReset();

		return;
	}

	gBatteryCheckCounter++;

	if (gCurrentFunction != FUNCTION_TRANSMIT)
	{
		if ((gBatteryCheckCounter & 1) == 0)
		{
			BOARD_ADC_GetBatteryInfo(&gBatteryVoltages[gBatteryVoltageIndex++], &gBatteryCurrent);
			if (gBatteryVoltageIndex > 3)
				gBatteryVoltageIndex = 0;
			BATTERY_GetReadings(true);
		}
	}

	// regular status updates (once every 2 sec)
	if ((gBatteryCheckCounter & 3) == 0)
		gUpdateStatus = true;

	if (gReloadSettingsAfterSerial && !SerialConfigInProgress() && gCurrentFunction != FUNCTION_TRANSMIT)
	{	// EEPROM was written over UART: use the new settings and channel data
		gReloadSettingsAfterSerial = false;
		SETTINGS_InitEEPROM();
		SETTINGS_LoadCalibration();
		RADIO_ConfigureChannel();
		RADIO_SetupRegisters(true);
		gMonitor       = false;
		// an open menu item would otherwise store its old value on MENU
		gIsInSubMenu   = false;
		if (gScreenToDisplay == DISPLAY_MENU)
			MENU_ShowCurrentSetting();
		gUpdateStatus  = true;
		gUpdateDisplay = true;
	}

	if (!gPttIsPressed && gVFOStateResumeCountdown_500ms > 0 && --gVFOStateResumeCountdown_500ms == 0)
		RADIO_SetVfoState(VFO_STATE_NORMAL);

	BATTERY_TimeSlice500ms();
	UI_MAIN_TimeSlice500ms();
}

// Saves postponed while an UP/DOWN key was held. EEPROM writes and the
// receiver set-up that follows them never happen while transmitting: they
// wait until the transmission has ended.
static void FlushHeldKeySaves(void)
{
	if (gCurrentFunction == FUNCTION_TRANSMIT)
		return;

	if (flagSaveVfo) {
		SETTINGS_SaveVfoIndices();
		flagSaveVfo = false;
	}

	if (flagSaveSettings) {
		SETTINGS_SaveSettings();
		flagSaveSettings = false;
	}

	if (flagSaveChannel) {
		SETTINGS_SaveChannel(gVfo);
		flagSaveChannel = false;

		if (gVfoConfigureMode == VFO_CONFIGURE_NONE)
			gVfoConfigureMode = VFO_CONFIGURE;
	}
}

static void ProcessKey(KEY_Code_t Key, bool bKeyPressed, bool bKeyHeld)
{
	if (Key == KEY_EXIT && !BACKLIGHT_IsOn() && gEeprom.BACKLIGHT_TIME > 0)
	{	// just turn the light on for now so the user can see what's what
		BACKLIGHT_TurnOn();
		return;
	}

	if (!bKeyPressed) { // key released
		FlushHeldKeySaves();
	}
	else { // key pressed or held
		if (Key != KEY_PTT)
			BACKLIGHT_TurnOn();

		if (Key == KEY_EXIT && bKeyHeld) { // exit key held pressed
			cancelUserInputModes();

			if (gMonitor && gCurrentFunction != FUNCTION_TRANSMIT)
				MAIN_ToggleMonitor(); //turn off the monitor
		}

		if (gScreenToDisplay == DISPLAY_MENU)       // 1of11
			gMenuCountdown = menu_timeout_500ms;
	}

	bool lowBatPopup = gLowBattery && !gLowBatteryConfirmed &&  gScreenToDisplay == DISPLAY_MAIN;

	if ((gEeprom.KEY_LOCK || lowBatPopup) && gCurrentFunction != FUNCTION_TRANSMIT && Key != KEY_PTT)
	{	// keyboard is locked or low battery popup

		// close low battery popup
		if(Key == KEY_EXIT && bKeyPressed && lowBatPopup) {
			gLowBatteryConfirmed = true;
			gUpdateDisplay = true;
			return;
		}

		if (Key == KEY_F) { // function/key-lock key
			if (!bKeyPressed)
				return;

			if (!bKeyHeld) { // keypad is locked, tell the user
				gKeypadLocked  = 4;      // 2 seconds
				gUpdateDisplay = true;
				return;
			}
		}
		else if (Key != KEY_SIDE1 && Key != KEY_SIDE2) // pass side buttons
		{
			if (bKeyPressed && !bKeyHeld) {
				// keypad is locked, tell the user
				gKeypadLocked  = 4;          // 2 seconds
				gUpdateDisplay = true;
			}
			return;
		}
	}

	bool bFlag = false;
	if (Key == KEY_PTT) {
		if (gPttWasPressed) {
			bFlag = bKeyHeld;
			if (!bKeyPressed) {
				bFlag          = true;
				gPttWasPressed = false;
			}
		}
	}
	else if (gPttWasReleased) {
		if (bKeyHeld)
			bFlag = true;
		if (!bKeyPressed) {
			bFlag           = true;
			gPttWasReleased = false;
		}
	}

	if (gWasFKeyPressed && (Key == KEY_PTT || Key == KEY_EXIT || Key == KEY_SIDE1 || Key == KEY_SIDE2)) {
		// cancel the F-key
		gWasFKeyPressed = false;
		gUpdateStatus   = true;
	}

	if (bFlag) {
		goto Skip;
	}

	if (gCurrentFunction == FUNCTION_TRANSMIT) {
		// only PTT matters while transmitting
		if (Key == KEY_PTT)
			MAIN_Key_PTT(bKeyPressed);
		goto Skip;
	}

	if (Key == KEY_PTT)
		MAIN_Key_PTT(bKeyPressed);
	else if (Key == KEY_SIDE1 || Key == KEY_SIDE2)
		MAIN_ProcessSideKey(Key, bKeyPressed, bKeyHeld);
	else if (gScreenToDisplay != DISPLAY_INVALID)
		ProcessKeysFunctions[gScreenToDisplay](Key, bKeyPressed, bKeyHeld);

Skip:
	// Nothing below writes EEPROM or touches the receiver while
	// transmitting; the requests stay pending until TX has ended (the PTT
	// release that ends it comes through here too).
	if (gCurrentFunction != FUNCTION_TRANSMIT) {
		if (!bKeyPressed)
			FlushHeldKeySaves();

		if (gFlagAcceptSetting) {
			gMenuCountdown = menu_timeout_500ms;

			MENU_AcceptSetting();

			gFlagRefreshSetting = true;
			gFlagAcceptSetting  = false;
		}

		if (gRequestSaveSettings) {
			if (!bKeyHeld)
				SETTINGS_SaveSettings();
			else
				flagSaveSettings = true;
			gRequestSaveSettings = false;
			gUpdateStatus        = true;
		}

		if (gRequestSaveVFO) {
			gRequestSaveVFO = false;
			if (!bKeyHeld)
				SETTINGS_SaveVfoIndices();
			else
				flagSaveVfo = true;
		}

		if (gRequestSaveChannel) {
			if (!bKeyHeld) {
				SETTINGS_SaveChannel(gVfo);

				if (gVfoConfigureMode == VFO_CONFIGURE_NONE)
					gVfoConfigureMode = VFO_CONFIGURE;
			}
			else { // save when the up/down button is released
				flagSaveChannel = true;

				if (gRequestDisplayScreen == DISPLAY_INVALID)
					gRequestDisplayScreen = DISPLAY_MAIN;
			}

			gRequestSaveChannel = false;
		}

		if (gVfoConfigureMode != VFO_CONFIGURE_NONE) {
			RADIO_ConfigureChannel();

			if (gRequestDisplayScreen == DISPLAY_INVALID)
				gRequestDisplayScreen = DISPLAY_MAIN;

			gFlagReconfigureVfos = true;
			gVfoConfigureMode    = VFO_CONFIGURE_NONE;
		}

		if (gFlagReconfigureVfos) {
			RADIO_SetupRegisters(true);

			gFlagReconfigureVfos = false;

			if (gMonitor)
				MAIN_ToggleMonitor();   // 1of11
		}
	}

	if (gFlagRefreshSetting) {
		gFlagRefreshSetting = false;
		gMenuCountdown      = menu_timeout_500ms;

		MENU_ShowCurrentSetting();
	}

	if (gFlagPrepareTX) {
		RADIO_PrepareTX();
		gFlagPrepareTX = false;
	}

	GUI_SelectNextDisplay(gRequestDisplayScreen);
	gRequestDisplayScreen = DISPLAY_INVALID;

	gUpdateDisplay = true;
}
