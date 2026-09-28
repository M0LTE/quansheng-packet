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
#include <string.h>

#include "app/action.h"
#include "app/app.h"
#include "app/chFrScanner.h"
#include "app/common.h"
#include "app/dtmf.h"
#include "app/scanner.h"
#include "audio.h"
#include "bsp/dp32g030/gpio.h"
#include "driver/bk4819.h"
#include "driver/gpio.h"
#include "driver/backlight.h"
#include "functions.h"
#include "misc.h"
#include "settings.h"
#include "ui/inputbox.h"
#include "ui/ui.h"




inline static void ACTION_ScanRestart() { ACTION_Scan(true); };

void (*action_opt_table[])(void) = {
	[ACTION_OPT_NONE] = &FUNCTION_NOP,
	[ACTION_OPT_POWER] = &ACTION_Power,
	[ACTION_OPT_MONITOR] = &ACTION_Monitor,
	[ACTION_OPT_SCAN] = &ACTION_ScanRestart,
	[ACTION_OPT_KEYLOCK] = &COMMON_KeypadLockToggle,
	[ACTION_OPT_A_B] = &COMMON_SwitchVFOs,
	[ACTION_OPT_VFO_MR] = &COMMON_SwitchVFOMode,
	[ACTION_OPT_SWITCH_DEMODUL] = &ACTION_SwitchDemodul,

	[ACTION_OPT_FLASHLIGHT] = &FUNCTION_NOP,

	[ACTION_OPT_VOX] = &FUNCTION_NOP,

	[ACTION_OPT_FM] = &FUNCTION_NOP,

	[ACTION_OPT_ALARM] = &FUNCTION_NOP,

	[ACTION_OPT_1750] = &FUNCTION_NOP,

	[ACTION_OPT_BLMIN_TMP_OFF] = &FUNCTION_NOP,

	[ACTION_OPT_SPECTRUM] = &FUNCTION_NOP,
};

static_assert(ARRAY_SIZE(action_opt_table) == ACTION_OPT_LEN);

void ACTION_Power(void)
{
	if (++gTxVfo->OUTPUT_POWER > OUTPUT_POWER_HIGH)
		gTxVfo->OUTPUT_POWER = OUTPUT_POWER_LOW;

	gRequestSaveChannel = 1;

	gRequestDisplayScreen = gScreenToDisplay;


}

void ACTION_Monitor(void)
{
	if (gCurrentFunction != FUNCTION_MONITOR) { // enable the monitor
		RADIO_SelectVfos();
		RADIO_SetupRegisters(true);
		APP_StartListening(FUNCTION_MONITOR);
		return;
	}

	gMonitor = false;

	if (gScanStateDir != SCAN_OFF) {
		gScanPauseDelayIn_10ms = scan_pause_delay_in_1_10ms;
		gScheduleScanListen    = false;
		gScanPauseMode         = true;
	}


	RADIO_SetupRegisters(true);

		gRequestDisplayScreen = gScreenToDisplay;
}

void ACTION_Scan(bool bRestart)
{
	(void)bRestart;


	if (SCANNER_IsScanning()) {
		return;
	}

	// not scanning
	gMonitor = false;

	gDTMF_RX_live_timeout = 0;
	memset(gDTMF_RX_live, 0, sizeof(gDTMF_RX_live));

	RADIO_SelectVfos();


	GUI_SelectNextDisplay(DISPLAY_MAIN);

	if (gScanStateDir != SCAN_OFF) {
		// already scanning

		if (!IS_MR_CHANNEL(gNextMrChannel)) {
			CHFRSCANNER_Stop();
			return;
		}

		// channel mode. Keep scanning but toggle between scan lists
		gEeprom.SCAN_LIST_DEFAULT = (gEeprom.SCAN_LIST_DEFAULT + 1) % 3;

		// jump to the next channel
		CHFRSCANNER_Start(false, gScanStateDir);
		gScanPauseDelayIn_10ms = 1;
		gScheduleScanListen    = false;
	} else {
		// start scanning
		CHFRSCANNER_Start(true, SCAN_FWD);


		// clear the other vfo's rssi level (to hide the antenna symbol)
		gVFO_RSSI_bar_level[(gEeprom.RX_VFO + 1) & 1U] = 0;

		// let the user see DW is not active
		gDualWatchActive = false;
	}

	gUpdateStatus = true;
}


void ACTION_SwitchDemodul(void)
{
	gRequestSaveChannel = 1;

	gTxVfo->Modulation++;

	if(gTxVfo->Modulation == MODULATION_UKNOWN)
		gTxVfo->Modulation = MODULATION_FM;
}


void ACTION_Handle(KEY_Code_t Key, bool bKeyPressed, bool bKeyHeld)
{
	if (gScreenToDisplay == DISPLAY_MAIN && gDTMF_InputMode){
		 // entering DTMF code

		gPttWasReleased = true;

		if (Key != KEY_SIDE1 || bKeyHeld || !bKeyPressed){
			return;
		}

		// side1 btn pressed

		gBeepToPlay = BEEP_1KHZ_60MS_OPTIONAL;
		gRequestDisplayScreen = DISPLAY_MAIN;

		if (gDTMF_InputBox_Index <= 0) {
			// turn off DTMF input box if no codes left
			gDTMF_InputMode = false;
			return;
		}

		// DTMF codes are in the input box
		gDTMF_InputBox[--gDTMF_InputBox_Index] = '-'; // delete one code

		return;
	}

	enum ACTION_OPT_t funcShort = ACTION_OPT_NONE;
	enum ACTION_OPT_t funcLong  = ACTION_OPT_NONE;
	switch(Key) {
		case KEY_SIDE1:
			funcShort = gEeprom.KEY_1_SHORT_PRESS_ACTION;
			funcLong  = gEeprom.KEY_1_LONG_PRESS_ACTION;
			break;
		case KEY_SIDE2:
			funcShort = gEeprom.KEY_2_SHORT_PRESS_ACTION;
			funcLong  = gEeprom.KEY_2_LONG_PRESS_ACTION;
			break;
		case KEY_MENU:
			funcLong  = gEeprom.KEY_M_LONG_PRESS_ACTION;
			break;
		default:
			break;
	}

	if (!bKeyHeld && bKeyPressed) // button pushed
	{
		return;
	}

	// held or released beyond this point

	if(!(bKeyHeld && !bKeyPressed)) // don't beep on released after hold
		gBeepToPlay = BEEP_1KHZ_60MS_OPTIONAL;

	if (bKeyHeld || bKeyPressed) // held
	{
		funcShort = funcLong;

		if (!bKeyPressed) //ignore release if held
			return;
	}

	// held or released after short press beyond this point

	action_opt_table[funcShort]();
}






