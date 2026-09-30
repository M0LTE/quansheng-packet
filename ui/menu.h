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

#ifndef UI_MENU_H
#define UI_MENU_H

#include <stdbool.h>
#include <stdint.h>

// Packet firmware menu: only the settings a packet station needs.
enum {
	MENU_STEP = 0,  // frequency step (the operating channel)
	MENU_TXP,       // TX power (the operating channel)
	MENU_W_N,       // bandwidth (the operating channel)
	MENU_DEVW,      // wide deviation, REG_40<11:0>
	MENU_DEVN,      // narrow deviation, REG_40<11:0>
	MENU_RXG,       // RX AF gain 2, REG_48<9:4>
	MENU_RXDAC,     // RX AF DAC gain, REG_48<3:0>
	MENU_TOT,       // TX timeout
	MENU_ABR,       // backlight time
	MENU_BATTYP,    // battery type
	MENU_VOL,       // battery voltage (read only)
	MENU_VER,       // firmware version (read only)
	MENU_N_ITEMS
};

extern const char        gMenuNames[MENU_N_ITEMS][7];
extern const char        gSubMenu_BACKLIGHT[8][7];

extern bool              gIsInSubMenu;
extern uint8_t           gMenuCursor;
extern int32_t           gSubMenuSelection;

void UI_DisplayMenu(void);

#endif
