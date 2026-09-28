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

#define IS_MR_CHANNEL(x)       ((x) <= MR_CHANNEL_LAST)
#define IS_FREQ_CHANNEL(x)     ((x) >= FREQ_CHANNEL_FIRST && (x) <= FREQ_CHANNEL_LAST)
#define IS_VALID_CHANNEL(x)    ((x) < LAST_CHANNEL)

// Channel numbers, as in the upstream EEPROM layout: 200 memory channels,
// then one frequency (VFO) slot per band.
enum {
	MR_CHANNEL_FIRST   = 0,
	MR_CHANNEL_LAST    = 199u,
	FREQ_CHANNEL_FIRST = 200u,
	FREQ_CHANNEL_LAST  = 206u,
	LAST_CHANNEL
};

enum {
	VFO_CONFIGURE_NONE = 0,
	VFO_CONFIGURE,
	VFO_CONFIGURE_RELOAD
};

extern const uint8_t         vfo_state_resume_countdown_500ms;
extern const uint8_t         menu_timeout_500ms;
extern const uint8_t         key_input_timeout_500ms;
extern const uint16_t        key_repeat_delay_10ms;
extern const uint16_t        key_repeat_10ms;
extern const uint16_t        key_debounce_10ms;

// PTT is ignored, and any transmission is stopped, for this long after the
// last UART configuration command (hello, EEPROM read or write). On the K1
// connector the UART receive line shares a contact with PTT, so serial
// traffic must never be taken as a key press. Upstream used 12 (6 s).
// Counted in 500 ms ticks, so 3 means 1.0 to 1.5 s.
#define SERIAL_PTT_LOCK_500ms  3u

// TX frequency limits, read from EEPROM 0x0F40 (no menu for them).
extern bool                  gSetting_350TX;
extern bool                  gSetting_200TX;
extern bool                  gSetting_500TX;
extern bool                  gSetting_350EN;
extern uint8_t               gSetting_F_LOCK;

extern bool                  gMonitor;

typedef union {
    struct {
        uint8_t
            band : 4,
            compander : 2,
            scanlist2 : 1,
            scanlist1 : 1;
    };
    uint8_t __val;
} ChannelAttributes_t;

extern ChannelAttributes_t   gMR_ChannelAttributes[FREQ_CHANNEL_LAST + 1];

extern volatile uint8_t      gSerialConfigCountDown_500ms;
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
extern bool                  gPttWasPressed;
extern bool                  gFlagReconfigureVfos;
extern uint8_t               gVfoConfigureMode;
extern bool                  gRequestSaveVFO;
extern bool                  gRequestSaveChannel;
extern bool                  gRequestSaveSettings;
extern uint8_t               gKeypadLocked;
extern bool                  gFlagPrepareTX;

extern bool                  gFlagAcceptSetting;   // accept menu setting
extern bool                  gFlagRefreshSetting;  // refresh menu display

// true means we are receiving signal
extern bool                  g_SquelchLost;

extern bool                  gKeyBeingHeld;
extern bool                  gPttIsPressed;
extern uint8_t               gPttDebounceCounter;
extern volatile bool         gNextTimeslice;
extern bool                  gUpdateDisplay;
extern volatile uint8_t      gVFOStateResumeCountdown_500ms;
extern volatile uint8_t      boot_counter_10ms;

int32_t NUMBER_AddWithWraparound(int32_t Base, int32_t Add, int32_t LowerLimit, int32_t UpperLimit);
unsigned long StrToUL(const char * str);

inline bool SerialConfigInProgress() { return gSerialConfigCountDown_500ms != 0; }

#endif
