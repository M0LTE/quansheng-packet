/* Copyright 2026 packet-fw contributors
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

// Host tests of the menu screen layout: the real ui/menu.c, ui/helper.c
// and font.c draw into a framebuffer with a guard band after it. For every
// menu item and every value it can show: the edit cursor (">") never
// touches the value text and leaves a clear column on each side, and
// nothing is drawn outside the 128 x 8 framebuffer.

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "frequencies.h"
#include "helper/battery.h"
#include "misc.h"
#include "ui/helper.h"
#include "ui/menu.h"

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

// driver/st7565.h: 7 lines of 128 columns. Defined here with two more
// lines after it as a guard band that must stay blank (the header is not
// included, so the definition can be larger than its declaration).
#define LINES 7
#define WIDTH 128
uint8_t gFrameBuffer[LINES + 2][WIDTH];
#define GUARD (&gFrameBuffer[LINES][0])

// what ui/menu.c needs from the rest of the firmware
const uint8_t gTxTimeoutSeconds[7] = { 5, 10, 15, 20, 30, 60, 120 };
uint16_t      gBatteryVoltageAverage = 812;
const char    Version[] = "PKTFW 30ed918";
void          ST7565_BlitFullScreen(void) {}
unsigned int  BATTERY_VoltsToPercent(unsigned int v) { (void)v; return 100; }
void          BACKLIGHT_TurnOn(void) {}

int sprintf_(char *buffer, const char *format, ...)
{
	va_list ap;
	va_start(ap, format);
	const int n = vsprintf(buffer, format, ap);
	va_end(ap);
	return n;
}

static void Render(uint8_t item, int32_t value, bool editing)
{
	gMenuCursor       = item;
	gSubMenuSelection = value;
	gIsInSubMenu      = editing;
	UI_DisplayMenu();
}

static int32_t MaxValue(uint8_t item)
{
	switch (item) {
		case MENU_STEP:   return STEP_N_ELEM - 1;
		case MENU_TXP:    return 2;
		case MENU_W_N:    return 1;
		case MENU_DEVW:
		case MENU_DEVN:   return 0xA7F;
		case MENU_RXG:    return 63;
		case MENU_RXDAC:  return 15;
		case MENU_TOT:    return 6;
		case MENU_ABR:    return 7;
		default:          return 0;
	}
}

static bool Blank(const uint8_t *p, unsigned n)
{
	for (unsigned i = 0; i < n; i++)
		if (p[i])
			return false;
	return true;
}

int main(void)
{
	unsigned checked = 0;

	for (uint8_t item = 0; item < MENU_N_ITEMS; item++) {
		for (int32_t v = 0; v <= MaxValue(item); v++) {
			uint8_t plain[LINES][WIDTH];

			Render(item, v, false);
			memcpy(plain, gFrameBuffer, sizeof(plain));
			CHECK(Blank(GUARD, 2 * WIDTH));

			Render(item, v, true);
			CHECK(Blank(GUARD, 2 * WIDTH));

			// the cursor is the only change, on line 1, left of the value
			int lo = -1, hi = -1;
			for (unsigned line = 0; line < LINES; line++)
				for (unsigned x = 0; x < WIDTH; x++)
					if (gFrameBuffer[line][x] != plain[line][x]) {
						CHECK(line == 1);
						if (lo < 0) lo = (int)x;
						hi = (int)x;
					}
			CHECK(lo >= 0);
			if (lo < 0)
				continue;

			// it went where nothing was drawn, so it hides nothing
			for (int x = lo; x <= hi; x++)
				CHECK(plain[1][x] == 0);

			// a clear column either side of the cursor, above and below
			// (the big-font value covers lines 1 and 2)
			CHECK(lo >= 1 && hi + 1 < WIDTH);
			if (lo >= 1 && hi + 1 < WIDTH) {
				for (unsigned line = 1; line <= 2; line++) {
					CHECK(gFrameBuffer[line][lo - 1] == 0);
					CHECK(gFrameBuffer[line][hi + 1] == 0);
				}
			}
			for (int x = lo; x <= hi; x++)
				CHECK(gFrameBuffer[2][x] == 0);
			if (failures) {
				printf("  item %u (%s), value %d: cursor ink at columns %d to %d\n", item, gMenuNames[item], (int)v, lo, hi);
				return 1;
			}
			checked++;
		}
	}

	// the power names fit the menu's big font and the main screen's line 4
	for (unsigned i = 0; i < 3; i++) {
		CHECK(strlen(gPowerNames[i]) <= 5);
		char line4[32];
		sprintf(line4, "%s %s TOT%u", gPowerNames[i], "NARR", 120u);
		CHECK(2 + 7 * strlen(line4) <= WIDTH);   // as ui/main.c draws it
	}

	if (failures) {
		printf("%d check(s) failed\n", failures);
		return 1;
	}
	printf("all menu layout host tests passed (%u screens)\n", checked);
	return 0;
}
