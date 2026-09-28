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

#include "bitmaps.h"
#include "driver/keyboard.h"
#include "driver/st7565.h"
#include "external/printf/printf.h"
#include "functions.h"
#include "helper/battery.h"
#include "misc.h"
#include "settings.h"
#include "ui/battery.h"
#include "ui/helper.h"
#include "ui/ui.h"
#include "ui/status.h"

// Status line: TX/RX, monitor, key lock or F, "PKT", battery voltage, USB-C
// charge, battery level.
void UI_DisplayStatus()
{
	gUpdateStatus = false;
	memset(gStatusLine, 0, sizeof(gStatusLine));

	uint8_t     *line = gStatusLine;
	unsigned int x    = 0;

	if (gCurrentFunction == FUNCTION_TRANSMIT) {
		memcpy(line + x, BITMAP_TX, sizeof(BITMAP_TX));
	}
	else if (FUNCTION_IsRx()) {
		memcpy(line + x, BITMAP_RX, sizeof(BITMAP_RX));
	}
	x += 10;

	if (gMonitor) {
		UI_PrintStringSmallBufferNormal("MON", line + x);
	}
	x += 24;

	// KEY-LOCK indicator
	if (gEeprom.KEY_LOCK) {
		memcpy(line + x, BITMAP_KeyLock, sizeof(BITMAP_KeyLock));
	}
	else if (gWasFKeyPressed) {
		memcpy(line + x, BITMAP_F_Key, sizeof(BITMAP_F_Key));
	}
	x += 10;

	UI_PrintStringSmallBufferNormal("PKT", line + x);

	{	// battery voltage
		char         s[8];
		unsigned int x2 = LCD_WIDTH - sizeof(BITMAP_BatteryLevel1) - sizeof(BITMAP_USB_C);
		const uint16_t voltage = (gBatteryVoltageAverage <= 999) ? gBatteryVoltageAverage : 999; // limit to 9.99V
		sprintf(s, "%u.%02uV", voltage / 100, voltage % 100);
		UI_PrintStringSmallBufferNormal(s, line + x2 - (7 * strlen(s)));
	}

	// move to right side of the screen
	x = LCD_WIDTH - sizeof(BITMAP_BatteryLevel1) - sizeof(BITMAP_USB_C);

	// USB-C charge indicator
	if (gChargingWithTypeC)
		memcpy(line + x, BITMAP_USB_C, sizeof(BITMAP_USB_C));
	x += sizeof(BITMAP_USB_C);

	// BATTERY LEVEL indicator
	UI_DrawBattery(line + x, gBatteryDisplayLevel, gLowBatteryBlink);

	ST7565_BlitStatusLine();
}
