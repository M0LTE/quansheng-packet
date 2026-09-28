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
#include <stdlib.h>  // abs()

#include "app/chFrScanner.h"
#include "app/dtmf.h"
#include "bitmaps.h"
#include "board.h"
#include "driver/bk4819.h"
#include "driver/st7565.h"
#include "external/printf/printf.h"
#include "functions.h"
#include "helper/battery.h"
#include "misc.h"
#include "radio.h"
#include "settings.h"
#include "ui/helper.h"
#include "ui/inputbox.h"
#include "ui/main.h"
#include "ui/ui.h"

center_line_t center_line = CENTER_LINE_NONE;

const int8_t dBmCorrTable[7] = {
			-15, // band 1
			-25, // band 2
			-20, // band 3
			-4, // band 4
			-7, // band 5
			-6, // band 6
			 -1  // band 7
		};

const char *VfoStateStr[] = {
       [VFO_STATE_NORMAL]="",
       [VFO_STATE_BUSY]="BUSY",
       [VFO_STATE_BAT_LOW]="BAT LOW",
       [VFO_STATE_TX_DISABLE]="TX DISABLE",
       [VFO_STATE_TIMEOUT]="TIMEOUT",
       [VFO_STATE_ALARM]="ALARM",
       [VFO_STATE_VOLTAGE_HIGH]="VOLT HIGH"
};

// ***************************************************************************

static void DrawSmallAntennaAndBars(uint8_t *p, unsigned int level)
{
	if(level>6)
		level = 6;

	memcpy(p, BITMAP_Antenna, ARRAY_SIZE(BITMAP_Antenna));

	for(uint8_t i = 1; i <= level; i++) {
		char bar = (0xff << (6-i)) & 0x7F;
		memset(p + 2 + i*3, bar, 2);
	}
}

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



void DisplayRSSIBar(const bool now)
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

	if ((gEeprom.KEY_LOCK && gKeypadLocked > 0) || center_line != CENTER_LINE_RSSI)
		return;     // display is in use

	if (gCurrentFunction == FUNCTION_TRANSMIT ||
		gScreenToDisplay != DISPLAY_MAIN
		)
		return;     // display is in use

	if (now)
		memset(p_line, 0, LCD_WIDTH);


	const int16_t s0_dBm   = -gEeprom.S0_LEVEL;                  // S0 .. base level
	const int16_t rssi_dBm =
		BK4819_GetRSSI_dBm()
		+ dBmCorrTable[gRxVfo->Band];

	int s0_9 = gEeprom.S0_LEVEL - gEeprom.S9_LEVEL;
	const uint8_t s_level = MIN(MAX((int32_t)(rssi_dBm - s0_dBm)*100 / (s0_9*100/9), 0), 9); // S0 - S9
	uint8_t overS9dBm = MIN(MAX(rssi_dBm + gEeprom.S9_LEVEL, 0), 99);
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
	if(gScreenToDisplay==DISPLAY_MAIN) {

		if(FUNCTION_IsRx()) {
			DisplayRSSIBar(true);
		}
	}
}

// ***************************************************************************

void UI_DisplayMain(void)
{
	char               String[22];

	center_line = CENTER_LINE_NONE;

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

	unsigned int activeTxVFO = gRxVfoIsActive ? gEeprom.RX_VFO : gEeprom.TX_VFO;

	for (unsigned int vfo_num = 0; vfo_num < 2; vfo_num++)
	{
		const unsigned int line0 = 0;  // text screen line
		const unsigned int line1 = 4;
		const unsigned int line       = (vfo_num == 0) ? line0 : line1;
		const bool         isMainVFO  = (vfo_num == gEeprom.TX_VFO);
		uint8_t           *p_line0    = gFrameBuffer[line + 0];
		uint8_t           *p_line1    = gFrameBuffer[line + 1];
		enum Vfo_txtr_mode mode       = VFO_MODE_NONE;

		if (activeTxVFO != vfo_num) // this is not active TX VFO
		{


			if (gDTMF_InputMode
			) {
				char *pPrintStr = "";
				// show DTMF stuff
				{
					sprintf(String, ">%s", gDTMF_InputBox);
					pPrintStr = String;
				}

				UI_PrintString(pPrintStr, 2, 0, 0 + (vfo_num * 3), 8);

				center_line = CENTER_LINE_IN_USE;
				continue;
			}

			// highlight the selected/used VFO with a marker
			if (isMainVFO)
				memcpy(p_line0 + 0, BITMAP_VFO_Default, sizeof(BITMAP_VFO_Default));
		}
		else // active TX VFO
		{	// highlight the selected/used VFO with a marker
			if (isMainVFO)
				memcpy(p_line0 + 0, BITMAP_VFO_Default, sizeof(BITMAP_VFO_Default));
			else
				memcpy(p_line0 + 0, BITMAP_VFO_NotDefault, sizeof(BITMAP_VFO_NotDefault));
		}

		if (gCurrentFunction == FUNCTION_TRANSMIT)
		{	// transmitting

			{
				if (activeTxVFO == vfo_num)
				{	// show the TX symbol
					mode = VFO_MODE_TX;
					UI_PrintStringSmallBold("TX", 14, 0, line);
				}
			}
		}
		else
		{	// receiving .. show the RX symbol
			mode = VFO_MODE_RX;
			if (FUNCTION_IsRx() && gEeprom.RX_VFO == vfo_num) {
				UI_PrintStringSmallBold("RX", 14, 0, line);
			}
		}

		if (IS_MR_CHANNEL(gEeprom.ScreenChannel[vfo_num]))
		{	// channel mode
			const unsigned int x = 2;
			const bool inputting = gInputBoxIndex != 0 && gEeprom.TX_VFO == vfo_num;
			if (!inputting)
				sprintf(String, "M%u", gEeprom.ScreenChannel[vfo_num] + 1);
			else
				sprintf(String, "M%.3s", INPUTBOX_GetAscii());  // show the input text
			UI_PrintStringSmallNormal(String, x, 0, line + 1);
		}
		else if (IS_FREQ_CHANNEL(gEeprom.ScreenChannel[vfo_num]))
		{	// frequency mode
			// show the frequency band number
			const unsigned int x = 2;
			char * buf = gEeprom.VfoInfo[vfo_num].pRX->Frequency < _1GHz_in_KHz ? "" : "+";
			sprintf(String, "F%u%s", 1 + gEeprom.ScreenChannel[vfo_num] - FREQ_CHANNEL_FIRST, buf);
			UI_PrintStringSmallNormal(String, x, 0, line + 1);
		}

		// ************

		enum VfoState_t state = VfoState[vfo_num];


		uint32_t frequency = gEeprom.VfoInfo[vfo_num].pRX->Frequency;

		if (state != VFO_STATE_NORMAL)
		{
			if (state < ARRAY_SIZE(VfoStateStr))
				UI_PrintString(VfoStateStr[state], 31, 0, line, 8);
		}
		else if (gInputBoxIndex > 0 && IS_FREQ_CHANNEL(gEeprom.ScreenChannel[vfo_num]) && gEeprom.TX_VFO == vfo_num)
		{	// user entering a frequency
			const char * ascii = INPUTBOX_GetAscii();
			bool isGigaF = frequency>=_1GHz_in_KHz;
			sprintf(String, "%.*s.%.3s", 3 + isGigaF, ascii, ascii + 3 + isGigaF);
			if(!isGigaF) {
				// show the remaining 2 small frequency digits
				UI_PrintStringSmallNormal(String + 7, 113, 0, line + 1);
				String[7] = 0;
				// show the main large frequency digits
				UI_DisplayFrequency(String, 32, line, false);
			}
			else
			{
				// show the frequency in the main font
				UI_PrintString(String, 32, 0, line, 8);
			}

			continue;
		}
		else
		{
			if (gCurrentFunction == FUNCTION_TRANSMIT)
			{	// transmitting
				if (activeTxVFO == vfo_num)
					frequency = gEeprom.VfoInfo[vfo_num].pTX->Frequency;
			}

			if (IS_MR_CHANNEL(gEeprom.ScreenChannel[vfo_num]))
			{	// it's a channel

				// show the scan list assigment symbols
				const ChannelAttributes_t att = gMR_ChannelAttributes[gEeprom.ScreenChannel[vfo_num]];
				if (att.scanlist1)
					memcpy(p_line0 + 113, BITMAP_ScanList1, sizeof(BITMAP_ScanList1));
				if (att.scanlist2)
					memcpy(p_line0 + 120, BITMAP_ScanList2, sizeof(BITMAP_ScanList2));

				// compander symbol
				// TODO:  // find somewhere else to put the symbol

				switch (gEeprom.CHANNEL_DISPLAY_MODE)
				{
					case MDF_FREQUENCY:	// show the channel frequency
						sprintf(String, "%3u.%05u", frequency / 100000, frequency % 100000);
						if(frequency < _1GHz_in_KHz) {
							// show the remaining 2 small frequency digits
							UI_PrintStringSmallNormal(String + 7, 113, 0, line + 1);
							String[7] = 0;
							// show the main large frequency digits
							UI_DisplayFrequency(String, 32, line, false);
						}
						else
						{
							// show the frequency in the main font
							UI_PrintString(String, 32, 0, line, 8);
						}

						break;

					case MDF_CHANNEL:	// show the channel number
						sprintf(String, "CH-%03u", gEeprom.ScreenChannel[vfo_num] + 1);
						UI_PrintString(String, 32, 0, line, 8);
						break;

					case MDF_NAME:		// show the channel name
					case MDF_NAME_FREQ:	// show the channel name and frequency

						SETTINGS_FetchChannelName(String, gEeprom.ScreenChannel[vfo_num]);
						if (String[0] == 0)
						{	// no channel name, show the channel number instead
							sprintf(String, "CH-%03u", gEeprom.ScreenChannel[vfo_num] + 1);
						}

						if (gEeprom.CHANNEL_DISPLAY_MODE == MDF_NAME) {
							UI_PrintString(String, 32, 0, line, 8);
						}
						else {
							UI_PrintStringSmallBold(String, 32 + 4, 0, line);
							// show the channel frequency below the channel number/name
							sprintf(String, "%03u.%05u", frequency / 100000, frequency % 100000);
							UI_PrintStringSmallNormal(String, 32 + 4, 0, line + 1);
						}

						break;
				}
			}
			else
			{	// frequency mode
				sprintf(String, "%3u.%05u", frequency / 100000, frequency % 100000);

				if(frequency < _1GHz_in_KHz) {
					// show the remaining 2 small frequency digits
					UI_PrintStringSmallNormal(String + 7, 113, 0, line + 1);
					String[7] = 0;
					// show the main large frequency digits
					UI_DisplayFrequency(String, 32, line, false);
				}
				else
				{
					// show the frequency in the main font
					UI_PrintString(String, 32, 0, line, 8);
				}

				// show the channel symbols
				const ChannelAttributes_t att = gMR_ChannelAttributes[gEeprom.ScreenChannel[vfo_num]];
				if (att.compander)
					memcpy(p_line0 + 120, BITMAP_compand, sizeof(BITMAP_compand));
			}
		}

		// ************

		{	// show the TX/RX level
			uint8_t Level = 0;

			if (mode == VFO_MODE_TX)
			{	// TX power level
				switch (gRxVfo->OUTPUT_POWER)
				{
					case OUTPUT_POWER_LOW:  Level = 2; break;
					case OUTPUT_POWER_MID:  Level = 4; break;
					case OUTPUT_POWER_HIGH: Level = 6; break;
				}
			}
			else
			if (mode == VFO_MODE_RX)
			{	// RX signal level
			}
			if(Level)
				DrawSmallAntennaAndBars(p_line1 + LCD_WIDTH, Level);
		}

		// ************

		String[0] = '\0';
		const VFO_Info_t *vfoInfo = &gEeprom.VfoInfo[vfo_num];

		// show the modulation symbol
		const char * s = "";
		const ModulationMode_t mod = vfoInfo->Modulation;
		switch (mod){
			case MODULATION_FM: {
				const FREQ_Config_t *pConfig = (mode == VFO_MODE_TX) ? vfoInfo->pTX : vfoInfo->pRX;
				const unsigned int code_type = pConfig->CodeType;
				const char *code_list[] = {"", "CT", "DCS", "DCR"};
				if (code_type < ARRAY_SIZE(code_list))
					s = code_list[code_type];
				break;
			}
			default:
				s = gModulationStr[mod];
			break;
		}
		UI_PrintStringSmallNormal(s, LCD_WIDTH + 24, 0, line + 1);

		if (state == VFO_STATE_NORMAL || state == VFO_STATE_ALARM)
		{	// show the TX power
			const char pwr_list[][2] = {"L","M","H"};
			int i = vfoInfo->OUTPUT_POWER % 3;
			UI_PrintStringSmallNormal(pwr_list[i], LCD_WIDTH + 46, 0, line + 1);
		}

		if (vfoInfo->freq_config_RX.Frequency != vfoInfo->freq_config_TX.Frequency)
		{	// show the TX offset symbol
			const char dir_list[][2] = {"", "+", "-"};
			int i = vfoInfo->TX_OFFSET_FREQUENCY_DIRECTION % 3;
			UI_PrintStringSmallNormal(dir_list[i], LCD_WIDTH + 54, 0, line + 1);
		}

		// show the TX/RX reverse symbol
		if (vfoInfo->FrequencyReverse)
			UI_PrintStringSmallNormal("R", LCD_WIDTH + 62, 0, line + 1);

		if (vfoInfo->CHANNEL_BANDWIDTH == BANDWIDTH_NARROW)
			UI_PrintStringSmallNormal("N", LCD_WIDTH + 70, 0, line + 1);


		// show the audio scramble symbol
		if (vfoInfo->SCRAMBLING_TYPE > 0 && gSetting_ScrambleEnable)
			UI_PrintStringSmallNormal("SCR", LCD_WIDTH + 106, 0, line + 1);
	}


	if (center_line == CENTER_LINE_NONE)
	{	// we're free to use the middle line

		const bool rx = FUNCTION_IsRx();



		if (rx) {
			center_line = CENTER_LINE_RSSI;
			DisplayRSSIBar(false);
		}
		else
		if (rx || gCurrentFunction == FUNCTION_FOREGROUND || gCurrentFunction == FUNCTION_POWER_SAVE)
		{
			#if 1
				if (gSetting_live_DTMF_decoder && gDTMF_RX_live[0] != 0)
				{	// show live DTMF decode
					const unsigned int len = strlen(gDTMF_RX_live);
					const unsigned int idx = (len > (17 - 5)) ? len - (17 - 5) : 0;  // limit to last 'n' chars

					if (gScreenToDisplay != DISPLAY_MAIN
						)
						return;

					center_line = CENTER_LINE_DTMF_DEC;

					sprintf(String, "DTMF %s", gDTMF_RX_live + idx);
					UI_PrintStringSmallNormal(String, 2, 0, 3);
				}
			#else
				if (gSetting_live_DTMF_decoder && gDTMF_RX_index > 0)
				{	// show live DTMF decode
					const unsigned int len = gDTMF_RX_index;
					const unsigned int idx = (len > (17 - 5)) ? len - (17 - 5) : 0;  // limit to last 'n' chars

					if (gScreenToDisplay != DISPLAY_MAIN ||
						gDTMF_CallState != DTMF_CALL_STATE_NONE)
						return;

					center_line = CENTER_LINE_DTMF_DEC;

					sprintf(String, "DTMF %s", gDTMF_RX_live + idx);
					UI_PrintStringSmallNormal(String, 2, 0, 3);
				}
			#endif

		}
	}

	ST7565_BlitFullScreen();
}

// ***************************************************************************
