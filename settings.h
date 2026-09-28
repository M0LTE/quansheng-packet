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

#ifndef SETTINGS_H
#define SETTINGS_H

#include <stdbool.h>
#include <stdint.h>

#include "frequencies.h"
#include <helper/battery.h>
#include "radio.h"
#include <driver/backlight.h>

enum TxLockModes_t {
	F_LOCK_DEF, //all default frequencies + configurable
	F_LOCK_FCC,
	F_LOCK_CE,
	F_LOCK_GB,
	F_LOCK_430,
	F_LOCK_438,
	F_LOCK_ALL,	// disable TX on all frequencies
	F_LOCK_NONE, // enable TX on all frequencies
	F_LOCK_LEN
};

enum {
	OUTPUT_POWER_LOW = 0,
	OUTPUT_POWER_MID,
	OUTPUT_POWER_HIGH
};

// Packet firmware settings block in EEPROM, in the old DTMF contacts area
// (unused here). All 16 bytes are this firmware's own. Unless the first byte
// is the layout version the whole block is ignored (defaults used), so data
// left there by another firmware is never taken as settings; a single byte
// that is out of range (0xFF when blank) means "use the default".
//
//   0x1D00  layout version (1)
//   0x1D01  squelch level, 0 (open) to 9
//   0x1D02  TX timeout, index into gTxTimeoutSeconds
//   0x1D03  mic gain, REG_7D<4:0>, 0 to 31
//   0x1D04  wide deviation, REG_40<11:0>, u16 little-endian, at most 0xA7F
//   0x1D06  narrow deviation, REG_40<11:0>, u16 little-endian, at most 0xA7F
//   0x1D08  RX AF gain 2, REG_48<9:4>, 0 to 63
//   0x1D09  RX AF DAC gain, REG_48<3:0>, 0 to 15
//   0x1D0A  backlight time, 0 (off) to 7 (always on)
//   0x1D0B  battery type, 0 = 1600 mAh, 1 = 2200 mAh
//   0x1D0C  keypad lock, 0 or 1
//   0x1D0D  reserved (0xFF)
//
// The radio reads this block at power-on, and again (with the channel
// data) about 1 to 1.5 s after the last UART EEPROM write of a session.
#define SETTINGS_PKT_BLOCK        0x1D00u
#define SETTINGS_PKT_VERSION      1u

// Register override table (for experiments without reflashing): 8 entries
// of 8 bytes at 0x1D10..0x1D4F, used only when the settings block above
// carries its layout version. Each entry:
//
//   +0  phase: bit 0 = after the TX set-up (every key-up),
//              bit 1 = after the RX set-up (every return to receive and
//              every squelch open); 0 or 0xFF ends the list
//   +1  BK4819 register; 0xFF ends the list
//   +2  AND mask, u16 little-endian
//   +4  OR value, u16 little-endian
//   +6  reserved (0xFF)
//
// The register becomes (value & mask) | or, written after all of the
// firmware's own writes for that phase, so it wins. Registers that key the
// transmitter, drive the PA, set the frequency or reset or power the chip
// are refused (see RegOverrideAllowed in settings.c). A REG_40 result is
// clamped to PKT_DEVIATION_MAX. The table is read at power-on and after a
// UART EEPROM write session, like the settings.
#define SETTINGS_REG_OVERRIDES    0x1D10u

// Key-up and key-down timing, 8 bytes at 0x1D50 (after the override table),
// used only with a valid settings block like the table. One 8-byte UART
// write changes all of it; the menu does not show it.
//
//   0x1D50  PTT press debounce, ms, 1 to 40 (default 5)
//   0x1D51  PTT release debounce, ms, 2 to 40 (default 5)
//   0x1D52  delay after PA enable, before the PA bias, ms, 1 to 20 (default 1; upstream 5)
//   0x1D53  delay after the PA bias, ms, 0 to 20 (default 2; upstream 10)
//   0x1D54  reserved (0xFF)
#define SETTINGS_TIMING           0x1D50u
#define PA_DELAY_MAX_MS           20u
#define PA_ENABLE_DELAY_MIN_MS    1u
#define PA_ENABLE_DELAY_DEFAULT   1u
#define PA_BIAS_DELAY_DEFAULT     2u
#define REG_OVERRIDE_MAX          8u
#define REG_OVERRIDE_TX           0x01u
#define REG_OVERRIDE_RX           0x02u

typedef struct {
	uint8_t  phase;
	uint8_t  reg;
	uint16_t andMask;
	uint16_t orValue;
} RegOverride_t;

extern RegOverride_t gRegOverrides[REG_OVERRIDE_MAX];
extern uint8_t       gRegOverrideCount;

bool SETTINGS_RegOverrideAllowed(uint8_t reg);

extern const uint8_t gTxTimeoutSeconds[7];
#define TX_TIMEOUT_DEFAULT_INDEX  4u     // 30 s

typedef struct {
	uint8_t               ScreenChannel;  // channel in use (memory or band slot)
	uint8_t               FreqChannel;    // last band slot used
	uint8_t               MrChannel;      // last memory channel used

	uint8_t               SQUELCH_LEVEL;
	uint8_t               TX_TIMEOUT;
	uint8_t               MIC_GAIN;
	uint16_t              DEVIATION_WIDE;
	uint16_t              DEVIATION_NARROW;
	uint8_t               RX_GAIN;
	uint8_t               RX_DAC_GAIN;

	uint8_t               PTT_PRESS_MS;
	uint8_t               PTT_RELEASE_MS;
	uint8_t               PA_ENABLE_DELAY_MS;
	uint8_t               PA_BIAS_DELAY_MS;

	bool                  KEY_LOCK;
	uint8_t               BACKLIGHT_TIME;
	uint8_t               BACKLIGHT_MIN;
	uint8_t               BACKLIGHT_MAX;
	BATTERY_Type_t        BATTERY_TYPE;

	// read only, from calibration and upstream settings
	int16_t               BK4819_XTAL_FREQ_LOW;
	uint8_t               S0_LEVEL;
	uint8_t               S9_LEVEL;

	VFO_Info_t            Vfo;
} EEPROM_Config_t;

extern EEPROM_Config_t gEeprom;

void SETTINGS_InitEEPROM(void);
void SETTINGS_LoadCalibration(void);
void SETTINGS_SaveVfoIndices(void);
void SETTINGS_SaveSettings(void);
void SETTINGS_SaveChannel(const VFO_Info_t *pVFO);

#endif
