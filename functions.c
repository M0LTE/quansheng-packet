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

#include "driver/backlight.h"
#include "driver/bk4819.h"
#include "driver/st7565.h"
#include "functions.h"
#include "app/monitor.h"
#include "misc.h"
#include "radio.h"
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
	g_SquelchLost = false;
	gUpdateStatus = true;
}

static void FUNCTION_Transmit(void)
{
	// if DTMF is enabled when TX'ing, it changes the TX audio filtering !! .. 1of11
	BK4819_DisableDTMF();

	// Key up first; the screen and status line are redrawn afterwards by
	// the 10 ms slice (upstream redrew the whole screen before key-up).
	RADIO_SetTxParameters();
	MON_TxStarted();          // RF ready: TX_START (protocol v2)

	// turn the RED LED on
	BK4819_ToggleGpioOut(BK4819_GPIO5_PIN1_RED, true);

	gUpdateStatus  = true;
	gUpdateDisplay = true;
}

void FUNCTION_Select(FUNCTION_Type_t Function)
{
	const FUNCTION_Type_t PreviousFunction = gCurrentFunction;

	gCurrentFunction = Function;

	switch (Function) {
		case FUNCTION_FOREGROUND:
			if (PreviousFunction == FUNCTION_TRANSMIT) {
				gFixDisplayAfterTx = true;   // done before the next redraw
				gUpdateDisplay     = true;
			}
			gUpdateStatus = true;
			break;

		case FUNCTION_TRANSMIT:
			FUNCTION_Transmit();
			break;

		case FUNCTION_MONITOR:
			gMonitor = true;
			break;

		case FUNCTION_INCOMING:
		case FUNCTION_RECEIVE:
		default:
			break;
	}
}
