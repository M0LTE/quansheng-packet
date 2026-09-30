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

// Serial control protocol v2 commands (docs/protocol-v2.md 6). Requests are
// 0x5000 to 0x507F; the reply id is the request id + 0x80, also for errors.
// Nothing here keys the transmitter: 0x5020 (serial keying) stays reserved.

#ifndef APP_V2_H
#define APP_V2_H

#include <stdbool.h>
#include <stdint.h>

#define V2_MAGIC             0x32544B50u    // "PKT2", in the 0x0515 hello reply
#define V2_PROTOCOL_VERSION  0x0200u
#define V2_MAX_REQUEST_BODY  120u

enum {
	V2_OK = 0,
	V2_UNKNOWN_CMD,
	V2_BAD_LENGTH,
	V2_BAD_PARAM,
	V2_RANGE,
	V2_TX_BAND,
	V2_STATE,
	V2_REFUSED,
	V2_UNSUPPORTED,
	V2_EEPROM,
	V2_NOT_PERSISTABLE,
};

enum {
	V2_GET_INFO = 0x5000,
	V2_GET_STATUS,
	V2_SUBSCRIBE,
	V2_TIME_SYNC,
	V2_GET_PARAMS,
	V2_SET_PARAMS,
	V2_SAVE_PARAMS,
	V2_LEVEL_TONE,
	V2_REG_READ,
	V2_REG_WRITE,
	V2_REG_OVERRIDE,
	V2_EVENT_REPLAY,
	V2_GET_COUNTERS,
};

// GET_INFO capability bits
#define CAP_LIVE_TX          (1u << 0)
#define CAP_RSSI_BUSY        (1u << 1)
#define CAP_TONE_RAW         (1u << 2)
#define CAP_TONE_DEVIATION   (1u << 3)
#define CAP_RAM_OVERRIDES    (1u << 8)
#define CAP_PERSISTENCE      (1u << 9)
#define CAP_EXACT_TIME_SYNC  (1u << 10)
#define CAP_RAW_REG_WRITE    (1u << 11)     // bench build: legacy 0x0602 built in

void    V2_Init(void);
void    V2_Handle(uint16_t id, const uint8_t *body, uint16_t bodyLen, uint16_t payloadLen);
uint8_t V2_Flags1(void);
uint8_t V2_State(void);

#endif
