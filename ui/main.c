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

// Main screen (packet firmware):
//
//   lines 0-1  TX, or RX while a carrier is detected, and the frequency
//   line  3    RSSI in dBm and S-units while receiving
//   lines 4-6  the packet settings in use: power (nominal watts),
//              bandwidth, TX timeout, deviation, receive gains

#include <string.h>

#include "bitmaps.h"
#include "driver/bk4819.h"
#include "driver/st7565.h"
#include "external/printf/printf.h"
#include "functions.h"
#include "app/monitor.h"
#include "helper/battery.h"
#include "misc.h"
#include "radio.h"
#include "settings.h"
#include "ui/helper.h"
#include "ui/inputbox.h"
#include "ui/main.h"
#include "ui/ui.h"

// S-meter scale, -dBm at S0 and at S9: fixed (the upstream defaults). Other
// firmwares keep their own at EEPROM 0x0EA0; this one never reads it.
#define SMETER_S0_LEVEL  130
#define SMETER_S9_LEVEL  76

static const int8_t dBmCorrTable[7] = {
			-15, // band 1
			-25, // band 2
			-20, // band 3
			-4, // band 4
			-7, // band 5
			-6, // band 6
			 -1  // band 7
		};

static const char *VfoStateStr[] = {
       [VFO_STATE_NORMAL]="",
       [VFO_STATE_BAT_LOW]="BAT LOW",
       [VFO_STATE_TX_DISABLE]="TX DISABLE",
       [VFO_STATE_TIMEOUT]="TIMEOUT",
       [VFO_STATE_VOLTAGE_HIGH]="VOLT HIGH"
};

// ***************************************************************************

static void DrawLevelBar(uint8_t xpos, uint8_t line, uint8_t level)
{
	const char hollowBar[] = {
		0b01111111,
		0b01000001,
		0b01000001,
		0b01111111
	};

	uint8_t *p_line = gFrameBuffer[line];
	level = MIN(level, 13);

	for(uint8_t i = 0; i < level; i++) {
		if(i < 9) {
			for(uint8_t j = 0; j < 4; j++)
				p_line[xpos + i * 5 + j] = (~(0x7F >> (i+1))) & 0x7F;
		}
		else {
			memcpy(p_line + (xpos + i * 5), &hollowBar, ARRAY_SIZE(hollowBar));
		}
	}
}

static void DisplayRSSIBar(const bool now)
{
	const unsigned int txt_width    = 7 * 8;                 // 8 text chars
	const unsigned int bar_x        = 2 + txt_width + 4;     // X coord of bar graph

	const unsigned int line         = 3;
	uint8_t           *p_line        = gFrameBuffer[line];
	char               str[16];

	const char plus[] = {
		0b00011000,
		0b00011000,
		0b01111110,
		0b01111110,
		0b01111110,
		0b00011000,
		0b00011000,
	};

	if ((gEeprom.KEY_LOCK && gKeypadLocked > 0) || gCurrentFunction == FUNCTION_TRANSMIT || gScreenToDisplay != DISPLAY_MAIN)
		return;     // display is in use

	if (now)
		memset(p_line, 0, LCD_WIDTH);

	const int16_t s0_dBm   = -SMETER_S0_LEVEL;                   // S0 .. base level
	const int16_t rssi_dBm = BK4819_GetRSSI_dBm() + dBmCorrTable[gVfo->Band];

	int s0_9 = SMETER_S0_LEVEL - SMETER_S9_LEVEL;
	const uint8_t s_level = MIN(MAX((int32_t)(rssi_dBm - s0_dBm)*100 / (s0_9*100/9), 0), 9); // S0 - S9
	uint8_t overS9dBm = MIN(MAX(rssi_dBm + SMETER_S9_LEVEL, 0), 99);
	uint8_t overS9Bars = MIN(overS9dBm/10, 4);

	if(overS9Bars == 0) {
		sprintf(str, "% 4d S%d", rssi_dBm, s_level);
	}
	else {
		sprintf(str, "% 4d  %2d", rssi_dBm, overS9dBm);
		memcpy(p_line + 2 + 7*5, &plus, ARRAY_SIZE(plus));
	}

	UI_PrintStringSmallNormal(str, 2, 0, line);
	DrawLevelBar(bar_x, line, s_level + overS9Bars);
	if (now)
		ST7565_BlitLine(line);
}

void UI_MAIN_TimeSlice500ms(void)
{
	if (gScreenToDisplay == DISPLAY_MAIN && FUNCTION_IsRx())
		DisplayRSSIBar(true);
}

static void DisplayFrequencyString(const char *String)
{
	// "123.45678": 3 digits, point, 3 big digits, then 2 small ones
	UI_PrintStringSmallNormal(String + 7, 113, 0, 1);
	char big[8];
	memcpy(big, String, 7);
	big[7] = 0;
	UI_DisplayFrequency(big, 32, 0, false);
}

// ***************************************************************************

void UI_DisplayMain(void)
{
	char String[22];

	// clear the screen
	UI_DisplayClear();

	if(gLowBattery && !gLowBatteryConfirmed) {
		UI_DisplayPopup("LOW BATTERY");
		ST7565_BlitFullScreen();
		return;
	}

	if (gEeprom.KEY_LOCK && gKeypadLocked > 0)
	{	// tell user how to unlock the keyboard
		UI_PrintString("Long press #", 0, LCD_WIDTH, 1, 8);
		UI_PrintString("to unlock",    0, LCD_WIDTH, 3, 8);
		ST7565_BlitFullScreen();
		return;
	}

	const VFO_Info_t *vfo = gVfo;

	// TX / RX indicator
	if (gCurrentFunction == FUNCTION_TRANSMIT)
		UI_PrintStringSmallBold("TX", 14, 0, 0);
	else if (MON_Busy())
		UI_PrintStringSmallBold("RX", 14, 0, 0);

	const bool inputting = gInputBoxIndex != 0;

	// frequency, or the reason TX was refused
	if (gVfoState != VFO_STATE_NORMAL && gVfoState < ARRAY_SIZE(VfoStateStr))
	{
		UI_PrintString(VfoStateStr[gVfoState], 31, 0, 0, 8);
	}
	else if (inputting)
	{	// user entering a frequency
		const char * ascii = INPUTBOX_GetAscii();
		sprintf(String, "%.3s.%.5s", ascii, ascii + 3);
		DisplayFrequencyString(String);
	}
	else
	{
		const uint32_t frequency = vfo->Frequency;
		if (frequency < _1GHz_in_KHz) {
			sprintf(String, "%3u.%05u", frequency / 100000, frequency % 100000);
			DisplayFrequencyString(String);
		}
		else {
			sprintf(String, "%4u.%05u", frequency / 100000, frequency % 100000);
			UI_PrintString(String, 32, 0, 0, 8);
		}
	}

	// receive level
	if (FUNCTION_IsRx())
		DisplayRSSIBar(false);

	// the packet settings in use; at most "~0.5W NARR TOT120", 17
	// characters, which ends at column 120
	const bool narrow = vfo->CHANNEL_BANDWIDTH == BANDWIDTH_NARROW;
	sprintf(String, "%s %s TOT%u",
		gPowerNames[vfo->OUTPUT_POWER % 3],
		narrow ? "NARR" : "WIDE",
		gTxTimeoutSeconds[gEeprom.TX_TIMEOUT]);
	UI_PrintStringSmallNormal(String, 2, 0, 4);

	// approximate deviation for the bandwidth in use, at most
	// "DEV ~12.5kHz", 12 characters
	strcpy(String, "DEV ");
	UI_DeviationString(String + 4,
		narrow ? gEeprom.DEVIATION_NARROW : gEeprom.DEVIATION_WIDE);
	UI_PrintStringSmallNormal(String, 2, 0, 5);

	sprintf(String, "RXG%u DAC%u", gEeprom.RX_GAIN, gEeprom.RX_DAC_GAIN);
	UI_PrintStringSmallNormal(String, 2, 0, 6);

	ST7565_BlitFullScreen();
}
