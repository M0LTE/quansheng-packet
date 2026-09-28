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

#include "misc.h"

const uint8_t     vfo_state_resume_countdown_500ms =  2500 / 500;  // 2.5 seconds
const uint8_t     menu_timeout_500ms               = 20000 / 500;  // 20 seconds
const uint8_t     key_input_timeout_500ms          =  8000 / 500;  // 8 seconds
const uint16_t    key_repeat_delay_10ms            =   400 / 10;   // 400ms
const uint16_t    key_repeat_10ms                  =    80 / 10;   // 80ms .. MUST be less than 'key_repeat_delay'
const uint16_t    key_debounce_10ms                =    20 / 10;   // 20ms

bool              gSetting_350TX;
bool              gSetting_200TX;
bool              gSetting_500TX;
bool              gSetting_350EN;
uint8_t           gSetting_F_LOCK;

bool              gMonitor = false;           // true opens the squelch

ChannelAttributes_t gMR_ChannelAttributes[FREQ_CHANNEL_LAST + 1];

volatile uint8_t  gSerialConfigCountDown_500ms;
bool              gReloadSettingsAfterSerial;
volatile bool     gNextTimeslice_500ms;
volatile uint16_t gTxTimerCountdown_500ms;
volatile bool     gTxTimeoutReached;
volatile uint8_t  gVFOStateResumeCountdown_500ms;

bool              gEnableSpeaker;
uint8_t           gKeyInputCountdown = 0;
uint8_t           gUpdateStatus;

uint8_t           gReducedService;
uint8_t           gBatteryVoltageIndex;

uint16_t          gMenuCountdown;
bool              gPttWasReleased;
bool              gPttWasPressed;
uint8_t           gKeypadLocked;
bool              gFlagReconfigureVfos;
uint8_t           gVfoConfigureMode;
bool              gRequestSaveVFO;
bool              gRequestSaveChannel;
bool              gRequestSaveSettings;
bool              gFlagPrepareTX;

bool              gFlagAcceptSetting;
bool              gFlagRefreshSetting;

bool              g_SquelchLost;

bool              gKeyBeingHeld;
bool              gPttIsPressed;

bool              gUpdateDisplay;
bool              gFixDisplayAfterTx;

volatile bool     gNextTimeslice;
volatile uint8_t  boot_counter_10ms;

int32_t NUMBER_AddWithWraparound(int32_t Base, int32_t Add, int32_t LowerLimit, int32_t UpperLimit)
{
	Base += Add;

	if (Base == 0x7fffffff || Base < LowerLimit)
		return UpperLimit;

	if (Base > UpperLimit)
		return LowerLimit;

	return Base;
}

unsigned long StrToUL(const char * str)
{
	unsigned long ul = 0;
	for(uint8_t i = 0; i < strlen(str); i++){
		char c = str[i];
		if(c < '0' || c > '9')
			break;
		ul = ul * 10 + (uint8_t)(c-'0');
	}
	return ul;
}
