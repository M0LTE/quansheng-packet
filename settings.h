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
//   0x1D01  busy detector level, 1 to 9 (default 1): which row of the
//           factory squelch tables the chip's carrier detector uses. There
//           is no squelch: it never mutes the audio. (Until 30 September
//           2026 this byte was the squelch level; 0 now means 1.)
//   0x1D02  TX timeout, index into gTxTimeoutSeconds
//   0x1D03  reserved: was the mic gain (retired, ignored, written 0xFF)
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
//              every receive set-up); 0 or 0xFF ends the list
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

// The operating channel, 8 bytes at 0x1D58 (after the timing), used only
// with a valid settings block. There are no memory channels or band slots:
// this is the one frequency the radio uses, with its power, bandwidth and
// step, as the keypad, the menu or the protocol last stored them.
//
//   0x1D58  frequency, u32 little-endian, 10 Hz units; must be receivable
//           (inside the band table), else the whole block is unused
//   0x1D5C  power, 0 low, 1 mid, 2 high (default 0)
//   0x1D5D  bandwidth, 0 wide, 1 narrow (default 0)
//   0x1D5E  step, index into gStepFrequencyTable (default 12.5 kHz)
//   0x1D5F  reserved (0xFF)
//
// When the block is unused (a radio coming from another firmware, or from
// the channel-memory builds of this one), the frequency in use in the old
// upstream layout (channel indices at 0x0E80 and the record they point at)
// is taken once if it is receivable, else 144.800 MHz; with a valid
// settings block it is then written here, and the old layout is never read
// again.
#define SETTINGS_OPERATING        0x1D58u
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

// the EEPROM table as loaded, and the RAM trial table (protocol v2
// REG_OVERRIDE), applied after it in each phase
extern RegOverride_t gRegOverrides[REG_OVERRIDE_MAX];
extern uint8_t       gRegOverrideCount;
extern RegOverride_t gRegOverridesRam[REG_OVERRIDE_MAX];
extern uint8_t       gRegOverrideRamCount;

// Protocol v2 settings, 16 bytes at 0x1D60 (docs/protocol-v2.md 7.1), used
// only when the settings block above is valid and 0x1D60 holds its layout
// version. A byte out of range means "default". The first menu save over
// foreign data at 0x1D00 blanks it, with the override table and timing.
//
//   0x1D60  layout version (1)
//   0x1D61  serial PTT lock, 10 ms units, 0 to 150 (default 2 = 20 ms)
//   0x1D62  busy source, 1 to 3 (default 1, the chip's squelch detector)
//   0x1D63  busy hang, ms, 0 to 250 (default 20)
//   0x1D64  busy RSSI open, u16, 0 to 511 (default 110)
//   0x1D66  busy RSSI close, u16, at most open (default 104)
//   0x1D68  default event mask, u32 (blank = 0)
//   0x1D6C  default heartbeat, ms, u16: 0 or 100 to 60000 (default 0)
//   0x1D6E  default subscribe options, bit 0 LIVE_TX (default 0)
//   0x1D6F  tone calibration: gain code for 3 kHz deviation at 1 kHz,
//           1 to 127 (blank = none)
#define SETTINGS_V2_BLOCK         0x1D60u
#define SETTINGS_V2_VERSION       1u
#define BUSY_SOURCE_SQUELCH       0x01u
#define BUSY_SOURCE_RSSI          0x02u
#define BUSY_RSSI_OPEN_DEFAULT    110u
#define BUSY_RSSI_CLOSE_DEFAULT   104u
#define BUSY_HANG_DEFAULT_MS      20u

typedef struct {
	uint16_t SERIAL_LOCK_MS;
	uint8_t  BUSY_SOURCE;
	uint8_t  BUSY_HANG_MS;
	uint16_t BUSY_RSSI_OPEN;
	uint16_t BUSY_RSSI_CLOSE;
	uint32_t DEFAULT_MASK;
	uint16_t DEFAULT_HEARTBEAT_MS;
	uint8_t  DEFAULT_OPTIONS;
	uint8_t  TONE_CAL;          // 0: none
	bool     valid;             // the block is in use
} V2_Config_t;

extern V2_Config_t gV2;
extern bool        gSettingsBlockValid;

bool SETTINGS_RegOverrideAllowed(uint8_t reg);

extern const uint8_t gTxTimeoutSeconds[7];
#define TX_TIMEOUT_DEFAULT_INDEX  4u     // 30 s

typedef struct {
	uint8_t               BUSY_LEVEL;         // busy detector level, 1 to 9
	uint8_t               TX_TIMEOUT;
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
// decoding shared by the power-on load and the protocol's stored reads;
// Data and T are the 0x1D00 and 0x1D50 blocks, 0xFF-filled if not valid
void SETTINGS_Decode(const uint8_t Data[16], const uint8_t T[8], EEPROM_Config_t *e);
void SETTINGS_DecodeV2(const uint8_t b[16], bool valid, V2_Config_t *v);
uint8_t SETTINGS_FactoryRxGain(void);
void SETTINGS_LoadCalibration(void);
void SETTINGS_SaveSettings(void);
void SETTINGS_SaveOperating(void);                  // gVfo's frequency, power, bandwidth, step
bool SETTINGS_DecodeOperating(const uint8_t b[8], VFO_Info_t *v);
void SETTINGS_ImportOldFrequency(VFO_Info_t *v);    // read only: the old upstream layout

#endif
