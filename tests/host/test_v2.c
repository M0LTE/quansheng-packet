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

// Host tests of the serial control protocol v2 (docs/protocol-v2.md) on
// the real firmware code (harness.h): framing and the reply header, every
// implemented command's byte layout, events (busy edges and bursts, TX
// events, deferral while PTT is held, the ring, replay, EVENTS_LOST,
// heartbeats, the RSSI stream), parameters (atomic, validated, RAM by
// default, persisted one block per pass and never during TX), register
// access and overrides, the level tone, and the calibration guard.
//
// Run with a path argument, it writes the golden vectors it recorded
// (tests/vectors/README.md).

#include <stdlib.h>
#include <string.h>

#include "app/events.h"
#include "app/monitor.h"
#include "app/params.h"
#include "app/uart.h"
#include "app/v2.h"
#include "app/wire.h"
#include "driver/eeprom.h"
#include "functions.h"
#include "misc.h"
#include "radio.h"
#include "settings.h"

#include "harness.h"

static const uint32_t session = 0x12345678;

// ------------------------------------------------------------ vectors --

typedef struct {
	const char *name;
	const char *state;
	bool        reqObf, respObf;
	uint8_t     req[320];
	unsigned    reqn;
	uint8_t     resp[2048];
	unsigned    respn;
} Vec_t;

static Vec_t    vecs[64];
static int      nvec;
static bool     reqObf;

static void vec(const char *name, const char *state)
{
	if (nvec >= 64) return;
	Vec_t *v = &vecs[nvec++];
	v->name = name;
	v->state = state;
	v->reqObf = reqObf;
	v->respObf = host_obfuscated();
	memcpy(v->req, last_req, last_req_len);
	v->reqn = last_req_len;
	v->respn = out_len > sizeof(v->resp) ? sizeof(v->resp) : out_len;
	memcpy(v->resp, out, v->respn);
}

static void hex(FILE *f, const uint8_t *p, unsigned n)
{
	for (unsigned i = 0; i < n; i++) fprintf(f, "%02X", p[i]);
}

static void write_vectors(const char *path)
{
	FILE *f = fopen(path, "w");
	if (!f) { printf("cannot write %s\n", path); failures++; return; }
	fprintf(f, "{\n  \"format\": \"packet-fw protocol v2 golden vectors, version 1\",\n");
	fprintf(f, "  \"spec\": \"docs/protocol-v2.md\",\n");
	fprintf(f, "  \"generator\": \"tests/host/test_v2.c on the real firmware code\",\n");
	fprintf(f, "  \"firmware_version_string\": \"PKTFW test\",\n");
	fprintf(f, "  \"vectors\": [\n");
	for (int i = 0; i < nvec; i++) {
		Vec_t *v = &vecs[i];
		fprintf(f, "    {\n      \"name\": \"%s\",\n      \"state\": \"%s\",\n", v->name, v->state);
		fprintf(f, "      \"request_mode\": \"%s\",\n      \"request\": \"", v->reqObf ? "obfuscated" : "plain");
		hex(f, v->req, v->reqn);
		fprintf(f, "\",\n      \"response_mode\": \"%s\",\n      \"response\": \"", v->respObf ? "obfuscated" : "plain");
		hex(f, v->resp, v->respn);
		fprintf(f, "\",\n      \"frames\": [");
		// decode the response frames for the reader
		memcpy(out, v->resp, v->respn);
		out_len = v->respn;
		const bool save = host_obfuscated();
		host_set_obfuscated(v->respObf);
		Frame_t fr[32];
		const int n = host_frames(fr, 32);
		host_set_obfuscated(save);
		for (int k = 0; k < n; k++) {
			fprintf(f, "%s\n        { \"id\": \"0x%04X\", \"crc\": \"%s\", \"body\": \"", k ? "," : "", fr[k].id,
			        fr[k].crc_real ? "real" : fr[k].crc_ff ? "FFFF" : "bad");
			hex(f, fr[k].body, fr[k].body_len);
			fprintf(f, "\" }");
		}
		fprintf(f, "%s]\n    }%s\n", n ? "\n      " : "", i + 1 < nvec ? "," : "");
	}
	fprintf(f, "  ]\n}\n");
	fclose(f);
}

// ------------------------------------------------------------ helpers --

static Frame_t fr[64];
static int     nfr;

static void boot_plain(void)
{
	host_boot();
	host_clear_out();
	host_send_mode(0x0514, &session, 4, false);   // raw 14 05: plain mode
	host_clear_out();
	reqObf = false;
}

static void valid_settings_block(void)
{
	eeprom[SETTINGS_PKT_BLOCK] = SETTINGS_PKT_VERSION;
	host_boot_keep_eeprom();
	host_clear_out();
	host_send_mode(0x0514, &session, 4, false);
	host_clear_out();
	reqObf = false;
}

static uint8_t  status(const Frame_t *f)  { return f ? f->body[1] : 0xEE; }
static uint16_t lockms(const Frame_t *f)  { return get16(f->body + 2); }
static const uint8_t *rb(const Frame_t *f) { return f->body + 4; }

static int events(uint8_t ev, const Frame_t **first)
{
	int n = 0;
	*first = NULL;
	nfr = host_frames(fr, 64);
	for (int i = 0; i < nfr; i++)
		if (fr[i].id == EV_ID_BASE + ev) {
			if (!n) *first = &fr[i];
			n++;
		}
	return n;
}

static const Frame_t *subscribe(uint32_t mask, uint8_t options, uint16_t hb, uint8_t period, uint8_t batch, uint8_t burst)
{
	uint8_t r[10];
	put32(r, mask);
	r[4] = options;
	put16(r + 5, hb);
	r[7] = period;
	r[8] = batch;
	r[9] = burst;
	return v2(V2_SUBSCRIBE, 0x33, r, 10);
}

static void squelch(bool open)
{
	regs[0x0C] = open ? (regs[0x0C] | 2u) : (regs[0x0C] & ~2u);
}

// -------------------------------------------------------------- tests --

static void test_framing(void)
{
	boot_plain();

	// GET_INFO: 40 bytes after the reply header, real CRC, id + 0x80
	const Frame_t *f = v2(V2_GET_INFO, 0x01, NULL, 0);
	CHECK(f && f->id == 0x5080 && f->crc_real && !f->crc_ff);
	CHECK(f->body_len == 44 && f->body[0] == 0x01 && status(f) == V2_OK);
	CHECK(lockms(f) == SERIAL_LOCK_DEFAULT_MS);   // the frame itself started the lock
	const uint8_t *o = rb(f);
	CHECK(get16(o) == 0x0200);
	CHECK(memcmp(o + 2, "PKTFW test\0\0\0\0\0\0", 16) == 0);
	CHECK(get32(o + 18) == (CAP_LIVE_TX | CAP_RSSI_BUSY | CAP_TONE_RAW | CAP_RAM_OVERRIDES | CAP_PERSISTENCE | CAP_EXACT_TIME_SYNC));
	CHECK(get32(o + 22) == 0x01FFFFFEu);
	CHECK(get32(o + 26) == 0x1FFFu);
	CHECK(o[30] == 120 && o[31] >= 16);
	CHECK(get16(o + 32) == 20 && o[34] == 30);
	CHECK(o[35] == 0 && o[36] == 0x08);           // F_LOCK default, 350 MHz receive enabled
	CHECK(o[37] == 0 && o[38] == 0 && o[39] == 5); // blank EEPROM: no settings block
	vec("get_info", "fresh boot on a blank EEPROM (factory calibration only), plain mode after a hello; lock 20 ms");

	// the same in obfuscated mode
	host_clear_out();
	host_send_mode(0x0514, &session, 4, true);     // raw 02 69: back to obfuscated
	CHECK(host_obfuscated());
	reqObf = true;
	f = v2(V2_GET_INFO, 0x02, NULL, 0);
	CHECK(f && f->crc_real && status(f) == V2_OK && f->body_len == 44);
	vec("get_info_obfuscated", "as get_info, but obfuscated mode (after an obfuscated hello)");
	host_send_mode(0x0514, &session, 4, false);
	reqObf = false;

	// an undefined request id: UNKNOWN_CMD, reply id + 0x80, detail 0
	f = v2(0x500D, 0x07, NULL, 0);
	CHECK(f && f->id == 0x508D && status(f) == V2_UNKNOWN_CMD && f->body_len == 5 && rb(f)[0] == 0);
	vec("unknown_command", "any state; 0x500D is not defined");
	f = v2(0x5020, 0x08, NULL, 0);                 // reserved serial keying: never defined
	CHECK(f && f->id == 0x50A0 && status(f) == V2_UNKNOWN_CMD);
	f = v2(0x507F, 0x09, NULL, 0);
	CHECK(f && f->id == 0x50FF && status(f) == V2_UNKNOWN_CMD);

	// ids 0x5080 and up are not requests: no reply
	host_clear_out();
	const uint8_t tag = 1;
	host_send(0x5080, &tag, 1);
	CHECK(host_frames(fr, 64) == 0);

	// no tag at all: BAD_LENGTH with tag 0
	host_clear_out();
	host_send(V2_GET_INFO, NULL, 0);
	nfr = host_frames(fr, 64);
	CHECK(nfr == 1 && fr[0].id == 0x5080 && fr[0].body[0] == 0 && status(&fr[0]) == V2_BAD_LENGTH);

	// body_len that does not match the frame length: BAD_LENGTH
	{
		uint8_t p[16] = { 0x00, 0x50, 0x03, 0x00, 0x05 };   // says 3, carries 1
		uint16_t crc = 0;
		for (unsigned i = 0; i < 5; i++) {
			crc ^= (uint16_t)p[i] << 8;
			for (int b = 0; b < 8; b++) crc = (crc & 0x8000) ? (crc << 1) ^ 0x1021 : crc << 1;
		}
		uint8_t fbytes[16] = { 0xAB, 0xCD, 5, 0, p[0], p[1], p[2], p[3], p[4], crc & 0xFF, crc >> 8, 0xDC, 0xBA };
		host_clear_out();
		host_rx(fbytes, 13);
		host_poll();
		nfr = host_frames(fr, 64);
		CHECK(nfr == 1 && fr[0].body[0] == 0x05 && status(&fr[0]) == V2_BAD_LENGTH);
	}

	// wrong fixed length, and a body over 120 bytes
	uint8_t big[130] = { 0 };
	f = v2(V2_GET_INFO, 0x0A, big, 1);
	CHECK(f && status(f) == V2_BAD_LENGTH);
	f = v2(V2_GET_PARAMS, 0x0B, big, 120);          // 121 with the tag
	CHECK(f && status(f) == V2_BAD_LENGTH);
	vec("bad_length", "GET_PARAMS with a 121-byte body (tag included): over the 120-byte limit");
	CHECK(gCounters[CNT_REPLIES_NOT_OK] >= 6);
}

static void test_status(void)
{
	boot_plain();
	regs[0x67] = 0x0123; regs[0x65] = 0x2F49; regs[0x63] = 0x5B; regs[0x7E] = 0xB7C0;
	const Frame_t *f = v2(V2_GET_STATUS, 0x10, NULL, 0);
	CHECK(f && f->id == 0x5081 && status(f) == V2_OK && f->body_len == 38);
	const uint8_t *o = rb(f);
	CHECK(get32(o) == g_ms);
	CHECK(get32(o + 4) == 144800000u);             // blank 2 m band slot
	CHECK(o[8] == 0);                              // idle
	CHECK(o[9] == (0x08 | 0x10));                  // lock active, TX allowed
	CHECK(o[10] == 0);                             // blank EEPROM: live equals stored defaults
	CHECK(o[11] == 0 && o[12] == 0 && o[13] == 1);
	CHECK(get16(o + 14) == 0x0856);
	CHECK(get16(o + 16) == 0x0123 && o[18] == 0x49 && o[19] == 0x5B);
	CHECK(o[20] == (0x80 | 3));                    // AGC fix bit and index
	CHECK(o[21] == 5 && get16(o + 22) == 7800);
	CHECK(get16(o + 24) == 20);
	CHECK(get16(o + 26) == 0xFFFF);
	CHECK(get16(o + 30) == EVT_NextSeq());
	CHECK(o[32] == 202 && o[33] == 30);
	vec("get_status", "fresh boot, blank EEPROM; REG_67=0x0123 REG_65=0x2F49 REG_63=0x005B REG_7E=0xB7C0; battery 7.80 V level 5; clock 1000 ms");
}

static void test_subscribe_and_busy(void)
{
	boot_plain();

	// ranges
	CHECK(status(subscribe(1, 0, 50, 0, 0, 0)) == V2_RANGE);
	CHECK(status(subscribe(1, 0, 0, 4, 1, 0)) == V2_RANGE);
	CHECK(status(subscribe(1, 0, 0, 10, 0, 0)) == V2_RANGE);
	CHECK(status(subscribe(1, 0, 0, 10, 21, 0)) == V2_RANGE);
	CHECK(status(subscribe(1, 0, 0, 0, 0, 1)) == V2_RANGE);
	CHECK(status(subscribe(1, SUB_PERSIST, 0, 0, 0, 0)) == V2_EEPROM);   // no settings block

	const Frame_t *f = subscribe((1u << EV_CD) | (1u << EV_RX_BURST), 0, 0, 0, 0, 0);
	CHECK(f && f->id == 0x5082 && status(f) == V2_OK && f->body_len == 12);
	const uint16_t next = get16(rb(f));
	CHECK(get16(rb(f) + 2) == next);                // empty ring: oldest == next
	CHECK(get32(rb(f) + 4) == g_ms);
	CHECK(gSub.mask == 3);
	vec("subscribe", "fresh boot; SUBSCRIBE mask CD|RX_BURST, no heartbeat, no stream; ring empty");

	// the lock runs out; the squelch opens: CD open at once
	host_advance(30);
	host_clear_out();
	regs[0x67] = 200; regs[0x65] = 0x0010; regs[0x63] = 0x20; regs[0x64] = 0x0400;
	squelch(true);
	const uint32_t tOpen = g_ms + 1;
	host_advance(1);
	const Frame_t *e;
	CHECK(events(EV_CD, &e) == 1);
	CHECK(e->crc_real && e->body_len == 14);
	CHECK(get16(e->body) == next);
	CHECK(get32(e->body + 2) == tOpen);
	CHECK(e->body[6] == EVF_TIME_EXACT);
	CHECK(e->body[7] == 1 && e->body[8] == 1 && e->body[9] == CD_CAUSE_SQUELCH);
	CHECK(get16(e->body + 10) == 200 && e->body[12] == 0x10 && e->body[13] == 0x20);
	memcpy(out, out + e->offset, e->size); out_len = e->size;
	last_req_len = 0;
	vec("event_cd_open", "event, no request: squelch opened at t=1031 ms; REG_67=200 REG_65=0x0010 REG_63=0x0020; first stored event after SUBSCRIBE");

	// samples every 5 ms while busy, then the close: CD, then RX_BURST
	host_clear_out();
	regs[0x67] = 220; host_advance(5);
	regs[0x67] = 180; host_advance(5);
	regs[0x67] = 210; regs[0x64] = 0x0600; host_advance(5);
	squelch(false);
	host_advance(1);
	nfr = host_frames(fr, 64);
	CHECK(nfr == 2 && fr[0].id == 0x50C0 && fr[1].id == 0x50C1);
	CHECK(fr[0].body[7] == 0 && fr[0].body[9] == CD_CAUSE_SQUELCH);
	CHECK(get16(fr[1].body) == next + 2 && fr[1].body_len == 36);
	const uint8_t *b = fr[1].body + 7;
	CHECK(get32(b) == tOpen);
	CHECK(get32(b + 4) == 16);
	CHECK(get16(b + 8) == 4);                        // the open sample and three more
	CHECK(get16(b + 10) == (200 + 220 + 180 + 210) / 4);
	CHECK(get16(b + 12) == 220 && get16(b + 14) == 180);
	CHECK(b[16] == 0x10 && b[17] == 0x10 && b[18] == 0x20 && b[19] == 0x20);
	CHECK(get16(b + 20) == (0x400 * 3 + 0x600) / 4 && get16(b + 22) == 0x600);
	CHECK(get16(b + 26) == 0x7FFF && b[28] == CD_CAUSE_SQUELCH);
	last_req_len = 0;
	vec("event_cd_close_and_rx_burst", "events, no request: the squelch closed 16 ms after opening; RSSI samples 200, 220, 180, 210 at 5 ms; AF amplitude 0x400, 0x400, 0x400, 0x600");

	// burst sample period and RSSI source with hang
	f = subscribe(3, 0, 0, 0, 0, 2);
	CHECK(status(f) == V2_OK);
	gV2.BUSY_SOURCE = BUSY_SOURCE_RSSI;
	gV2.BUSY_HANG_MS = 10;
	regs[0x67] = 50;
	host_advance(40);
	host_clear_out();
	regs[0x67] = 110;                                // at the open threshold
	host_advance(2);
	CHECK(events(EV_CD, &e) == 1 && e->body[7] == 1 && e->body[9] == CD_CAUSE_RSSI);
	host_clear_out();
	regs[0x67] = 104;                                // between: not below close
	host_advance(20);
	CHECK(events(EV_CD, &e) == 0);
	regs[0x67] = 103;                                // below close: closes after the hang
	host_advance(9);
	CHECK(events(EV_CD, &e) == 0);
	host_advance(4);
	CHECK(events(EV_CD, &e) == 1 && e->body[7] == 0 && e->body[9] == CD_CAUSE_RSSI);
	CHECK(events(EV_RX_BURST, &e) == 1 && get16(e->body + 7 + 8) >= 15);   // 2 ms sampling
	gV2.BUSY_SOURCE = BUSY_SOURCE_SQUELCH;

	// unsubscribed events are not stored and take no sequence number
	const uint16_t seq = EVT_NextSeq();
	subscribe(0, 0, 0, 0, 0, 0);
	squelch(true); host_advance(3); squelch(false); host_advance(3);
	CHECK(EVT_NextSeq() == seq);

	// a retune forces busy closed (cause RETUNE)
	subscribe(1, 0, 0, 0, 0, 0);
	host_advance(25);
	squelch(true); host_advance(2);
	host_clear_out();
	MON_Retune();
	host_advance(1);
	CHECK(events(EV_CD, &e) >= 1 && e->body[7] == 0 && (e->body[9] & CD_CAUSE_RETUNE));
	squelch(false);
}

static void test_deferral_and_tx_events(void)
{
	boot_plain();
	subscribe(EV_STORED_MASK, 0, 0, 0, 0, 0);
	host_advance(30);
	host_clear_out();

	// PTT held: a busy edge is stored but not sent
	host_ptt.pressed = true;
	squelch(true);
	host_advance(5);
	const Frame_t *e;
	CHECK(events(EV_CD, &e) == 0);
	CHECK(EVT_Unsent() == 1);

	// the key-up: busy closed (TX), TX_START at RF ready
	gCurrentFunction = FUNCTION_TRANSMIT;
	MON_KeyInfo(g_ms - 5, 12, true, true);
	MON_ForceClose(CD_CAUSE_TX);
	const uint32_t tStart = g_ms;
	MON_TxStarted();
	host_advance(100);
	CHECK(host_frames(fr, 64) == 0);                 // held while transmitting
	CHECK(EVT_Unsent() == 4);                        // CD open, CD close, RX_BURST, TX_START

	// release: TX_END once the receiver is set up; events 2 ms later, DEFERRED
	host_ptt.pressed = false;
	gCurrentFunction = FUNCTION_FOREGROUND;
	gTxCarrierOffMs = g_ms;
	MON_TxEnded(TXEND_RELEASE, g_ms - 5);
	squelch(false);
	host_advance(1);
	CHECK(host_frames(fr, 64) == 0);
	host_advance(2);
	nfr = host_frames(fr, 64);
	CHECK(nfr == 5);
	for (int i = 0; i < nfr; i++) CHECK(fr[i].body[6] & EVF_DEFERRED);
	for (int i = 1; i < nfr; i++) CHECK(get16(fr[i].body) == (uint16_t)(get16(fr[0].body) + i));
	CHECK(fr[1].id == 0x50C0 && (fr[1].body[9] & CD_CAUSE_TX));
	CHECK(fr[3].id == 0x50C2 && fr[3].body_len == 22);
	const uint8_t *p = fr[3].body + 7;
	CHECK(get32(fr[3].body + 2) == tStart);
	CHECK(get32(p) == tStart - 5 && get32(p + 4) == 144800000u);
	CHECK(p[8] == 0 && p[9] == 0 && get16(p + 10) == 0x0856);
	CHECK(get16(p + 12) == 12 && p[14] == 3);        // busy at the press, late key
	CHECK(fr[4].id == 0x50C3 && fr[4].body_len == 26);
	p = fr[4].body + 7;
	CHECK(get32(p) == tStart && p[12] == TXEND_RELEASE);
	CHECK(get16(p + 17) >= 9);                       // mic sampled every 10 ms
	CHECK(gCounters[CNT_TRANSMISSIONS] == 1 && gCounters[CNT_LATE_KEYS] == 1);
	CHECK(gCounters[CNT_EVENTS_DEFERRED] == 5);
	memcpy(out, out + fr[3].offset, fr[3].size + fr[4].size); out_len = fr[3].size + fr[4].size;
	last_req_len = 0;
	vec("event_tx_start_and_end", "events, no request: a late key (12 ms lock delay, busy at the press), 100 ms of TX, released; both sent after the release with DEFERRED");

	// TX_REFUSED
	host_clear_out();
	MON_TxRefused(g_ms - 4, 1, 1234);
	host_advance(1);
	CHECK(events(EV_TX_REFUSED, &e) == 1 && e->body_len == 14);
	CHECK(e->body[7 + 4] == 1 && get16(e->body + 12) == 1234);
	CHECK(gCounters[CNT_TX_REFUSED] == 1);
	last_req_len = 0;
	vec("event_tx_refused", "event, no request: a press refused with 1234 ms of lock left (reason 1 LOCK)");

	// LIVE_TX: events go out while transmitting
	subscribe(EV_STORED_MASK, SUB_LIVE_TX, 0, 0, 0, 0);
	host_advance(25);
	host_clear_out();
	gCurrentFunction = FUNCTION_TRANSMIT;
	host_ptt.pressed = true;
	MON_TxStarted();
	host_advance(1);
	CHECK(events(EV_TX_START, &e) == 1 && !(e->body[6] & EVF_DEFERRED));
	gCurrentFunction = FUNCTION_FOREGROUND;
	host_ptt.pressed = false;
	host_advance(3);

	// the ring overflows while held: EVENTS_LOST once delivery resumes
	subscribe(EV_STORED_MASK, 0, 0, 0, 0, 0);
	host_advance(25);
	host_clear_out();
	host_ptt.pressed = true;
	const uint16_t first = EVT_NextSeq();
	for (int i = 0; i < 25; i++) MON_TxRefused(g_ms, 2, 0);
	host_advance(3);
	CHECK(host_frames(fr, 64) == 0);
	host_ptt.pressed = false;
	host_advance(3);
	nfr = host_frames(fr, 64);
	CHECK(nfr == 21);                                // 20 kept, then EVENTS_LOST
	CHECK(get16(fr[0].body) == first + 5);
	CHECK(fr[20].id == 0x50C9 && get16(fr[20].body + 7) == first && get16(fr[20].body + 9) == 5);
	CHECK(gCounters[CNT_EVENTS_LOST] == 5);
}

static void test_replay(void)
{
	boot_plain();
	subscribe(1u << EV_TX_REFUSED, 0, 0, 0, 0, 0);
	host_advance(25);
	const uint16_t first = EVT_NextSeq();
	for (int i = 0; i < 6; i++) MON_TxRefused(1000 + i, 2, 0);
	host_advance(1);
	host_clear_out();

	uint8_t r[2];
	put16(r, first + 2);
	const Frame_t *f = v2(V2_EVENT_REPLAY, 0x21, r, 2);
	CHECK(f && status(f) == V2_OK && f->body_len == 11);
	CHECK(v2_nframes == 5 && f == &v2_frames[4]);   // four events, then the reply
	for (int i = 0; i < 4; i++) {
		CHECK(v2_frames[i].id == 0x50C4 && (v2_frames[i].body[6] & EVF_REPLAY));
		CHECK(get16(v2_frames[i].body) == first + 2 + i);
		CHECK(get32(v2_frames[i].body + 7) == 1002u + i);
	}
	CHECK(get16(rb(f)) == first + 2 && rb(f)[2] == 4);
	CHECK(get16(rb(f) + 3) == first && get16(rb(f) + 5) == first + 6);
	vec("event_replay", "six TX_REFUSED events stored and sent (seq 0 to 5); EVENT_REPLAY from seq 2 re-sends four with REPLAY, then the reply");

	// from before the ring: everything still there; replayed bytes are the originals
	put16(r, (uint16_t)(first - 100));
	f = v2(V2_EVENT_REPLAY, 0x22, r, 2);
	CHECK(f && rb(f)[2] == 6 && get16(rb(f)) == first);

	// more than the budget: the host asks again from first + count
	for (int i = 0; i < 20; i++) MON_TxRefused(2000 + i, 2, 0);
	host_advance(1);
	put16(r, EVT_OldestSeq());
	f = v2(V2_EVENT_REPLAY, 0x23, r, 2);
	CHECK(f && rb(f)[2] > 0 && rb(f)[2] < 20);
	CHECK(rb(f)[2] * (FRAME_OVERHEAD + EVT_HEADER + 7) <= EVT_REPLAY_BUDGET);

	// events not yet sent are not replayed (they follow normally)
	host_ptt.pressed = true;
	MON_TxRefused(3000, 2, 0);
	host_advance(1);
	put16(r, (uint16_t)(EVT_NextSeq() - 1));
	f = v2(V2_EVENT_REPLAY, 0x24, r, 2);
	CHECK(f && rb(f)[2] == 0 && get16(rb(f)) == EVT_NextSeq());
	host_ptt.pressed = false;
}

static void test_ephemeral(void)
{
	boot_plain();
	regs[0x67] = 0x77;
	const Frame_t *f = subscribe((1u << EV_HEARTBEAT) | (1u << EV_RSSI_STREAM), 0, 100, 10, 3, 0);
	CHECK(status(f) == V2_OK);
	host_clear_out();
	host_us = 250;
	host_advance(100);
	const Frame_t *e;
	CHECK(events(EV_HEARTBEAT, &e) == 1 && e->body_len == 19);
	CHECK(e->body[6] == (EVF_EPHEMERAL | EVF_TIME_EXACT));
	CHECK(get16(e->body) == EVT_NextSeq());
	CHECK(get16(e->body + 7) == 250);
	CHECK(e->body[7 + 3] == 0 && get16(e->body + 7 + 4) == 0x77 && get16(e->body + 7 + 6) == 7800);
	CHECK(events(EV_RSSI_STREAM, &e) >= 3);
	CHECK(e->body_len == 7 + 2 + 12 && e->body[7] == 10 && e->body[8] == 3);
	CHECK(get16(e->body + 9) == 0x77 && e->body[11] == 0x49 && e->body[12] == 0x5B);

	// ephemeral events are dropped, not held, while PTT is asserted
	host_clear_out();
	host_ptt.pressed = true;
	const uint32_t dropped = gCounters[CNT_EPHEMERAL_DROPPED];
	host_advance(200);
	CHECK(host_frames(fr, 64) == 0 && gCounters[CNT_EPHEMERAL_DROPPED] > dropped);
	host_ptt.pressed = false;
	host_advance(3);
	CHECK(EVT_Unsent() == 0);
	host_us = 0;
}

static void test_params(void)
{
	boot_plain();

	// GET_PARAMS, all live
	uint8_t r[64] = { 0 };
	const Frame_t *f = v2(V2_GET_PARAMS, 0x40, r, 1);
	CHECK(f && status(f) == V2_OK && f->body_len == 4 + 1 + 24 + 37);
	const uint8_t *o = rb(f);
	CHECK(o[0] == 0 && o[1] == P_FREQ_HZ && get32(o + 2) == 144800000u);
	CHECK(o[6] == P_POWER && o[7] == 0 && o[8] == P_BANDWIDTH && o[9] == 0);
	CHECK(o[10] == P_DEV_WIDE && get16(o + 11) == 0x0856);
	vec("get_params_all", "fresh boot, blank EEPROM (RX gain from the factory byte 0x1F8E = 45); GET_PARAMS flags 0, no ids: every parameter, live");

	// STORED omits the RAM-only ones; a list in request order
	r[0] = 1; r[1] = P_SQL_RAW; r[2] = P_SQUELCH; r[3] = P_RX_GAIN;
	f = v2(V2_GET_PARAMS, 0x41, r, 4);
	CHECK(f && status(f) == V2_OK && f->body_len == 4 + 1 + 4);
	CHECK(rb(f)[1] == P_SQUELCH && rb(f)[2] == 1 && rb(f)[3] == P_RX_GAIN && rb(f)[4] == 45);
	r[0] = 0; r[1] = 0x19;
	f = v2(V2_GET_PARAMS, 0x42, r, 2);
	CHECK(status(f) == V2_BAD_PARAM && rb(f)[0] == 0x19);
	r[1] = 7; r[2] = 7;
	CHECK(status(v2(V2_GET_PARAMS, 0x43, r, 3)) == V2_BAD_PARAM);

	// SET_PARAMS: applied together, read back, receiver set up once
	uint8_t s[64];
	unsigned n = 0;
	s[n++] = 0;
	s[n++] = P_FREQ_HZ; put32(s + n, 433500000u); n += 4;
	s[n++] = P_SQUELCH; s[n++] = 3;
	s[n++] = P_DEV_WIDE; put16(s + n, 0x0800); n += 2;
	const int setups = reg_writes[0x3F];
	f = v2(V2_SET_PARAMS, 0x44, s, n);
	CHECK(f && status(f) == V2_OK && f->body_len == 4 + 1 + 5 + 2 + 3);
	CHECK(rb(f)[0] == (SETR_TX_ALLOWED | SETR_RETUNED));
	CHECK(rb(f)[1] == P_FREQ_HZ && get32(rb(f) + 2) == 433500000u);
	CHECK(gVfo->Frequency == 43350000 && gEeprom.SQUELCH_LEVEL == 3 && gEeprom.DEVIATION_WIDE == 0x0800);
	CHECK(gEeprom.ScreenChannel == FREQ_CHANNEL_FIRST + BAND6_400MHz);
	CHECK(regs[0x38] == (43350000 & 0xFFFF) && regs[0x39] == (43350000 >> 16));
	CHECK(reg_writes[0x3F] - setups == 2);           // one RADIO_SetupRegisters (it writes REG_3F twice)
	CHECK(eeprom[0x0C80 + 5 * 32] == 0xFF);           // RAM only: nothing written
	vec("set_params", "fresh boot; SET_PARAMS flags 0: FREQ_HZ 433.5 MHz, SQUELCH 3, DEV_WIDE 0x0800 (RAM)");

	// a PARAMS_CHANGED would go to a subscriber
	subscribe(1u << EV_PARAMS_CHANGED, 0, 0, 0, 0, 0);
	s[0] = 0; s[1] = P_MIC_GAIN; s[2] = 20;
	f = v2(V2_SET_PARAMS, 0x45, s, 3);
	host_advance(1);
	const Frame_t *e;
	CHECK(events(EV_PARAMS_CHANGED, &e) == 1 && e->body[7] == PSRC_SERIAL && get32(e->body + 8) == (1u << P_MIC_GAIN));

	// atomic: one bad record and nothing changes
	n = 0;
	s[n++] = 0;
	s[n++] = P_SQUELCH; s[n++] = 5;
	s[n++] = P_MIC_GAIN; s[n++] = 32;
	f = v2(V2_SET_PARAMS, 0x46, s, n);
	CHECK(status(f) == V2_RANGE && rb(f)[0] == P_MIC_GAIN);
	CHECK(gEeprom.SQUELCH_LEVEL == 3 && gEeprom.MIC_GAIN == 20);
	vec("set_params_range", "after set_params: SQUELCH 5 with MIC_GAIN 32 (out of range): RANGE, detail 0x06, nothing applied");

	// duplicate, unknown, truncated
	n = 0; s[n++] = 0; s[n++] = P_SQUELCH; s[n++] = 2; s[n++] = P_SQUELCH; s[n++] = 2;
	CHECK(status(v2(V2_SET_PARAMS, 0x47, s, n)) == V2_BAD_PARAM);
	n = 0; s[n++] = 0; s[n++] = 0x30; s[n++] = 2;
	CHECK(status(v2(V2_SET_PARAMS, 0x48, s, n)) == V2_BAD_PARAM);
	n = 0; s[n++] = 0; s[n++] = P_DEV_WIDE; s[n++] = 2;
	CHECK(status(v2(V2_SET_PARAMS, 0x49, s, n)) == V2_BAD_LENGTH);

	// ranges that need more than a limit
	n = 0; s[n++] = 0; s[n++] = P_FREQ_HZ; put32(s + n, 433500005u); n += 4;
	CHECK(status(v2(V2_SET_PARAMS, 0x4A, s, n)) == V2_RANGE);
	n = 0; s[n++] = 0; s[n++] = P_FREQ_HZ; put32(s + n, 80000000u); n += 4;      // in no band
	CHECK(status(v2(V2_SET_PARAMS, 0x4B, s, n)) == V2_RANGE);
	n = 0; s[n++] = 0; s[n++] = P_TX_TIMEOUT_S; s[n++] = 25;
	CHECK(status(v2(V2_SET_PARAMS, 0x4C, s, n)) == V2_RANGE);
	n = 0; s[n++] = 0; s[n++] = P_SERIAL_LOCK_MS; put16(s + n, 25); n += 2;
	CHECK(status(v2(V2_SET_PARAMS, 0x4D, s, n)) == V2_RANGE);
	n = 0; s[n++] = 0; s[n++] = P_BUSY_RSSI_CLOSE; put16(s + n, 120); n += 2;   // above open (110)
	f = v2(V2_SET_PARAMS, 0x4E, s, n);
	CHECK(status(f) == V2_RANGE && rb(f)[0] == P_BUSY_RSSI_CLOSE);
	s[n++] = P_BUSY_RSSI_OPEN; put16(s + n, 130); n += 2;                       // both together: fine
	CHECK(status(v2(V2_SET_PARAMS, 0x4F, s, n)) == V2_OK);
	CHECK(gV2.BUSY_RSSI_OPEN == 130 && gV2.BUSY_RSSI_CLOSE == 120);

	// TX band: REQUIRE_TX_OK refuses, otherwise reported in the result
	n = 0; s[n++] = SETP_REQUIRE_TX; s[n++] = P_FREQ_HZ; put32(s + n, 120000000u); n += 4;
	f = v2(V2_SET_PARAMS, 0x50, s, n);
	CHECK(status(f) == V2_TX_BAND && rb(f)[0] == P_FREQ_HZ && gVfo->Frequency == 43350000);
	s[0] = 0;
	f = v2(V2_SET_PARAMS, 0x51, s, n);
	CHECK(status(f) == V2_OK && !(rb(f)[0] & SETR_TX_ALLOWED) && gVfo->Frequency == 12000000);
	CHECK(gEeprom.ScreenChannel == FREQ_CHANNEL_FIRST + BAND2_108MHz);

	// DRY_RUN validates and reads back without applying
	n = 0; s[n++] = SETP_DRY_RUN; s[n++] = P_POWER; s[n++] = 2;
	f = v2(V2_SET_PARAMS, 0x52, s, n);
	CHECK(status(f) == V2_OK && rb(f)[1] == P_POWER && rb(f)[2] == 2 && gVfo->OUTPUT_POWER == 0);

	// RAM-only parameters
	n = 0; s[n++] = 0; s[n++] = P_SQL_RAW;
	const uint8_t sql[6] = { 30, 20, 40, 50, 60, 70 };
	memcpy(s + n, sql, 6); n += 6;
	s[n++] = P_AGC_FIX; s[n++] = 5;
	f = v2(V2_SET_PARAMS, 0x53, s, n);
	CHECK(status(f) == V2_OK && memcmp(rb(f) + 2, sql, 6) == 0);
	CHECK(gVfo->SquelchOpenRSSIThresh == 30 && gVfo->SquelchCloseGlitchThresh == 70);
	CHECK((regs[0x7E] & 0xF000) == (0x8000 | (5u << 12)));
	s[0] = SETP_PERSIST;
	f = v2(V2_SET_PARAMS, 0x54, s, n);
	CHECK(status(f) == V2_NOT_PERSISTABLE && rb(f)[0] == P_SQL_RAW);
	// SQL_RAW survives a retune, and setting SQUELCH drops it
	n = 0; s[n++] = 0; s[n++] = P_FREQ_HZ; put32(s + n, 145000000u); n += 4;
	v2(V2_SET_PARAMS, 0x55, s, n);
	CHECK(gVfo->SquelchOpenRSSIThresh == 30);
	n = 0; s[n++] = 0; s[n++] = P_SQUELCH; s[n++] = 3;
	v2(V2_SET_PARAMS, 0x56, s, n);
	CHECK(!gSqlRawActive);

	// PERSIST needs the settings block
	n = 0; s[n++] = SETP_PERSIST; s[n++] = P_MIC_GAIN; s[n++] = 10;
	f = v2(V2_SET_PARAMS, 0x57, s, n);
	CHECK(status(f) == V2_EEPROM && rb(f)[0] == P_MIC_GAIN);
	CHECK(eeprom_writes_in_cal == 0);
}

static void test_persist(void)
{
	boot_plain();
	valid_settings_block();
	const Frame_t *f = v2(V2_GET_INFO, 1, NULL, 0);
	CHECK(rb(f)[37] == 1 && rb(f)[38] == 0);

	uint8_t s[32];
	unsigned n = 0;
	s[n++] = SETP_PERSIST;
	s[n++] = P_MIC_GAIN; s[n++] = 12;
	s[n++] = P_SERIAL_LOCK_MS; put16(s + n, 40); n += 2;
	s[n++] = P_PTT_PRESS_MS; s[n++] = 3;
	f = v2(V2_SET_PARAMS, 0x60, s, n);
	CHECK(status(f) == V2_OK && (rb(f)[0] & SETR_PERSIST));
	CHECK(eeprom[0x1D03] == 0xFF);                   // nothing yet: after the reply
	CHECK(lockms(f) == 20);                          // this frame's lock is the old setting

	// never during a transmission, nor while PTT is pressed
	gCurrentFunction = FUNCTION_TRANSMIT;
	host_advance(20);
	CHECK(eeprom[0x1D03] == 0xFF && PARAMS_PersistPending());
	gCurrentFunction = FUNCTION_FOREGROUND;
	host_ptt.pressed = true;
	host_advance(20);
	CHECK(eeprom[0x1D03] == 0xFF);
	host_ptt.pressed = false;

	// one block per pass
	const uint32_t w0 = gEepromBlocksWritten;
	host_advance(1);
	CHECK(gEepromBlocksWritten == w0 + 1 && eeprom[0x1D03] == 12);
	host_advance(10);
	CHECK(!PARAMS_PersistPending());
	CHECK(eeprom[0x1D50] == 3);
	CHECK(eeprom[0x1D60] == 1 && eeprom[0x1D61] == 4);
	CHECK(eeprom[0x1D62] == 0xFF && eeprom[0x1D68] == 0xFF && eeprom[0x1D6F] == 0xFF);   // fresh v2 block blanked
	CHECK(eeprom[0x1D00] == 1 && eeprom[0x1D01] == 0xFF);    // others untouched
	CHECK(eeprom_writes_in_cal == 0);

	// stored reads now match, and survive a reboot
	uint8_t r[4] = { 1, P_MIC_GAIN, P_SERIAL_LOCK_MS };
	f = v2(V2_GET_PARAMS, 0x61, r, 3);
	CHECK(rb(f)[2] == 12 && get16(rb(f) + 4) == 40);
	host_boot_keep_eeprom();
	CHECK(gEeprom.MIC_GAIN == 12 && gV2.SERIAL_LOCK_MS == 40 && gEeprom.PTT_PRESS_MS == 3 && gV2.valid);
	host_send_mode(0x0514, &session, 4, false);
	CHECK(gSerialLockMs == 40);

	// a RAM change, then SAVE: the differences are written
	n = 0; s[n++] = 0; s[n++] = P_RX_DAC_GAIN; s[n++] = 9; s[n++] = P_BACKLIGHT; s[n++] = 7;
	v2(V2_SET_PARAMS, 0x62, s, n);
	f = v2(V2_GET_STATUS, 0x63, NULL, 0);
	CHECK(rb(f)[10] & 0x01);                         // live differs from stored
	uint8_t op = 0;
	f = v2(V2_SAVE_PARAMS, 0x64, &op, 1);
	CHECK(status(f) == V2_OK && get32(rb(f)) == ((1u << P_RX_DAC_GAIN) | (1u << P_BACKLIGHT)));
	host_advance(10);
	CHECK(eeprom[0x1D09] == 9 && eeprom[0x1D0A] == 7);
	f = v2(V2_GET_STATUS, 0x65, NULL, 0);
	CHECK(!(rb(f)[10] & 0x01));

	// REVERT drops RAM changes
	n = 0; s[n++] = 0; s[n++] = P_MIC_GAIN; s[n++] = 30; s[n++] = P_AFC; s[n++] = 0;
	v2(V2_SET_PARAMS, 0x66, s, n);
	op = 1;
	f = v2(V2_SAVE_PARAMS, 0x67, &op, 1);
	CHECK(status(f) == V2_OK && get32(rb(f)) == ((1u << P_MIC_GAIN) | (1u << P_AFC)));
	CHECK(gEeprom.MIC_GAIN == 12 && gAfcOn);
	vec("save_params_revert", "settings block valid with MIC_GAIN 12 stored; RAM MIC_GAIN 30 and AFC 0; SAVE_PARAMS op 1 (REVERT)");
	op = 2;
	CHECK(status(v2(V2_SAVE_PARAMS, 0x68, &op, 1)) == V2_RANGE);

	// frequency persist: band slot record and channel indices, as the keypad
	n = 0; s[n++] = SETP_PERSIST; s[n++] = P_FREQ_HZ; put32(s + n, 145012500u); n += 4; s[n++] = P_POWER; s[n++] = 2;
	f = v2(V2_SET_PARAMS, 0x69, s, n);
	CHECK(status(f) == V2_OK);
	host_advance(10);
	uint32_t fq;
	memcpy(&fq, &eeprom[0x0C80 + 2 * 32], 4);
	CHECK(fq == 14501250);
	CHECK(((eeprom[0x0C80 + 2 * 32 + 12] >> 2) & 3) == 2);
	CHECK(eeprom[0x0E80] == FREQ_CHANNEL_FIRST + BAND3_137MHz);
	host_boot_keep_eeprom();
	CHECK(gVfo->Frequency == 14501250 && gVfo->OUTPUT_POWER == 2);

	// SUBSCRIBE PERSIST: the power-on default, BOOT from power-on
	host_send_mode(0x0514, &session, 4, false);
	f = subscribe((1u << EV_BOOT) | (1u << EV_CD), SUB_PERSIST, 1000, 0, 0, 0);
	CHECK(status(f) == V2_OK);
	host_advance(10);
	CHECK(get32(&eeprom[0x1D68]) == ((1u << EV_BOOT) | (1u << EV_CD)) && get16(&eeprom[0x1D6C]) == 1000);
	host_boot_keep_eeprom();
	CHECK(gSub.mask == ((1u << EV_BOOT) | (1u << EV_CD)) && gSub.heartbeatMs == 1000);
	host_advance(1);
	const Frame_t *e;
	host_set_obfuscated(true);                       // unprompted, in the power-on mode
	CHECK(events(EV_BOOT, &e) == 1 && get16(e->body + 7) == 0x0200 && get16(e->body) == 0);
	reqObf = true;
	last_req_len = 0;
	vec("event_boot", "event, no request, obfuscated (power-on mode): BOOT at power-on with CD|BOOT persisted as the default mask");
	reqObf = false;
	CHECK(eeprom_writes_in_cal == 0);
}

static void test_time_sync(void)
{
	boot_plain();
	uint8_t ref[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
	host_us = 321;
	const Frame_t *f = v2(V2_TIME_SYNC, 0x70, ref, 8);
	CHECK(f && status(f) == V2_OK && f->body_len == 25);
	CHECK(memcmp(rb(f), ref, 8) == 0);
	CHECK(get32(rb(f) + 8) == g_ms && get16(rb(f) + 12) == 321);
	CHECK(get32(rb(f) + 14) == g_ms && get16(rb(f) + 18) == 321 && rb(f)[20] == 1);
	vec("time_sync", "clock 1000 ms and 321 us when the request completed and the reply started; output queue empty (exact)");
	host_us = 0;
}

static void test_registers(void)
{
	boot_plain();
	for (int i = 0; i < 128; i++) regs[i] = 0x1000 + i;
	uint8_t r[8] = { 0x5C, 4, 0 };
	const Frame_t *f = v2(V2_REG_READ, 0x80, r, 3);
	CHECK(f && status(f) == V2_OK && f->body_len == 4 + 2 + 8);
	CHECK(rb(f)[0] == 0x5C && rb(f)[1] == 4);
	CHECK(get16(rb(f) + 2) == 0x105C && get16(rb(f) + 6) == 0x105E && get16(rb(f) + 8) == 0);
	vec("reg_read", "registers hold 0x1000 + address; REG_READ 0x5C, 4 registers, flags 0 (0x5F not read, returns 0)");
	r[2] = 1;
	f = v2(V2_REG_READ, 0x81, r, 3);
	CHECK(get16(rb(f) + 8) == 0x105F);
	r[0] = 0x70; r[1] = 0x11;
	CHECK(status(v2(V2_REG_READ, 0x82, r, 3)) == V2_RANGE);
	r[0] = 0; r[1] = 65;
	CHECK(status(v2(V2_REG_READ, 0x83, r, 3)) == V2_RANGE);
	r[0] = 0; r[1] = 64;
	f = v2(V2_REG_READ, 0x84, r, 3);
	CHECK(status(f) == V2_OK && f->body_len == 4 + 2 + 128);

	uint8_t w[64] = { 2, 0x47, 0x40, 0x62, 0x70, 0x00, 0x80 };
	f = v2(V2_REG_WRITE, 0x85, w, 7);
	CHECK(f && status(f) == V2_OK && regs[0x47] == 0x6240 && regs[0x70] == 0x8000);
	CHECK(rb(f)[0] == 2 && rb(f)[1] == 0x47 && get16(rb(f) + 2) == 0x6240);
	vec("reg_write", "REG_WRITE two registers: REG_47 = 0x6240, REG_70 = 0x8000");
	// a refused register rejects the whole command
	const uint8_t refused[] = { 0x00, 0x30, 0x33, 0x36, 0x37, 0x38, 0x39, 0x3B, 0x3C, 0x80 };
	for (unsigned i = 0; i < sizeof(refused); i++) {
		uint8_t w2[7] = { 2, 0x47, 0x11, 0x11, refused[i], 0x00, 0x00 };
		regs[0x47] = 0x6240;
		const uint16_t before = regs[refused[i] & 127];
		f = v2(V2_REG_WRITE, 0x86, w2, 7);
		CHECK(status(f) == V2_REFUSED && rb(f)[0] == refused[i]);
		CHECK(regs[0x47] == 0x6240 && regs[refused[i] & 127] == before);
	}
	w[0] = 17;
	CHECK(status(v2(V2_REG_WRITE, 0x87, w, 1 + 3 * 17)) == V2_RANGE);
	w[0] = 2;
	CHECK(status(v2(V2_REG_WRITE, 0x88, w, 6)) == V2_BAD_LENGTH);
}

static void test_overrides(void)
{
	boot_plain();
	regs[0x2B] = 0x0707;

	// ADD an RX entry with a 2 s expiry: applied at once
	uint8_t r[64] = { 1, 2, 0, 0, 1, REG_OVERRIDE_RX, 0x2B, 0xF8, 0xFF, 0x00, 0x00 };
	const Frame_t *f = v2(V2_REG_OVERRIDE, 0x90, r, 11);
	CHECK(f && status(f) == V2_OK);
	CHECK(regs[0x2B] == 0x0700);
	CHECK(rb(f)[0] == 1 && get16(rb(f) + 1) == 2 && rb(f)[3] == 0xFF);
	CHECK(rb(f)[4] == REG_OVERRIDE_RX && rb(f)[5] == 0x2B && get16(rb(f) + 6) == 0xFFF8 && get16(rb(f) + 8) == 0);
	CHECK(rb(f)[10] == 0);                           // no EEPROM entries
	vec("reg_override_add", "REG_OVERRIDE ADD one RX entry (REG_2B & 0xFFF8 | 0), expiry 2 s, no key-up bound");

	// the setup re-applies it (receive set-up)
	regs[0x2B] = 0x0707;
	RADIO_SetupRegisters(true);
	CHECK(regs[0x2B] == 0x0700);

	// refused registers and bad phases
	uint8_t bad[11] = { 1, 0, 0, 0, 1, REG_OVERRIDE_TX, 0x30, 0, 0, 0, 0 };
	f = v2(V2_REG_OVERRIDE, 0x91, bad, 11);
	CHECK(status(f) == V2_REFUSED && rb(f)[0] == 0x30 && gRegOverrideRamCount == 1);
	bad[5] = 0; bad[6] = 0x2B;
	CHECK(status(v2(V2_REG_OVERRIDE, 0x92, bad, 11)) == V2_RANGE);

	// expiry: cleared, receiver set up again, OVERRIDE_EXPIRED
	subscribe(1u << EV_OVERRIDE_EXPIRED, 0, 0, 0, 0, 0);
	host_clear_out();
	host_advance(2100);
	const Frame_t *e;
	CHECK(events(EV_OVERRIDE_EXPIRED, &e) == 1 && e->body[7] == 0 && e->body[8] == 1);
	CHECK(gRegOverrideRamCount == 0);

	// key-up bound: after one key-up, cleared when back in receive
	uint8_t k[11] = { 1, 0, 0, 1, 1, REG_OVERRIDE_TX, 0x7D, 0x00, 0x00, 0x34, 0x12 };
	f = v2(V2_REG_OVERRIDE, 0x93, k, 11);
	CHECK(status(f) == V2_OK && get16(rb(f) + 1) == 0xFFFF && rb(f)[3] == 1);
	gCurrentFunction = FUNCTION_TRANSMIT;
	MON_TxStarted();
	host_advance(5);
	CHECK(gRegOverrideRamCount == 1);                // never during a transmission
	gCurrentFunction = FUNCTION_FOREGROUND;
	host_clear_out();
	host_advance(3);
	CHECK(gRegOverrideRamCount == 0);
	CHECK(events(EV_OVERRIDE_EXPIRED, &e) == 1 && e->body[7] == 1);

	// COMMIT needs the settings block; then 0x1D10 holds the table
	f = v2(V2_REG_OVERRIDE, 0x94, r, 11);
	uint8_t c[5] = { 3, 0, 0, 0, 0 };
	CHECK(status(v2(V2_REG_OVERRIDE, 0x95, c, 5)) == V2_EEPROM);
	valid_settings_block();
	f = v2(V2_REG_OVERRIDE, 0x96, r, 11);
	f = v2(V2_REG_OVERRIDE, 0x97, c, 5);
	CHECK(status(f) == V2_OK && rb(f)[0] == 0 && rb(f)[4] == 1);
	host_advance(20);
	CHECK(eeprom[0x1D10] == REG_OVERRIDE_RX && eeprom[0x1D11] == 0x2B && eeprom[0x1D12] == 0xF8 && eeprom[0x1D18] == 0xFF);
	host_boot_keep_eeprom();
	CHECK(gRegOverrideCount == 1 && gRegOverrides[0].reg == 0x2B);
	host_send_mode(0x0514, &session, 4, false);
	c[0] = 4;
	f = v2(V2_REG_OVERRIDE, 0x98, c, 5);
	CHECK(status(f) == V2_OK && gRegOverrideCount == 0);
	host_advance(20);
	CHECK(eeprom[0x1D10] == 0xFF);
	c[0] = 2;
	CHECK(status(v2(V2_REG_OVERRIDE, 0x99, c, 5)) == V2_OK);
	c[0] = 5;
	CHECK(status(v2(V2_REG_OVERRIDE, 0x9A, c, 5)) == V2_RANGE);
	CHECK(eeprom_writes_in_cal == 0);
}

static void test_tone(void)
{
	boot_plain();
	subscribe(1u << EV_TONE_END, 0, 0, 0, 0, 0);
	uint8_t r[7];
	put16(r, 1000); r[2] = 1; put16(r + 3, 64); put16(r + 5, 500);
	const Frame_t *f = v2(V2_LEVEL_TONE, 0xA0, r, 7);
	CHECK(f && status(f) == V2_OK && f->body_len == 7);
	CHECK(rb(f)[0] == 64 && get16(rb(f) + 1) == 0x2854);   // round(1000 x 10.32444) = 10324
	CHECK(regs[0x71] == 0x2854 && regs[0x70] == (0x8000 | (64 << 8)) && regs[0x47] == 0x6240);
	vec("level_tone_raw", "LEVEL_TONE 1000 Hz, mode 1 (raw), gain code 64, 500 ms");

	// a squelch-close receiver set-up does not end it
	RADIO_SetupRegisters(true);
	CHECK(regs[0x70] == (0x8000 | (64 << 8)) && regs[0x47] == 0x6240);

	// the duration ends it, TONE_END reason 0, audio muted again
	host_clear_out();
	host_advance(501);
	const Frame_t *e;
	CHECK(events(EV_TONE_END, &e) == 1 && e->body[7] == TONE_END_ELAPSED);
	CHECK(regs[0x70] == 0 && regs[0x47] == 0x6040);

	// the word for other frequencies: round(f x 10.32444)
	const uint16_t fq[] = { 100, 1200, 2200, 4999, 5000 };
	for (unsigned i = 0; i < 5; i++) {
		put16(r, fq[i]);
		f = v2(V2_LEVEL_TONE, 0xA1, r, 7);
		const double want = fq[i] * 10.32444 + 0.5;
		CHECK(get16(rb(f) + 1) == (uint16_t)want);
	}

	// stop, replaced, retune
	host_advance(1);
	host_clear_out();
	put16(r + 5, 0);
	f = v2(V2_LEVEL_TONE, 0xA2, r, 7);
	host_advance(1);
	CHECK(status(f) == V2_OK && events(EV_TONE_END, &e) == 1 && e->body[7] == TONE_END_STOPPED);
	put16(r + 5, 1000);
	v2(V2_LEVEL_TONE, 0xA3, r, 7);
	host_clear_out();
	v2(V2_LEVEL_TONE, 0xA4, r, 7);
	host_advance(1);
	CHECK(events(EV_TONE_END, &e) == 1 && e->body[7] == TONE_END_REPLACED);
	uint8_t s[6] = { 0, P_FREQ_HZ };
	put32(s + 2, 145500000u);
	host_clear_out();
	v2(V2_SET_PARAMS, 0xA5, s, 6);
	host_advance(1);
	CHECK(events(EV_TONE_END, &e) == 1 && e->body[7] == TONE_END_RETUNE && !TONE_Active());

	// ranges; mode 0 needs the calibration byte; reduced service
	put16(r, 99);
	CHECK(status(v2(V2_LEVEL_TONE, 0xA6, r, 7)) == V2_RANGE);
	put16(r, 1000); r[2] = 2;
	CHECK(status(v2(V2_LEVEL_TONE, 0xA7, r, 7)) == V2_RANGE);
	r[2] = 1; put16(r + 3, 128);
	CHECK(status(v2(V2_LEVEL_TONE, 0xA8, r, 7)) == V2_RANGE);
	r[2] = 0; put16(r + 3, 3000);
	f = v2(V2_LEVEL_TONE, 0xA9, r, 7);
	CHECK(status(f) == V2_UNSUPPORTED);
	vec("level_tone_uncalibrated", "LEVEL_TONE mode 0 (deviation) 3000 Hz without the calibration byte 0x1D6F: UNSUPPORTED");
	gV2.TONE_CAL = 60;
	f = v2(V2_LEVEL_TONE, 0xAA, r, 7);
	CHECK(status(f) == V2_OK && rb(f)[0] == 60);
	gV2.TONE_CAL = 0;
	gReducedService = true;
	CHECK(status(v2(V2_LEVEL_TONE, 0xAB, r, 7)) == V2_STATE);
	gReducedService = false;
}

static void test_counters(void)
{
	boot_plain();
	v2(V2_GET_INFO, 1, NULL, 0);
	v2(0x5011, 2, NULL, 0);
	uint8_t r = 0;
	const Frame_t *f = v2(V2_GET_COUNTERS, 0xB0, &r, 1);
	CHECK(f && status(f) == V2_OK && f->body_len == 4 + 1 + 4 * 14);
	CHECK(rb(f)[0] == 14);
	CHECK(get32(rb(f) + 1) == 4);                    // hello, GET_INFO, 0x5011, this one
	CHECK(get32(rb(f) + 1 + 4 * 3) == 1);            // one non-OK reply
	vec("get_counters", "after a plain hello, GET_INFO and an unknown 0x5011: frames 4, non-OK replies 1");
	r = 1;
	v2(V2_GET_COUNTERS, 0xB1, &r, 1);
	r = 0;
	f = v2(V2_GET_COUNTERS, 0xB2, &r, 1);
	CHECK(get32(rb(f) + 1) == 1);
}

// The calibration area is never written, whatever arrives.
static void test_calibration_guard_fuzz(void)
{
	boot_plain();
	valid_settings_block();
	srand(7);
	uint8_t b[130];
	for (int i = 0; i < 3000; i++) {
		const uint16_t id = 0x5000 + (rand() % 16);
		const uint16_t n = rand() % 40;
		for (unsigned k = 0; k < n; k++) b[k] = rand();
		host_clear_out();
		host_send(id, b, n);
		host_advance(rand() % 3);
	}
	host_advance(100);
	CHECK(eeprom_writes_in_cal == 0);
	for (unsigned a = 0x1E00; a < 0x1EC0; a++) if (eeprom[a] != 0x40) { CHECK(eeprom[a] == 0x40); break; }
}

int main(int argc, char **argv)
{
	test_framing();
	test_status();
	test_subscribe_and_busy();
	test_deferral_and_tx_events();
	test_replay();
	test_ephemeral();
	test_params();
	test_persist();
	test_time_sync();
	test_registers();
	test_overrides();
	test_tone();
	test_counters();
	test_calibration_guard_fuzz();

	if (argc > 1)
		write_vectors(argv[1]);

	if (failures) {
		printf("%d check(s) failed\n", failures);
		return 1;
	}
	printf("all protocol v2 host tests passed (%d vectors)\n", nvec);
	return 0;
}
