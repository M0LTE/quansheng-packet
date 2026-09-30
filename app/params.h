/* Copyright 2026 packet-fw contributors
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

// Protocol v2 parameters (docs/protocol-v2.md 7): GET_PARAMS, SET_PARAMS,
// SAVE_PARAMS, and the EEPROM writes they queue. Values are applied in RAM;
// persisting writes one 8-byte block per main-loop pass, never during a
// transmission or while a key-up is pending, and never at 0x1E00 and up
// (the EEPROM driver refuses those anyway).

#ifndef APP_PARAMS_H
#define APP_PARAMS_H

#include <stdbool.h>
#include <stdint.h>

enum {
	P_FREQ_HZ = 0x01,
	P_POWER,
	P_BANDWIDTH,
	P_DEV_WIDE,
	P_DEV_NARROW,
	P_RETIRED_MIC_GAIN,    // 0x06: MIC_GAIN, gone (fixed at the maximum); replies UNSUPPORTED
	P_RETIRED_SQUELCH,     // 0x07: SQUELCH, gone (no squelch); replies UNSUPPORTED
	P_RX_GAIN,
	P_RX_DAC_GAIN,
	P_TX_TIMEOUT_S,
	P_PTT_PRESS_MS,
	P_PTT_RELEASE_MS,
	P_PA_ENABLE_DELAY_MS,
	P_PA_BIAS_DELAY_MS,
	P_SERIAL_LOCK_MS,
	P_BUSY_SOURCE,
	P_BUSY_RSSI_OPEN,
	P_BUSY_RSSI_CLOSE,
	P_BUSY_HANG_MS,
	P_BUSY_SQL_RAW,        // 0x14: the busy detector's raw thresholds (was SQL_RAW)
	P_AGC_FIX,
	P_AFC,
	P_BACKLIGHT,
	P_KEY_LOCK,
	P_BUSY_SQL_LEVEL,      // 0x19: the busy detector level, 1 to 9
	P_LAST = P_BUSY_SQL_LEVEL
};

// retired ids: GET_PARAMS and SET_PARAMS reply UNSUPPORTED with the id as detail
#define PARAMS_RETIRED    ((1u << P_RETIRED_MIC_GAIN) | (1u << P_RETIRED_SQUELCH))
#define PARAMS_SUPPORTED  (((1u << (P_LAST + 1)) - 1u) & ~1u & ~PARAMS_RETIRED)
#define PARAMS_IS_RETIRED(id)  ((id) <= P_LAST && ((PARAMS_RETIRED >> (id)) & 1u))
#define PARAMS_RAM_ONLY   ((1u << P_BUSY_SQL_RAW) | (1u << P_AGC_FIX) | (1u << P_AFC))

// SET_PARAMS flags and result bits
#define SETP_PERSIST      0x01u
#define SETP_REQUIRE_TX   0x02u
#define SETP_DRY_RUN      0x04u
#define SETR_TX_ALLOWED   0x01u
#define SETR_PERSIST      0x02u
#define SETR_RETUNED      0x04u

// PARAMS_CHANGED sources
enum { PSRC_KEYPAD = 0, PSRC_RELOAD, PSRC_SERIAL, PSRC_BOOT };

void     PARAMS_Init(void);
uint8_t  PARAMS_Size(uint8_t id);                  // 0: unknown
uint8_t  PARAMS_Get(uint8_t id, bool stored, uint8_t *out);   // bytes written

// Returns a v2 status; on error *detail is the offending id. out/outLen:
// the reply after the reply header (result byte and read-back records).
uint8_t  PARAMS_Set(uint8_t flags, const uint8_t *rec, uint16_t n, uint8_t *detail, uint8_t *out, uint16_t *outLen);
uint8_t  PARAMS_Save(uint8_t op, uint32_t *mask);

bool     PARAMS_PersistPending(void);
void     PARAMS_CancelPersist(void);
bool     PARAMS_LiveDiffers(void);
void     PARAMS_PersistService(bool allowed);      // one block per call
bool     PARAMS_PersistSubscription(uint32_t mask, uint8_t options, uint16_t heartbeatMs);
bool     PARAMS_PersistOverrides(void);            // the EEPROM table as loaded (gRegOverrides)
void     PARAMS_Changed(uint8_t source);           // PARAMS_CHANGED for what differs from the last look
// Re-read the stored view now. Not needed after an EEPROM write (the
// view follows those by itself); for a change in how EEPROM is decoded,
// such as the reload after legacy UART writes.
void     PARAMS_RefreshStored(void);

#endif
