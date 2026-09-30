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

// Menu keys: UP/DOWN move through the list, or change the value once MENU
// has opened it; MENU again stores the value, EXIT cancels. The value takes
// effect at once and is saved to EEPROM.

#include <string.h>

#include "app/menu.h"
#include "driver/backlight.h"
#include "frequencies.h"
#include "misc.h"
#include "packet.h"
#include "radio.h"
#include "settings.h"
#include "ui/menu.h"
#include "ui/ui.h"

// returns false for a read-only item
static bool MENU_GetLimits(uint8_t menu_id, int32_t *pMin, int32_t *pMax)
{
	*pMin = 0;
	switch (menu_id)
	{
		case MENU_STEP:   *pMax = STEP_N_ELEM - 1;                     break;
		case MENU_TXP:    *pMax = OUTPUT_POWER_HIGH;                   break;
		case MENU_W_N:    *pMax = BANDWIDTH_NARROW;                    break;
		case MENU_DEVW:
		case MENU_DEVN:   *pMax = PKT_DEVIATION_MAX;                   break;
		case MENU_RXG:    *pMax = PKT_RX_GAIN_MAX;                     break;
		case MENU_RXDAC:  *pMax = PKT_RX_DAC_GAIN_MAX;                 break;
		case MENU_TOT:    *pMax = ARRAY_SIZE(gTxTimeoutSeconds) - 1;   break;
		case MENU_ABR:    *pMax = ARRAY_SIZE(gSubMenu_BACKLIGHT) - 1;  break;
		case MENU_BATTYP: *pMax = BATTERY_TYPE_2200_MAH;               break;
		default:
			*pMax = 0;
			return false;
	}
	return true;
}

void MENU_AcceptSetting(void)
{
	int32_t Min;
	int32_t Max;

	if (!MENU_GetLimits(gMenuCursor, &Min, &Max))
		return;

	if (gSubMenuSelection < Min) gSubMenuSelection = Min;
	if (gSubMenuSelection > Max) gSubMenuSelection = Max;

	switch (gMenuCursor)
	{
		// the operating channel (settings.h, 0x1D58)
		case MENU_STEP:
			gVfo->STEP_SETTING    = FREQUENCY_GetStepIdxFromSortedIdx(gSubMenuSelection);
			gVfo->StepFrequency   = gStepFrequencyTable[gVfo->STEP_SETTING];
			gRequestSaveOperating = true;
			return;

		case MENU_TXP:
			gVfo->OUTPUT_POWER    = gSubMenuSelection;
			gRequestSaveOperating = true;
			return;

		case MENU_W_N:
			gVfo->CHANNEL_BANDWIDTH = gSubMenuSelection;
			gRequestSaveOperating   = true;
			return;

		// radio wide
		case MENU_DEVW:   gEeprom.DEVIATION_WIDE   = gSubMenuSelection; break;
		case MENU_DEVN:   gEeprom.DEVIATION_NARROW = gSubMenuSelection; break;
		case MENU_RXG:    gEeprom.RX_GAIN          = gSubMenuSelection; break;
		case MENU_RXDAC:  gEeprom.RX_DAC_GAIN      = gSubMenuSelection; break;
		case MENU_TOT:    gEeprom.TX_TIMEOUT       = gSubMenuSelection; break;
		case MENU_BATTYP: gEeprom.BATTERY_TYPE     = gSubMenuSelection; break;
		case MENU_ABR:
			gEeprom.BACKLIGHT_TIME = gSubMenuSelection;
			BACKLIGHT_TurnOn();
			break;
		default:
			return;
	}

	gRequestSaveSettings = true;
	gVfoConfigureMode    = VFO_CONFIGURE;   // gains: set the chip up again
}

void MENU_ShowCurrentSetting(void)
{
	switch (gMenuCursor)
	{
		case MENU_STEP:   gSubMenuSelection = FREQUENCY_GetSortedIdxFromStepIdx(gVfo->STEP_SETTING); break;
		case MENU_TXP:    gSubMenuSelection = gVfo->OUTPUT_POWER;       break;
		case MENU_W_N:    gSubMenuSelection = gVfo->CHANNEL_BANDWIDTH;  break;
		case MENU_DEVW:   gSubMenuSelection = gEeprom.DEVIATION_WIDE;   break;
		case MENU_DEVN:   gSubMenuSelection = gEeprom.DEVIATION_NARROW; break;
		case MENU_RXG:    gSubMenuSelection = gEeprom.RX_GAIN;          break;
		case MENU_RXDAC:  gSubMenuSelection = gEeprom.RX_DAC_GAIN;      break;
		case MENU_TOT:    gSubMenuSelection = gEeprom.TX_TIMEOUT;       break;
		case MENU_ABR:    gSubMenuSelection = gEeprom.BACKLIGHT_TIME;   break;
		case MENU_BATTYP: gSubMenuSelection = gEeprom.BATTERY_TYPE;     break;
		default:          gSubMenuSelection = 0;                        break;
	}
}

static void MENU_Key_UP_DOWN(bool bKeyPressed, bool bKeyHeld, int8_t Direction)
{
	if (!bKeyPressed)
		return;

	if (!gIsInSubMenu) {
		gMenuCursor = NUMBER_AddWithWraparound(gMenuCursor, -Direction, 0, MENU_N_ITEMS - 1);
		gFlagRefreshSetting = true;
		gRequestDisplayScreen = DISPLAY_MENU;
		return;
	}

	int32_t Min;
	int32_t Max;
	if (!MENU_GetLimits(gMenuCursor, &Min, &Max))
		return;

	// the deviation has 4096 values: move 16 at a time while the key is held
	const int32_t step = (bKeyHeld && (gMenuCursor == MENU_DEVW || gMenuCursor == MENU_DEVN)) ? 16 : 1;
	gSubMenuSelection = NUMBER_AddWithWraparound(gSubMenuSelection, Direction * step, Min, Max);
	gRequestDisplayScreen = DISPLAY_MENU;
}

static void MENU_Key_MENU(bool bKeyPressed, bool bKeyHeld)
{
	if (bKeyHeld || !bKeyPressed)
		return;

	gRequestDisplayScreen = DISPLAY_MENU;

	int32_t Min;
	int32_t Max;
	if (!gIsInSubMenu) {
		if (MENU_GetLimits(gMenuCursor, &Min, &Max))   // read-only items do not open
			gIsInSubMenu = true;
		return;
	}

	gFlagAcceptSetting = true;
	gIsInSubMenu       = false;
}

static void MENU_Key_EXIT(bool bKeyPressed, bool bKeyHeld)
{
	if (bKeyHeld || !bKeyPressed)
		return;

	if (gIsInSubMenu) {
		gIsInSubMenu        = false;
		gFlagRefreshSetting = true;   // back to the stored value
		gRequestDisplayScreen = DISPLAY_MENU;
		return;
	}

	gRequestDisplayScreen = DISPLAY_MAIN;
}

void MENU_ProcessKeys(KEY_Code_t Key, bool bKeyPressed, bool bKeyHeld)
{
	switch (Key)
	{
		case KEY_MENU:
			MENU_Key_MENU(bKeyPressed, bKeyHeld);
			break;
		case KEY_UP:
			MENU_Key_UP_DOWN(bKeyPressed, bKeyHeld,  1);
			break;
		case KEY_DOWN:
			MENU_Key_UP_DOWN(bKeyPressed, bKeyHeld, -1);
			break;
		case KEY_EXIT:
			MENU_Key_EXIT(bKeyPressed, bKeyHeld);
			break;
		default:
			break;
	}

	if (gScreenToDisplay == DISPLAY_MENU && gMenuCursor == MENU_VOL)
		gUpdateDisplay = true;
}
