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

#include "battery.h"
#include "driver/backlight.h"
#include "driver/st7565.h"
#include "functions.h"
#include "misc.h"
#include "settings.h"
#include "ui/battery.h"
#include "audio.h"
#include "driver/bk4819.h"
#include "ui/ui.h"

uint16_t          gBatteryCalibration[6];
uint16_t          gBatteryCurrentVoltage;
uint16_t          gBatteryCurrent;
uint16_t          gBatteryVoltages[4];
uint16_t          gBatteryVoltageAverage;
uint8_t           gBatteryDisplayLevel;
bool              gChargingWithTypeC;
bool              gLowBatteryBlink;
bool              gLowBattery;
bool              gLowBatteryConfirmed;
uint16_t          gBatteryCheckCounter;

typedef enum {
	BATTERY_LOW_INACTIVE,
	BATTERY_LOW_ACTIVE,
	BATTERY_LOW_CONFIRMED
} BatteryLow_t;


uint16_t          lowBatteryCountdown;
const uint16_t 	  lowBatteryPeriod = 30;



// Charge percentage against voltage (10 mV units) for the standard 1600 mAh
// pack. The 2200 mAh curve and the battery type setting were removed on
// 2026-09-30: the percentage is only a guide, and the TX limits (about 6.3 V
// and 8.9 V) do not depend on it.
static const uint16_t Voltage2PercentageTable[][2] = {
	{828, 100},
	{814, 97 },
	{760, 25 },
	{729, 6  },
	{630, 0  },
	{0,   0  },
};


unsigned int BATTERY_VoltsToPercent(const unsigned int voltage_10mV)
{
	const uint16_t (*crv)[2] = Voltage2PercentageTable;
	const int mulipl = 1000;
	for (unsigned int i = 1; i < ARRAY_SIZE(Voltage2PercentageTable); i++) {
		if (voltage_10mV > crv[i][0]) {
			const int a = (crv[i - 1][1] - crv[i][1]) * mulipl / (crv[i - 1][0] - crv[i][0]);
			const int b = crv[i][1] - a * crv[i][0] / mulipl;
			const int p = a * voltage_10mV / mulipl + b;
			return MIN(p, 100);
		}
	}

	return 0;
}

void BATTERY_GetReadings(const bool bDisplayBatteryLevel)
{
	const uint8_t  PreviousBatteryLevel = gBatteryDisplayLevel;
	const uint16_t Voltage              = (gBatteryVoltages[0] + gBatteryVoltages[1] + gBatteryVoltages[2] + gBatteryVoltages[3]) / 4;

	gBatteryVoltageAverage = (Voltage * 760) / gBatteryCalibration[3];

	if(gBatteryVoltageAverage > 890)
		gBatteryDisplayLevel = 7; // battery overvoltage
	else if(gBatteryVoltageAverage < 630)
		gBatteryDisplayLevel = 0; // battery critical
	else {
		gBatteryDisplayLevel = 1;
		const uint8_t levels[] = {5,17,41,65,88};
		uint8_t perc = BATTERY_VoltsToPercent(gBatteryVoltageAverage);
		for(uint8_t i = 6; i >= 1; i--){
			if (perc > levels[i-2]) {
				gBatteryDisplayLevel = i;
				break;
			}
		}
	}


	if (gBatteryCurrent < 501)
	{
		if (gChargingWithTypeC)
		{
			gUpdateStatus  = true;
			gUpdateDisplay = true;
		}

		gChargingWithTypeC = false;
	}
	else
	{
		if (!gChargingWithTypeC)
		{
			gUpdateStatus  = true;
			gUpdateDisplay = true;
			BACKLIGHT_TurnOn();
		}

		gChargingWithTypeC = true;
	}

	if (PreviousBatteryLevel != gBatteryDisplayLevel)
	{
		if(gBatteryDisplayLevel > 2)
			gLowBatteryConfirmed = false;
		else if (gBatteryDisplayLevel < 2)
		{
			gLowBattery = true;
		}
		else
		{
			gLowBattery = false;

			if (bDisplayBatteryLevel)
				UI_DisplayBattery(gBatteryDisplayLevel, gLowBatteryBlink);
		}

		if(!gLowBatteryConfirmed)
			gUpdateDisplay = true;

		lowBatteryCountdown = 0;
	}
}

void BATTERY_TimeSlice500ms(void)
{
	if (!gLowBattery) {
		return;
	}

	gLowBatteryBlink = ++lowBatteryCountdown & 1;

	UI_DisplayBattery(0, gLowBatteryBlink);

	if (gCurrentFunction == FUNCTION_TRANSMIT) {
		return;
	}

	// not transmitting

	if (lowBatteryCountdown < lowBatteryPeriod) {
		return;
	}

	lowBatteryCountdown = 0;

	if (gChargingWithTypeC) {
		return;
	}

	// not on charge (no low battery beep in this firmware: it would go to the TNC)
	if (gBatteryDisplayLevel != 0) {
		return;
	}


	// battery critical: receiver off, wait for a charger (APP_TimeSlice500ms)
	gReducedService = true;

	AUDIO_AudioPathOff();
	BK4819_Sleep();
	BK4819_ToggleGpioOut(BK4819_GPIO0_PIN28_RX_ENABLE, false);

	ST7565_HardwareReset();

	if (gEeprom.BACKLIGHT_TIME < 7) {
		BACKLIGHT_TurnOff();
	}
}
