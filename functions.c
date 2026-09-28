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

#include "app/dtmf.h"
#include "audio.h"
#include "bsp/dp32g030/gpio.h"
#include "dcs.h"
#include "driver/backlight.h"
#include "driver/bk4819.h"
#include "driver/gpio.h"
#include "driver/system.h"
#include "driver/st7565.h"
#include "frequencies.h"
#include "functions.h"
#include "helper/battery.h"
#include "misc.h"
#include "radio.h"
#include "settings.h"
#include "ui/status.h"
#include "ui/ui.h"

FUNCTION_Type_t gCurrentFunction;

bool FUNCTION_IsRx()
{
	return gCurrentFunction == FUNCTION_MONITOR ||
		   gCurrentFunction == FUNCTION_INCOMING ||
		   gCurrentFunction == FUNCTION_RECEIVE;
}

void FUNCTION_Init(void)
{
	g_CxCSS_TAIL_Found = false;
	g_CDCSS_Lost       = false;
	g_CTCSS_Lost       = false;

	g_SquelchLost      = false;

	gFlagTailToneEliminationComplete   = false;
	gTailToneEliminationCountdown_10ms = 0;
	gFoundCTCSS                        = false;
	gFoundCDCSS                        = false;
	gFoundCTCSSCountdown_10ms          = 0;
	gFoundCDCSSCountdown_10ms          = 0;
	gEndOfRxDetectedMaybe              = false;

	gCurrentCodeType = (gRxVfo->Modulation != MODULATION_FM) ? CODE_TYPE_OFF : gRxVfo->pRX->CodeType;




	gUpdateStatus = true;
}

void FUNCTION_Foreground(const FUNCTION_Type_t PreviousFunction)
{

	if (PreviousFunction == FUNCTION_TRANSMIT) {
		ST7565_FixInterfGlitch();
		gVFO_RSSI_bar_level[0] = 0;
		gVFO_RSSI_bar_level[1] = 0;
	} else if (PreviousFunction != FUNCTION_RECEIVE) {
		return;
	}


	gUpdateStatus = true;
}

void FUNCTION_PowerSave() {
	gPowerSave_10ms = gEeprom.BATTERY_SAVE * 10;
	gPowerSaveCountdownExpired = false;

	gRxIdleMode = true;

	gMonitor = false;

	BK4819_DisableVox();
	BK4819_Sleep();

	BK4819_ToggleGpioOut(BK4819_GPIO0_PIN28_RX_ENABLE, false);

	gUpdateStatus = true;

	if (gScreenToDisplay != DISPLAY_MENU)     // 1of11 .. don't close the menu
		GUI_SelectNextDisplay(DISPLAY_MAIN);
}

void FUNCTION_Transmit()
{
	// if DTMF is enabled when TX'ing, it changes the TX audio filtering !! .. 1of11
	BK4819_DisableDTMF();


	// clear the DTMF RX live decoder buffer
	gDTMF_RX_live_timeout = 0;
	memset(gDTMF_RX_live, 0, sizeof(gDTMF_RX_live));



	gUpdateStatus = true;

	GUI_DisplayScreen();

	RADIO_SetTxParameters();

	// turn the RED LED on
	BK4819_ToggleGpioOut(BK4819_GPIO5_PIN1_RED, true);

	DTMF_Reply();

	if (gCurrentVfo->DTMF_PTT_ID_TX_MODE == PTT_ID_APOLLO)
		BK4819_PlaySingleTone(2525, 250, 0, gEeprom.DTMF_SIDE_TONE);


	if (gCurrentVfo->SCRAMBLING_TYPE > 0 && gSetting_ScrambleEnable)
		BK4819_EnableScramble(gCurrentVfo->SCRAMBLING_TYPE - 1);
	else
		BK4819_DisableScramble();

	if (gSetting_backlight_on_tx_rx & BACKLIGHT_ON_TR_TX) {
		BACKLIGHT_TurnOn();
	}
}



void FUNCTION_Select(FUNCTION_Type_t Function)
{
	const FUNCTION_Type_t PreviousFunction = gCurrentFunction;
	const bool bWasPowerSave = PreviousFunction == FUNCTION_POWER_SAVE;

	gCurrentFunction = Function;

	if (bWasPowerSave && Function != FUNCTION_POWER_SAVE) {
		BK4819_Conditional_RX_TurnOn_and_GPIO6_Enable();
		gRxIdleMode = false;
		UI_DisplayStatus();
	}

	switch (Function) {
		case FUNCTION_FOREGROUND:
			FUNCTION_Foreground(PreviousFunction);
			return;

		case FUNCTION_POWER_SAVE:
			FUNCTION_PowerSave();
			return;

		case FUNCTION_TRANSMIT:
			FUNCTION_Transmit();
			break;

		case FUNCTION_MONITOR:
			gMonitor = true;
			break;

		case FUNCTION_INCOMING:
		case FUNCTION_RECEIVE:
		case FUNCTION_BAND_SCOPE:
		default:
			break;
	}

	gBatterySaveCountdown_10ms = battery_save_count_10ms;
	gSchedulePowerSave         = false;

}
