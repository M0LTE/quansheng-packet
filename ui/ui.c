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

#include "driver/keyboard.h"
#include "misc.h"
#include "ui/inputbox.h"
#include "ui/main.h"
#include "ui/menu.h"
#include "ui/ui.h"

GUI_DisplayType_t gScreenToDisplay;
GUI_DisplayType_t gRequestDisplayScreen = DISPLAY_INVALID;

void (*UI_DisplayFunctions[])(void) = {
	[DISPLAY_MAIN] = &UI_DisplayMain,
	[DISPLAY_MENU] = &UI_DisplayMenu,
};

static_assert(ARRAY_SIZE(UI_DisplayFunctions) == DISPLAY_N_ELEM);

void GUI_DisplayScreen(void)
{
	if (gScreenToDisplay != DISPLAY_INVALID) {
		UI_DisplayFunctions[gScreenToDisplay]();
	}
}

void GUI_SelectNextDisplay(GUI_DisplayType_t Display)
{
	if (Display == DISPLAY_INVALID)
		return;

	if (gScreenToDisplay != Display)
	{
		gInputBoxIndex       = 0;
		gIsInSubMenu         = false;
		gWasFKeyPressed      = false;

		gUpdateStatus        = true;
	}

	gScreenToDisplay = Display;
	gUpdateDisplay   = true;
}
