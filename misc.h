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

#ifndef MISC_H
#define MISC_H

#include <stdbool.h>
#include <stdint.h>

#ifndef ARRAY_SIZE
	#define ARRAY_SIZE(x) (sizeof(x) / sizeof(x[0]))
#endif

#ifndef MAX
	#define MAX(a, b) ({ __typeof__ (a) _a = (a); __typeof__ (b) _b = (b); _a > _b ? _a : _b; })
#endif

#ifndef MIN
	#define MIN(a, b) ({ __typeof__ (a) _a = (a); __typeof__ (b) _b = (b); _a < _b ? _a : _b; })
#endif

enum {
	VFO_CONFIGURE_NONE = 0,
	VFO_CONFIGURE
};

extern const uint8_t         vfo_state_resume_countdown_500ms;
extern const uint8_t         menu_timeout_500ms;
extern const uint8_t         key_input_timeout_500ms;
extern const uint16_t        key_repeat_delay_10ms;
extern const uint16_t        key_repeat_10ms;
extern const uint16_t        key_debounce_10ms;

// Serial PTT lock (protocol v2, 5.3). Every valid frame from the host sets
// gSerialLockMs to the SERIAL_LOCK_MS setting (default 20 ms) and ends any
// transmission; the SysTick handler counts it down every 1 ms. A press
// while it runs keys at most LATE_KEY_MAX_MS late, or is refused until PTT
// is released (pttarb.h). v1 held PTT off for 1.0 to 1.5 s here.
#define SERIAL_LOCK_DEFAULT_MS  20u
#define SERIAL_LOCK_MAX_MS      1500u
#define LATE_KEY_MAX_MS         30u

// The settings reload after a legacy 0x051D EEPROM write session waits for
// this long after the last write, and never runs during a transmission.
#define SERIAL_RELOAD_QUIET_MS  1000u

// TX frequency limits, read from EEPROM 0x0F40 (no menu for them).
extern bool                  gSetting_350TX;
extern bool                  gSetting_200TX;
extern bool                  gSetting_500TX;
extern bool                  gSetting_350EN;
extern uint8_t               gSetting_F_LOCK;

extern bool                  gMonitor;

extern volatile uint32_t     g_ms;               // ms since boot, SysTick
extern volatile uint16_t     gSerialLockMs;      // serial PTT lock remaining, ms
extern volatile uint16_t     gReloadQuietMs;     // quiet time left before the reload, ms
// set by a UART EEPROM write; settings and channel are reloaded once the
// host has been quiet for SERIAL_RELOAD_QUIET_MS
extern bool                  gReloadSettingsAfterSerial;
extern volatile bool         gNextTimeslice_500ms;
extern volatile uint16_t     gTxTimerCountdown_500ms;
extern volatile bool         gTxTimeoutReached;

extern bool                  gEnableSpeaker;
extern uint8_t               gKeyInputCountdown;
extern uint8_t               gUpdateStatus;

// battery critical, limit functionality to minimum
extern uint8_t               gReducedService;
extern uint8_t               gBatteryVoltageIndex;

extern uint16_t              gMenuCountdown;
extern bool                  gPttWasReleased;
extern bool                  gFlagReconfigureVfos;
extern uint8_t               gVfoConfigureMode;
extern bool                  gRequestSaveOperating;   // frequency, power, bandwidth, step
extern bool                  gRequestSaveSettings;
extern uint8_t               gKeypadLocked;
extern bool                  gFlagPrepareTX;

extern bool                  gFlagAcceptSetting;   // accept menu setting
extern bool                  gFlagRefreshSetting;  // refresh menu display

// true means we are receiving signal
extern bool                  g_SquelchLost;

extern bool                  gKeyBeingHeld;
extern bool                  gPttIsPressed;
extern volatile bool         gNextTimeslice;
extern bool                  gUpdateDisplay;
extern bool                  gFixDisplayAfterTx;
extern volatile uint8_t      gVFOStateResumeCountdown_500ms;
extern volatile uint8_t      boot_counter_10ms;

int32_t NUMBER_AddWithWraparound(int32_t Base, int32_t Add, int32_t LowerLimit, int32_t UpperLimit);
unsigned long StrToUL(const char * str);

static inline bool SerialConfigInProgress(void) { return gSerialLockMs != 0; }

#endif
