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

#include "app/app.h"
#include "app/events.h"
#include "app/monitor.h"
#include "app/params.h"
#include "app/uart.h"
#include "app/v2.h"
#include "app/wire.h"
#include "driver/bk4819.h"
#include "driver/eeprom.h"
#include "frequencies.h"
#include "functions.h"
#include "helper/battery.h"
#include "misc.h"
#include "ptt.h"
#include "pttarb.h"
#include "radio.h"
#include "settings.h"
#include "version.h"

void V2_Init(void)
{
	EVT_Init();
	PARAMS_Init();
	MON_Init();

	uint8_t p[5];
	put16(p, V2_PROTOCOL_VERSION);
	p[2] = 0;                                     // reset cause: not known
	EVT_Store(EV_BOOT, g_ms, p, 3);
	p[0] = PSRC_BOOT;
	put32(p + 1, PARAMS_SUPPORTED);
	EVT_Store(EV_PARAMS_CHANGED, g_ms, p, 5);
}

uint8_t V2_State(void)
{
	if (gReducedService)
		return 4;
	switch (gCurrentFunction) {
		case FUNCTION_TRANSMIT: return 2;
		case FUNCTION_MONITOR:  return 3;
		case FUNCTION_INCOMING:
		case FUNCTION_RECEIVE:  return 1;
		default:                return 0;
	}
}

uint8_t V2_Flags1(void)
{
	PttState_t ptt;
	PTT_GetState(&ptt);
	const uint8_t arb = APP_PttArbState();
	return (g_SquelchLost                          ? 0x01u : 0)
	     | (MON_Busy()                             ? 0x02u : 0)
	     | (ptt.pressed                            ? 0x04u : 0)
	     | (gSerialLockMs                          ? 0x08u : 0)
	     | (TX_freq_check(gVfo->Frequency) == 0    ? 0x10u : 0)
	     | (arb == ARB_LATCHED                     ? 0x20u : 0)
	     | (TONE_Active()                          ? 0x40u : 0)
	     | (arb == ARB_PENDING                     ? 0x80u : 0);
}

static void Reply(uint16_t id, uint8_t tag, uint8_t status, uint16_t len)
{
	uint8_t *b = UART_FrameBody();
	b[0] = tag;
	b[1] = status;
	put16(b + 2, gSerialLockMs);
	if (status != V2_OK)
		gCounters[CNT_REPLIES_NOT_OK]++;
	UART_SendFrameBody(id + 0x80, 4 + len);
}

static uint16_t GetInfo(uint8_t *o)
{
	uint32_t caps = CAP_LIVE_TX | CAP_RSSI_BUSY | CAP_TONE_RAW | CAP_RAM_OVERRIDES | CAP_PERSISTENCE | CAP_EXACT_TIME_SYNC;
	if (gV2.TONE_CAL)
		caps |= CAP_TONE_DEVIATION;
	put16(o, V2_PROTOCOL_VERSION);
	memset(o + 2, 0, 16);
	strncpy((char *)o + 2, Version, 15);
	put32(o + 18, caps);
	put32(o + 22, PARAMS_SUPPORTED);
	put32(o + 26, EV_SUPPORTED);
	o[30] = V2_MAX_REQUEST_BODY;
	o[31] = EVT_SLOTS;
	put16(o + 32, gV2.SERIAL_LOCK_MS);
	o[34] = LATE_KEY_MAX_MS;
	o[35] = gSetting_F_LOCK;
	o[36] = (gSetting_200TX ? 1u : 0) | (gSetting_350TX ? 2u : 0) | (gSetting_500TX ? 4u : 0) | (gSetting_350EN ? 8u : 0);
	o[37] = gSettingsBlockValid ? SETTINGS_PKT_VERSION : 0;
	o[38] = gV2.valid ? SETTINGS_V2_VERSION : 0;
	o[39] = BURST_PERIOD_DEFAULT_MS;
	return 40;
}

static uint16_t GetStatus(uint8_t *o)
{
	const bool     chip = !gReducedService;
	const bool     tx   = gCurrentFunction == FUNCTION_TRANSMIT;
	const bool     narrow = gVfo->CHANNEL_BANDWIDTH == BANDWIDTH_NARROW;

	put32(o + 0, g_ms);
	put32(o + 4, gVfo->Frequency * 10u);
	o[8]  = V2_State();
	o[9]  = V2_Flags1();
	o[10] = (PARAMS_LiveDiffers()           ? 0x01u : 0)
	      | (gRegOverrideRamCount           ? 0x02u : 0)
	      | (gRegOverrideCount              ? 0x04u : 0)
	      | ((gSub.options & SUB_LIVE_TX)   ? 0x08u : 0)
	      | (PARAMS_PersistPending()        ? 0x10u : 0);   // bit 5: no memory channels
	o[11] = gVfo->OUTPUT_POWER;
	o[12] = gVfo->CHANNEL_BANDWIDTH;
	o[13] = gEeprom.SQUELCH_LEVEL;
	put16(o + 14, narrow ? gEeprom.DEVIATION_NARROW : gEeprom.DEVIATION_WIDE);
	put16(o + 16, chip ? (BK4819_ReadRegister(BK4819_REG_67) & 0x01FF) : 0);
	o[18] = chip ? (BK4819_ReadRegister(BK4819_REG_65) & 0x007F) : 0;
	o[19] = chip ? (BK4819_ReadRegister(BK4819_REG_63) & 0x00FF) : 0;
	o[20] = chip ? MON_AgcByte(BK4819_ReadRegister(BK4819_REG_7E)) : 0;
	o[21] = gBatteryDisplayLevel;
	put16(o + 22, gBatteryVoltageAverage * 10u);
	put16(o + 24, gSerialLockMs);
	put16(o + 26, tx ? gTxTimerCountdown_500ms * 5u : 0xFFFF);
	put16(o + 28, MON_BusyAge());
	put16(o + 30, EVT_NextSeq());
	o[32] = 0xFF;                                  // no channels any more
	o[33] = gTxTimeoutSeconds[gEeprom.TX_TIMEOUT];
	return 34;
}

static uint8_t Subscribe(const uint8_t *r, uint8_t *o, uint8_t *detail)
{
	const uint32_t mask    = get32(r);
	const uint8_t  options = r[4];
	const uint16_t hb      = get16(r + 5);
	const uint8_t  period  = r[7];
	const uint8_t  batch   = r[8];
	const uint8_t  burst   = r[9];

	if (hb != 0 && (hb < 100 || hb > 60000))           { *detail = 5; return V2_RANGE; }
	if (period != 0 && (period < 5 || period > 250))  { *detail = 7; return V2_RANGE; }
	if (batch > 20 || (period != 0 && batch == 0))    { *detail = 8; return V2_RANGE; }
	if (burst != 0 && (burst < 2 || burst > 50))      { *detail = 9; return V2_RANGE; }
	if ((options & SUB_PERSIST) && !PARAMS_PersistSubscription(mask & EV_SUPPORTED, options & SUB_LIVE_TX, hb))
		return V2_EEPROM;

	gSub.mask          = mask & EV_SUPPORTED;
	gSub.options       = options & SUB_LIVE_TX;
	gSub.heartbeatMs   = hb;
	gSub.rssiPeriodMs  = period;
	gSub.rssiBatch     = batch;
	gSub.burstPeriodMs = burst;
	MON_SubscriptionChanged();

	put16(o, EVT_NextSeq());
	put16(o + 2, EVT_OldestSeq());
	put32(o + 4, g_ms);
	return V2_OK;
}

static uint8_t GetParams(const uint8_t *r, uint16_t n, uint8_t *o, uint16_t *len, uint8_t *detail)
{
	const bool stored = r[0] & 1u;
	uint32_t   ids    = 0;

	for (uint16_t i = 1; i < n; i++) {
		const uint8_t id = r[i];
		if (!PARAMS_Size(id) || ((ids >> id) & 1u)) {
			*detail = id;
			return V2_BAD_PARAM;
		}
		ids |= 1u << id;
	}

	o[0] = r[0];
	uint16_t l = 1;
	for (uint16_t i = 0; i < ((n > 1) ? n - 1 : P_LAST); i++) {
		const uint8_t id = (n > 1) ? r[1 + i] : i + 1;
		if (stored && ((PARAMS_RAM_ONLY >> id) & 1u))
			continue;
		o[l++] = id;
		l += PARAMS_Get(id, stored, o + l);
	}
	*len = l;
	return V2_OK;
}

static uint8_t LevelTone(const uint8_t *r, uint8_t *o, uint8_t *detail)
{
	const uint16_t f     = get16(r);
	const uint8_t  mode  = r[2];
	const uint16_t level = get16(r + 3);
	const uint16_t dur   = get16(r + 5);
	uint8_t        gain;

	if (gReducedService)
		return V2_STATE;
	if (dur == 0) {
		TONE_Stop(TONE_END_STOPPED);
		o[0] = 0;
		put16(o + 1, 0);
		return V2_OK;
	}
	if (f < 100 || f > 5000)  { *detail = 0; return V2_RANGE; }
	if (mode > 1)             { *detail = 2; return V2_RANGE; }
	if (dur > 60000)          { *detail = 5; return V2_RANGE; }
	if (mode == 1) {
		if (level > 127)      { *detail = 3; return V2_RANGE; }
		gain = level;
	}
	else {
		if (level > 8000)     { *detail = 3; return V2_RANGE; }
		if (!gV2.TONE_CAL)
			return V2_UNSUPPORTED;
		// provisional: linear in the code (the gain law is open, Q4)
		const uint32_t g = ((uint32_t)gV2.TONE_CAL * level + 1500u) / 3000u;
		gain = (g > 127) ? 127 : g;
	}
	// REG_71 = round(f x 10.32444) for the 26 MHz crystal
	const uint16_t word = f * 10u + (uint16_t)(((uint32_t)f * 32444u + 50000u) / 100000u);
	TONE_Start(word, gain, dur);
	o[0] = gain;
	put16(o + 1, word);
	return V2_OK;
}

static uint8_t RegRead(const uint8_t *r, uint8_t *o, uint16_t *len, uint8_t *detail)
{
	const uint8_t first = r[0], count = r[1];
	if (count < 1 || count > 64)  { *detail = 1; return V2_RANGE; }
	if (first + count > 0x80)     { *detail = 0; return V2_RANGE; }
	o[0] = first;
	o[1] = count;
	for (uint8_t i = 0; i < count; i++) {
		const uint8_t reg = first + i;
		const uint16_t v = (reg == 0x5F && !(r[2] & 1u)) ? 0 : BK4819_ReadRegister(reg);
		put16(o + 2 + 2 * i, v);
	}
	*len = 2 + 2 * count;
	return V2_OK;
}

static uint8_t RegWrite(const uint8_t *r, uint16_t n, uint8_t *o, uint16_t *len, uint8_t *detail)
{
	const uint8_t count = r[0];
	if (count < 1 || count > 16)  { *detail = 0; return V2_RANGE; }
	if (n != 1u + 3u * count)
		return V2_BAD_LENGTH;
	for (uint8_t i = 0; i < count; i++)
		if (!SETTINGS_RegOverrideAllowed(r[1 + 3 * i])) {
			*detail = r[1 + 3 * i];
			return V2_REFUSED;
		}
	for (uint8_t i = 0; i < count; i++)
		BK4819_WriteRegister(r[1 + 3 * i], get16(r + 2 + 3 * i));
	o[0] = count;
	for (uint8_t i = 0; i < count; i++) {
		const uint8_t reg = r[1 + 3 * i];
		o[1 + 3 * i] = reg;
		put16(o + 2 + 3 * i, BK4819_ReadRegister(reg));
	}
	*len = 1 + 3 * count;
	return V2_OK;
}

static uint16_t ListOverrides(uint8_t *o, const RegOverride_t *t, uint8_t count)
{
	o[0] = count;
	for (uint8_t i = 0; i < count; i++) {
		uint8_t *e = o + 1 + 6 * i;
		e[0] = t[i].phase;
		e[1] = t[i].reg;
		put16(e + 2, t[i].andMask);
		put16(e + 4, t[i].orValue);
	}
	return 1 + 6 * count;
}

static void Resetup(void)
{
	if (gReducedService)
		return;           // the receiver stays off at critical battery
	MON_Retune();
	RADIO_SetupRegisters(true);
}

static uint8_t RegOverride(const uint8_t *r, uint16_t n, uint8_t *o, uint16_t *len, uint8_t *detail)
{
	const uint8_t  op      = r[0];
	const uint16_t seconds = get16(r + 1);
	const uint8_t  keyups  = r[3];
	const uint8_t  count   = r[4];

	if (op > 4)                   { *detail = 0; return V2_RANGE; }
	if (n != 5u + 6u * count)
		return V2_BAD_LENGTH;
	if (op != 1 && count)         { *detail = 4; return V2_RANGE; }

	switch (op) {
		case 1: {   // ADD
			if (gRegOverrideRamCount + count > REG_OVERRIDE_MAX) { *detail = 4; return V2_RANGE; }
			for (uint8_t i = 0; i < count; i++) {
				const uint8_t *e = r + 5 + 6 * i;
				if ((e[0] & 3u) == 0 || e[0] > 3) { *detail = 5 + 6 * i; return V2_RANGE; }
				if (!SETTINGS_RegOverrideAllowed(e[1])) { *detail = e[1]; return V2_REFUSED; }
			}
			bool rx = false;
			for (uint8_t i = 0; i < count; i++) {
				const uint8_t *e = r + 5 + 6 * i;
				RegOverride_t *d = &gRegOverridesRam[gRegOverrideRamCount++];
				d->phase   = e[0];
				d->reg     = e[1];
				d->andMask = get16(e + 2);
				d->orValue = get16(e + 4);
				rx |= e[0] & REG_OVERRIDE_RX;
			}
			OVR_SetExpiry(seconds, keyups);
			if (rx && gCurrentFunction != FUNCTION_TRANSMIT && !gReducedService)
				RADIO_ApplyRegOverrides(REG_OVERRIDE_RX);
			break;
		}
		case 2:     // CLEAR
			OVR_ClearRam();
			Resetup();
			break;
		case 3:     // COMMIT: the RAM table becomes the EEPROM table
		case 4:     // CLEAR_EEPROM
			if (!gSettingsBlockValid)
				return V2_EEPROM;
			if (op == 3) {
				memcpy(gRegOverrides, gRegOverridesRam, sizeof(gRegOverrides));
				gRegOverrideCount = gRegOverrideRamCount;
			}
			else
				gRegOverrideCount = 0;
			OVR_ClearRam();
			PARAMS_PersistOverrides();
			Resetup();
			break;
		default:    // LIST
			break;
	}

	uint16_t l = ListOverrides(o, gRegOverridesRam, gRegOverrideRamCount);
	// expiry fields go after n_ram in the reply
	memmove(o + 4, o + 1, l - 1);
	put16(o + 1, gRegOverrideRamCount ? OVR_SecondsLeft() : 0xFFFF);
	o[3] = gRegOverrideRamCount ? OVR_KeyupsLeft() : 0xFF;
	l += 3;
	l += ListOverrides(o + l, gRegOverrides, gRegOverrideCount);
	*len = l;
	return V2_OK;
}

static uint16_t Counters(const uint8_t *r, uint8_t *o)
{
	gCounters[CNT_EEPROM_BLOCKS] = gEepromBlocksWritten;
	o[0] = CNT_N;
	for (uint8_t i = 0; i < CNT_N; i++)
		put32(o + 1 + 4 * i, gCounters[i]);
	if (r[0] & 1u) {
		memset(gCounters, 0, sizeof(gCounters));
		gEepromBlocksWritten = 0;
	}
	return 1 + 4 * CNT_N;
}

// A 0x50xx frame. body/bodyLen: the body as the frame's inner header gives
// it; payloadLen: the frame's length field (id, length and body).
void V2_Handle(uint16_t id, const uint8_t *body, uint16_t bodyLen, uint16_t payloadLen)
{
	if (id >= 0x5080u)
		return;                                   // replies and events are not requests

	const uint8_t tag = (payloadLen >= 5) ? body[0] : 0;
	uint8_t      *o   = UART_FrameBody() + 4;     // reply body, after the reply header
	uint16_t      len = 0;
	uint8_t       detail = 0;
	uint8_t       st  = V2_OK;

	if (payloadLen < 5 || bodyLen != payloadLen - 4u || bodyLen > V2_MAX_REQUEST_BODY) {
		st = V2_BAD_LENGTH;
		goto reply;
	}

	const uint8_t *r = body + 1;
	const uint16_t n = bodyLen - 1;

	// the fixed request sizes; 0xFF: variable, checked by the command
	static const uint8_t kReqLen[] = { 0, 0, 10, 8, 0xFF, 0xFF, 1, 7, 3, 0xFF, 0xFF, 2, 1 };
	const uint16_t idx = id - V2_GET_INFO;
	if (idx >= sizeof(kReqLen)) {
		st = V2_UNKNOWN_CMD;
		goto reply;
	}
	if (kReqLen[idx] == 0xFF ? n < ((id == V2_REG_OVERRIDE) ? 5u : 1u) : n != kReqLen[idx]) {
		st = V2_BAD_LENGTH;
		goto reply;
	}

	switch (id) {
		case V2_GET_INFO:
			len = GetInfo(o);
			break;

		case V2_GET_STATUS:
			len = GetStatus(o);
			break;

		case V2_SUBSCRIBE:
			st  = Subscribe(r, o, &detail);
			len = 8;
			break;

		case V2_TIME_SYNC: {
			// 6.4: rx time from the frame parser, tx time taken just before
			// the reply's first byte is queued
			uint8_t *b = UART_FrameBody();
			uint32_t ms;
			uint16_t us;
			b[0] = tag;
			b[1] = V2_OK;
			put16(b + 2, gSerialLockMs);
			memcpy(o, r, 8);
			put32(o + 8, gRxMs);
			put16(o + 12, gRxUs);
			const bool exact = UART_SendStampedHeader(4 + 21, &ms, &us);
			put32(o + 14, ms);
			put16(o + 18, us);
			o[20] = exact;
			UART_SendStampedBody(id + 0x80, 4 + 21);
			return;
		}

		case V2_GET_PARAMS:
			st = GetParams(r, n, o, &len, &detail);
			break;

		case V2_SET_PARAMS:
			st = PARAMS_Set(r[0], r + 1, n - 1, &detail, o, &len);
			break;

		case V2_SAVE_PARAMS: {
			uint32_t mask;
			st = PARAMS_Save(r[0], &mask);
			put32(o, mask);
			len = 4;
			break;
		}

		case V2_LEVEL_TONE:
			st  = LevelTone(r, o, &detail);
			len = 3;
			break;

		case V2_REG_READ:
			st = RegRead(r, o, &len, &detail);
			break;

		case V2_REG_WRITE:
			st = RegWrite(r, n, o, &len, &detail);
			break;

		case V2_REG_OVERRIDE:
			st = RegOverride(r, n, o, &len, &detail);
			break;

		case V2_EVENT_REPLAY: {
			uint16_t first;
			uint8_t  count;
			EVT_Replay(get16(r), &first, &count);   // uses the frame buffer: reply built after
			put16(o, first);
			o[2] = count;
			put16(o + 3, EVT_OldestSeq());
			put16(o + 5, EVT_NextSeq());
			len = 7;
			break;
		}

		case V2_GET_COUNTERS:
			len = Counters(r, o);
			break;

		default:                                  // 0x500D to 0x507F, 0x5020 included
			st = V2_UNKNOWN_CMD;
			break;
	}

reply:
	if (st != V2_OK) {
		o[0] = detail;
		len  = 1;
	}
	Reply(id, tag, st, len);
}
