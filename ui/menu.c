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

#include "../app/menu.h"
#include "../driver/st7565.h"
#include "../external/printf/printf.h"
#include "../frequencies.h"
#include "../helper/battery.h"
#include "../misc.h"
#include "../settings.h"
#include "../version.h"
#include "helper.h"
#include "menu.h"
#include "ui.h"

const char gMenuNames[MENU_N_ITEMS][7] =
{
	[MENU_STEP]   = "Step",
	[MENU_TXP]    = "TxPwr",
	[MENU_W_N]    = "W/N",
	[MENU_MIC]    = "MicG",
	[MENU_DEVW]   = "DevW",
	[MENU_DEVN]   = "DevN",
	[MENU_RXG]    = "RxG",
	[MENU_RXDAC]  = "RxDAC",
	[MENU_TOT]    = "TxTOut",
	[MENU_ABR]    = "BackLt",
	[MENU_BATTYP] = "BatTyp",
	[MENU_VOL]    = "BatVol",
	[MENU_VER]    = "Ver",
};

const char gSubMenu_BACKLIGHT[][7] =
{
	"OFF",
	"5 sec",
	"10 sec",
	"20 sec",
	"1 min",
	"2 min",
	"4 min",
	"ON"
};

bool    gIsInSubMenu;
uint8_t gMenuCursor;
int32_t gSubMenuSelection;

void UI_DisplayMenu(void)
{
	const unsigned int list_x  = 0;
	const unsigned int value_x = 50;
	char String[24];
	char Hint[24];      // small font: 11 characters fit right of the list

	UI_DisplayClear();

	// the menu list, current item in the middle of 7 lines, inverted
	for (int i = -3; i <= 3; i++) {
		const int item = (int)gMenuCursor + i;
		if (item < 0 || item >= MENU_N_ITEMS)
			continue;
		UI_PrintStringSmallNormal(gMenuNames[item], list_x + 1, 0, 3 + i);
		if (i == 0)
			for (unsigned int x = list_x; x < value_x - 4; x++)
				gFrameBuffer[3][x] ^= 0x7F;
	}

	String[0] = 0;
	Hint[0]   = 0;

	switch (gMenuCursor)
	{
		case MENU_STEP: {
			const uint16_t step = gStepFrequencyTable[FREQUENCY_GetStepIdxFromSortedIdx(gSubMenuSelection)];
			sprintf(String, "%d.%02u", step / 100, step % 100);
			strcpy(Hint, "kHz");
			break;
		}

		case MENU_TXP:
			strcpy(String, gPowerNames[gSubMenuSelection % 3]);
			strcpy(Hint, "nominal");
			break;

		case MENU_W_N:
			strcpy(String, gSubMenuSelection ? "NARROW" : "WIDE");
			break;

		case MENU_MIC:
			sprintf(String, "%d", (int)gSubMenuSelection);
			sprintf(Hint, "+%d.%ddB", (int)gSubMenuSelection / 2, ((int)gSubMenuSelection % 2) * 5);
			break;

		case MENU_DEVW:
		case MENU_DEVN:
			sprintf(String, "%d", (int)gSubMenuSelection);
			sprintf(Hint, "0x%03X", (unsigned int)gSubMenuSelection);
			break;

		case MENU_RXG: {
			// AF RX gain 2: -26 dB to +5.5 dB in 0.5 dB steps
			const int tenths = (int)gSubMenuSelection * 5 - 260;
			sprintf(String, "%d", (int)gSubMenuSelection);
			sprintf(Hint, "%s%d.%ddB", tenths < 0 ? "-" : "+", (tenths < 0 ? -tenths : tenths) / 10, (tenths < 0 ? -tenths : tenths) % 10);
			break;
		}

		case MENU_RXDAC:
			sprintf(String, "%d", (int)gSubMenuSelection);
			strcpy(Hint, "2dB steps");
			break;

		case MENU_TOT:
			sprintf(String, "%u sec", gTxTimeoutSeconds[gSubMenuSelection % ARRAY_SIZE(gTxTimeoutSeconds)]);
			break;

		case MENU_ABR:
			strcpy(String, gSubMenu_BACKLIGHT[gSubMenuSelection % ARRAY_SIZE(gSubMenu_BACKLIGHT)]);
			break;

		case MENU_BATTYP:
			strcpy(String, gSubMenuSelection ? "2200mAh" : "1600mAh");
			break;

		case MENU_VOL:
			sprintf(String, "%u.%02uV", gBatteryVoltageAverage / 100, gBatteryVoltageAverage % 100);
			sprintf(Hint, "%u%%", BATTERY_VoltsToPercent(gBatteryVoltageAverage));
			break;

		case MENU_VER:
		{	// "PKTFW <git hash>": the hash in the big font
			const char *hash = strchr(Version, ' ');
			strncpy(String, hash ? hash + 1 : Version, 8);
			String[8] = 0;
			strcpy(Hint, "PKTFW");
		}
			break;
	}

	UI_PrintString(String, value_x, 0, 1, 8);
	UI_PrintStringSmallNormal(Hint, value_x, 0, 4);

	// the edit cursor, in the gap between the list and the value: the
	// small ">" is inked in its columns 1 to 4 and drawn one column in,
	// so from value_x - 7 it covers value_x - 5 to value_x - 2 and leaves
	// value_x - 1 clear before the value (the list names end by x 42)
	if (gIsInSubMenu)
		UI_PrintStringSmallNormal(">", value_x - 7, 0, 1);

	sprintf(String, "%u/%u", gMenuCursor + 1, MENU_N_ITEMS);
	UI_PrintStringSmallNormal(String, value_x, 0, 6);

	ST7565_BlitFullScreen();
}
