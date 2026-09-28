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

#include <stdint.h>
#include <string.h>
#include <stdio.h>     // NULL

#include "audio.h"
#include "board.h"
#include "misc.h"
#include "radio.h"
#include "settings.h"
#include "version.h"

#include "app/app.h"
#include "bsp/dp32g030/gpio.h"
#include "bsp/dp32g030/syscon.h"

#include "driver/backlight.h"
#include "driver/bk4819.h"
#include "driver/gpio.h"
#include "driver/keyboard.h"
#include "driver/system.h"
#include "driver/systick.h"
#include "driver/uart.h"

#include "helper/battery.h"

#include "ui/ui.h"
#include "ui/welcome.h"

void _putchar(__attribute__((unused)) char c)
{
	UART_Send((uint8_t *)&c, 1);
}

void Main(void)
{
	// Enable clock gating of blocks we need
	SYSCON_DEV_CLK_GATE = 0
		| SYSCON_DEV_CLK_GATE_GPIOA_BITS_ENABLE
		| SYSCON_DEV_CLK_GATE_GPIOB_BITS_ENABLE
		| SYSCON_DEV_CLK_GATE_GPIOC_BITS_ENABLE
		| SYSCON_DEV_CLK_GATE_UART1_BITS_ENABLE
		| SYSCON_DEV_CLK_GATE_SPI0_BITS_ENABLE
		| SYSCON_DEV_CLK_GATE_SARADC_BITS_ENABLE
		| SYSCON_DEV_CLK_GATE_CRC_BITS_ENABLE
		| SYSCON_DEV_CLK_GATE_PWM_PLUS0_BITS_ENABLE;

	SYSTICK_Init();
	BOARD_Init();

	boot_counter_10ms = 250;   // 2.5 sec

	UART_Init();
	UART_Send(UART_Version, strlen(UART_Version));

	BK4819_Init();

	BOARD_ADC_GetBatteryInfo(&gBatteryCurrentVoltage, &gBatteryCurrent);

	SETTINGS_InitEEPROM();
	SETTINGS_LoadCalibration();

	RADIO_ConfigureChannel();
	RADIO_SetupRegisters(true);

	for (unsigned int i = 0; i < ARRAY_SIZE(gBatteryVoltages); i++)
		BOARD_ADC_GetBatteryInfo(&gBatteryVoltages[i], &gBatteryCurrent);

	BATTERY_GetReadings(false);

	// wait for user to release all buttons before moving on
	if (!GPIO_CheckBit(&GPIOC->DATA, GPIOC_PIN_PTT) || KEYBOARD_Poll() != KEY_INVALID)
	{	// keys are pressed
		UI_DisplayReleaseKeys();
		BACKLIGHT_TurnOn();

		// 500ms
		for (int i = 0; i < 50;)
		{
			i = (GPIO_CheckBit(&GPIOC->DATA, GPIOC_PIN_PTT) && KEYBOARD_Poll() == KEY_INVALID) ? i + 1 : 0;
			SYSTEM_DelayMs(10);
		}
		gKeyReading0 = KEY_INVALID;
		gKeyReading1 = KEY_INVALID;
		gDebounceCounter = 0;
	}

	if (!gChargingWithTypeC && gBatteryDisplayLevel == 0)
	{	// battery critical: receiver off until a charger is connected
		AUDIO_AudioPathOff();
		BK4819_Sleep();
		BK4819_ToggleGpioOut(BK4819_GPIO0_PIN28_RX_ENABLE, false);

		if (gEeprom.BACKLIGHT_TIME < 7) // backlight is not set to be always on
			BACKLIGHT_TurnOff();
		else
			BACKLIGHT_TurnOn();

		gReducedService = true;
	}
	else
	{
		UI_DisplayWelcome();

		BACKLIGHT_TurnOn();

		// 2.5 second boot-up screen, cut short by any key
		while (boot_counter_10ms > 0)
		{
			if (KEYBOARD_Poll() != KEY_INVALID)
			{
				boot_counter_10ms = 0;
				break;
			}
		}

		GUI_SelectNextDisplay(DISPLAY_MAIN);

		GPIO_ClearBit(&GPIOA->DATA, GPIOA_PIN_VOICE_0);

		gUpdateStatus = true;
	}

	while (true) {
		APP_Update();

		if (gNextTimeslice) {

			APP_TimeSlice10ms();

			if (gNextTimeslice_500ms) {
				APP_TimeSlice500ms();
			}
		}
	}
}
