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

// Protocol v2 events (docs/protocol-v2.md 8): the subscription, the ring
// of stored events and their delivery, ephemeral events, replay, and the
// protocol counters.
//
// A stored event is kept serialized (its 7-byte header and payload) in a
// slot of the ring and gets the next sequence number, but only if the
// live subscription asks for it: a client never sees gaps for events it
// did not subscribe to. Delivery sends the oldest unsent event whenever
// the output queue has room for it plus the largest reply, and holds
// events back while PTT is asserted or the radio transmits (unless the
// host chose LIVE_TX). Replay re-sends events already delivered.

#ifndef APP_EVENTS_H
#define APP_EVENTS_H

#include <stdbool.h>
#include <stdint.h>

enum {
	EV_CD = 0,
	EV_RX_BURST,
	EV_TX_START,
	EV_TX_END,
	EV_TX_REFUSED,
	EV_RSSI_STREAM,
	EV_HEARTBEAT,
	EV_BATTERY,
	EV_PARAMS_CHANGED,
	EV_EVENTS_LOST,
	EV_TONE_END,
	EV_OVERRIDE_EXPIRED,
	EV_BOOT,
	EV_N
};

#define EV_ID_BASE         0x50C0u
#define EV_SUPPORTED       ((1u << EV_N) - 1u)
#define EV_EPHEMERAL_MASK  ((1u << EV_RSSI_STREAM) | (1u << EV_HEARTBEAT))
#define EV_STORED_MASK     (EV_SUPPORTED & ~EV_EPHEMERAL_MASK)

#define EVF_REPLAY         0x01u
#define EVF_DEFERRED       0x02u
#define EVF_QUEUED         0x04u
#define EVF_EPHEMERAL      0x08u
#define EVF_TIME_EXACT     0x10u

#define EVT_HEADER         7u
#define EVT_PAYLOAD_MAX    29u      // RX_BURST
#define EVT_SLOTS          20u      // 20 x 36 bytes: at least 16 of the largest
#define EVT_REPLAY_BUDGET  256u     // bytes of replayed frames per EVENT_REPLAY
#define FRAME_OVERHEAD     12u      // framing 8 + id and length 4
#define REPLY_RESERVE      150u     // room kept in the output queue for a reply

#define SUB_LIVE_TX        0x01u
#define SUB_PERSIST        0x02u

typedef struct {
	uint32_t mask;
	uint8_t  options;
	uint16_t heartbeatMs;
	uint8_t  rssiPeriodMs;
	uint8_t  rssiBatch;
	uint8_t  burstPeriodMs;     // 0: default (5)
} Subscription_t;

extern Subscription_t gSub;

enum {
	CNT_FRAMES_OK = 0,
	CNT_FRAMES_BAD,             // bad CRC, bad footer, truncated
	CNT_FRAMES_DROPPED,         // oversize
	CNT_REPLIES_NOT_OK,
	CNT_EVENTS_STORED,
	CNT_EVENTS_LOST,
	CNT_EVENTS_DEFERRED,
	CNT_TRANSMISSIONS,
	CNT_TX_TIMEOUTS,
	CNT_TX_REFUSED,
	CNT_BUSY_OPENS,
	CNT_LATE_KEYS,
	CNT_EPHEMERAL_DROPPED,
	CNT_EEPROM_BLOCKS,          // kept by the EEPROM driver, filled in on reading
	CNT_N
};

extern uint32_t gCounters[CNT_N];

static inline bool EVT_Wanted(uint8_t ev) { return (gSub.mask >> ev) & 1u; }

void     EVT_Init(void);
void     EVT_ResetSubscription(void);         // back to the stored default (power-on, legacy hello)
void     EVT_Store(uint8_t ev, uint32_t t_ms, const uint8_t *payload, uint8_t n);
bool     EVT_Ephemeral(uint8_t ev, uint32_t t_ms, const uint8_t *payload, uint8_t n, bool deferred, uint32_t now);
void     EVT_Service(bool deferred, uint32_t now);
uint16_t EVT_NextSeq(void);
uint16_t EVT_OldestSeq(void);
uint8_t  EVT_Unsent(void);
void     EVT_Replay(uint16_t from, uint16_t *first, uint8_t *count);

#endif
