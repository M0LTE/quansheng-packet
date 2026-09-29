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

#include <string.h>

#include "app/events.h"
#include "app/uart.h"
#include "app/wire.h"
#include "outq.h"
#include "settings.h"

typedef struct {
	uint8_t ev;
	uint8_t len;                                  // header + payload
	uint8_t body[EVT_HEADER + EVT_PAYLOAD_MAX];
} EvtSlot_t;

Subscription_t gSub;
uint32_t       gCounters[CNT_N];

static EvtSlot_t gRing[EVT_SLOTS];
static uint8_t   gHead;        // next slot to write
static uint8_t   gCount;       // events in the ring
static uint8_t   gUnsent;      // the newest gUnsent of them are not sent yet
static uint16_t  gNextSeq;
static uint16_t  gLostFirst;
static uint16_t  gLostCount;

static EvtSlot_t *Slot(uint8_t fromNewest)    // 0 = newest
{
	return &gRing[(gHead + 2u * EVT_SLOTS - 1u - fromNewest) % EVT_SLOTS];
}

void EVT_Init(void)
{
	gHead = gCount = gUnsent = 0;
	gNextSeq = 0;
	gLostCount = 0;
	EVT_ResetSubscription();
}

void EVT_ResetSubscription(void)
{
	memset(&gSub, 0, sizeof(gSub));
	gSub.mask        = gV2.DEFAULT_MASK & EV_SUPPORTED;
	gSub.options     = gV2.DEFAULT_OPTIONS & SUB_LIVE_TX;
	gSub.heartbeatMs = gV2.DEFAULT_HEARTBEAT_MS;
	gUnsent          = 0;      // a tool taking over the port gets no old events
	gLostCount       = 0;
}

uint16_t EVT_NextSeq(void)   { return gNextSeq; }
uint16_t EVT_OldestSeq(void) { return (uint16_t)(gNextSeq - gCount); }
uint8_t  EVT_Unsent(void)    { return gUnsent; }

static void Header(uint8_t *b, uint16_t seq, uint32_t t_ms, uint8_t flags)
{
	put16(b, seq);
	put32(b + 2, t_ms);
	b[6] = flags;
}

static void StoreRaw(uint8_t ev, uint32_t t_ms, const uint8_t *payload, uint8_t n)
{
	if (gCount == EVT_SLOTS) {
		// the oldest event goes; if it was never sent, it is lost
		if (gUnsent == EVT_SLOTS) {
			if (gLostCount == 0)
				gLostFirst = get16(Slot(EVT_SLOTS - 1)->body);
			gLostCount++;
			gUnsent--;
			gCounters[CNT_EVENTS_LOST]++;
		}
		gCount--;
	}

	EvtSlot_t *s = &gRing[gHead];
	gHead = (gHead + 1) % EVT_SLOTS;
	s->ev  = ev;
	s->len = EVT_HEADER + n;
	Header(s->body, gNextSeq++, t_ms, 0);
	memcpy(s->body + EVT_HEADER, payload, n);
	gCount++;
	gUnsent++;
	gCounters[CNT_EVENTS_STORED]++;
}

void EVT_Store(uint8_t ev, uint32_t t_ms, const uint8_t *payload, uint8_t n)
{
	if (!EVT_Wanted(ev) || n > EVT_PAYLOAD_MAX)
		return;
	StoreRaw(ev, t_ms, payload, n);
}

static void Send(uint8_t ev, const uint8_t *body, uint8_t len, uint8_t flags)
{
	uint8_t *b = UART_FrameBody();
	memcpy(b, body, len);
	b[6] = flags;
	UART_SendFrameBody(EV_ID_BASE + ev, len);
}

static uint8_t TimingFlags(uint32_t t_ms, uint32_t now, bool deferred)
{
	uint8_t f = 0;
	if (OUTQ_Used() > 8 || (!deferred && now - t_ms > 2))
		f |= EVF_QUEUED;
	if (OUTQ_Idle() && now - t_ms <= 1)
		f |= EVF_TIME_EXACT;
	return f;
}

void EVT_Service(bool deferred, uint32_t now)
{
	if (deferred) {
		for (uint8_t i = 0; i < gUnsent; i++)
			Slot(i)->body[6] |= EVF_DEFERRED;
		return;
	}

	while (gUnsent) {
		EvtSlot_t *s = Slot(gUnsent - 1);
		if (OUTQ_Free() < FRAME_OVERHEAD + s->len + REPLY_RESERVE)
			return;
		uint8_t f = s->body[6];
		f |= TimingFlags(get32(s->body + 2), now, f & EVF_DEFERRED);
		if (f & EVF_DEFERRED)
			gCounters[CNT_EVENTS_DEFERRED]++;
		s->body[6] = f;             // replay re-sends what was sent
		Send(s->ev, s->body, s->len, f);
		gUnsent--;
	}

	// EVENTS_LOST goes to every client that takes any stored event
	if (gLostCount && (gSub.mask & EV_STORED_MASK)) {
		uint8_t p[4];
		put16(p, gLostFirst);
		put16(p + 2, gLostCount);
		gLostCount = 0;
		StoreRaw(EV_EVENTS_LOST, now, p, sizeof(p));
	}
}

bool EVT_Ephemeral(uint8_t ev, uint32_t t_ms, const uint8_t *payload, uint8_t n, bool deferred, uint32_t now)
{
	if (!EVT_Wanted(ev))
		return false;

	const uint16_t len = EVT_HEADER + n;
	if (deferred || gUnsent || OUTQ_Free() < FRAME_OVERHEAD + len + REPLY_RESERVE) {
		gCounters[CNT_EPHEMERAL_DROPPED]++;
		return false;
	}

	uint8_t *b = UART_FrameBody();
	memcpy(b + EVT_HEADER, payload, n);
	if (ev == EV_HEARTBEAT) {
		// t_ms and t_us are when the frame was queued: exact if nothing was ahead
		uint16_t us;
		const bool exact = UART_SendStampedHeader(len, &t_ms, &us);
		Header(b, gNextSeq, t_ms, EVF_EPHEMERAL | (exact ? EVF_TIME_EXACT : 0));
		put16(b + EVT_HEADER, us);
		UART_SendStampedBody(EV_ID_BASE + ev, len);
	}
	else {
		Header(b, gNextSeq, t_ms, EVF_EPHEMERAL | TimingFlags(t_ms, now, false));
		UART_SendFrameBody(EV_ID_BASE + ev, len);
	}
	return true;
}

// Re-send stored events already delivered, oldest first, from from_seq on,
// as many as fit in EVT_REPLAY_BUDGET bytes and the output queue. Events
// not delivered yet are left to the normal delivery.
void EVT_Replay(uint16_t from, uint16_t *first, uint8_t *count)
{
	uint16_t budget = OUTQ_Free();
	budget = (budget > REPLY_RESERVE) ? budget - REPLY_RESERVE : 0;
	if (budget > EVT_REPLAY_BUDGET)
		budget = EVT_REPLAY_BUDGET;

	*first = gNextSeq;
	*count = 0;
	for (uint8_t i = gCount; i > gUnsent; i--) {
		const EvtSlot_t *s = Slot(i - 1);
		const uint16_t seq = get16(s->body);
		if ((int16_t)(seq - from) < 0)
			continue;
		if (FRAME_OVERHEAD + s->len > budget)
			break;
		budget -= FRAME_OVERHEAD + s->len;
		if (*count == 0)
			*first = seq;
		(*count)++;
		Send(s->ev, s->body, s->len, s->body[6] | EVF_REPLAY);
	}
}
