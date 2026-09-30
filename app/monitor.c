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
#include "app/monitor.h"
#include "app/v2.h"
#include "app/wire.h"
#include "driver/bk4819.h"
#include "functions.h"
#include "helper/battery.h"
#include "misc.h"
#include "packet.h"
#include "ptt.h"
#include "radio.h"
#include "settings.h"

#ifdef HOST_TEST
void AUDIO_AudioPathOnHost(void);
#define AUDIO_AudioPathOn AUDIO_AudioPathOnHost
#else
#include "audio.h"
#endif

// ------------------------------------------------------------ busy --

static bool     gBusy;
static bool     gSq;             // squelch source
static bool     gRssiBusy;       // RSSI source
static bool     gBelow;          // RSSI below close, since gBelowStart
static uint32_t gBelowStart;
static uint32_t gBusyEdgeMs;
static uint32_t gLastMs;
static uint32_t gNextRssiMs;
static uint32_t gBusyTimeMs;     // since the last heartbeat
static uint16_t gLastRssi;

static struct {
	uint32_t tOpen;
	uint32_t nextMs;
	uint16_t n;
	uint16_t afN;
	uint32_t rssiSum, noiseSum, glitchSum, afSum;
	uint16_t rssiMax, rssiMin, afMax;
	uint8_t  noiseMin, glitchMax;
	uint8_t  agcOpen;
} gBurst;

// ------------------------------------------------ transmit, stream --

static struct {
	uint32_t tPress;
	uint16_t lockDelay;
	uint8_t  flags;
	uint32_t tStart;
	uint32_t micSum;
	uint16_t micMax, micN;
	uint32_t nextMicMs;
} gTx;

static uint8_t  gStream[2 + 4 * 20];
static uint8_t  gStreamN;
static uint32_t gNextStreamMs;
static uint32_t gNextHeartbeatMs;
static uint32_t gLastActiveMs;   // last ms PTT was asserted or the radio transmitted
static uint8_t  gBatteryClass;

static struct {
	bool     on;
	uint8_t  gain;
	uint16_t word;
	uint32_t endMs;
} gTone;

static bool     gOvrTimed;
static uint32_t gOvrDeadline;
static uint8_t  gOvrKeyups = 0xFF;

static bool Due(uint32_t now, uint32_t t)
{
	return (int32_t)(now - t) >= 0;
}

uint8_t MON_AgcByte(uint16_t r)
{
	return ((r >> 8) & 0x80u) | ((r >> 12) & 0x07u);
}

static uint8_t BatteryClass(uint8_t level)
{
	return (level == 0) ? 2 : (level == 7) ? 3 : (level == 1) ? 1 : 0;
}

void MON_Init(void)
{
	gBatteryClass = BatteryClass(gBatteryDisplayLevel);
	gTone.on = false;
	MON_SubscriptionChanged();
}

void MON_SubscriptionChanged(void)
{
	gNextHeartbeatMs = g_ms + gSub.heartbeatMs;
	gNextStreamMs    = g_ms;
	gStreamN         = 0;
}

bool MON_Busy(void)
{
	return gBusy;
}

uint16_t MON_BusyAge(void)
{
	const uint32_t age = g_ms - gBusyEdgeMs;
	return (age > 0xFFFF) ? 0xFFFF : (uint16_t)age;
}

bool MON_Deferred(void)
{
	return !(gSub.options & SUB_LIVE_TX) && (g_ms - gLastActiveMs) < 2;
}

static void Sample(uint32_t now)
{
	const uint16_t rssi   = BK4819_ReadRegister(BK4819_REG_67) & 0x01FF;
	const uint8_t  noise  = BK4819_ReadRegister(BK4819_REG_65) & 0x007F;
	const uint8_t  glitch = BK4819_ReadRegister(BK4819_REG_63) & 0x00FF;
	const uint16_t af     = BK4819_ReadRegister(BK4819_REG_64) & 0x7FFF;

	if (gBurst.n == 0) {
		gBurst.rssiMax  = gBurst.rssiMin = rssi;
		gBurst.noiseMin = noise;
		gBurst.glitchMax = glitch;
		gBurst.afMax    = af;
	}
	gBurst.n++;
	gBurst.afN++;
	gBurst.rssiSum   += rssi;
	gBurst.noiseSum  += noise;
	gBurst.glitchSum += glitch;
	gBurst.afSum     += af;
	if (rssi > gBurst.rssiMax)     gBurst.rssiMax   = rssi;
	if (rssi < gBurst.rssiMin)     gBurst.rssiMin   = rssi;
	if (noise < gBurst.noiseMin)   gBurst.noiseMin  = noise;
	if (glitch > gBurst.glitchMax) gBurst.glitchMax = glitch;
	if (af > gBurst.afMax)         gBurst.afMax     = af;

	const uint8_t period = gSub.burstPeriodMs ? gSub.burstPeriodMs : BURST_PERIOD_DEFAULT_MS;
	gBurst.nextMs = now + period;
	gLastRssi = rssi;
}

// A busy edge: CD, and RX_BURST after a closing CD. With read false (the
// radio is about to transmit, or the receiver is being set up again) the
// last sampled values are reported.
static void Edge(bool busy, uint8_t cause, uint32_t now, bool read)
{
	uint8_t p[EVT_PAYLOAD_MAX];
	uint8_t noise = 0, glitch = 0;
	uint16_t rssi = gLastRssi;

	gBusy       = busy;
	gBusyEdgeMs = now;

	if (busy) {
		memset(&gBurst, 0, sizeof(gBurst));
		gBurst.tOpen   = now;
		gBurst.agcOpen = MON_AgcByte(BK4819_ReadRegister(BK4819_REG_7E));
		Sample(now);
		rssi   = gLastRssi;
		noise  = gBurst.noiseMin;
		glitch = gBurst.glitchMax;
		gCounters[CNT_BUSY_OPENS]++;
	}
	else if (read) {
		rssi   = BK4819_ReadRegister(BK4819_REG_67) & 0x01FF;
		noise  = BK4819_ReadRegister(BK4819_REG_65) & 0x007F;
		glitch = BK4819_ReadRegister(BK4819_REG_63) & 0x00FF;
		gLastRssi = rssi;
	}

	p[0] = busy;
	p[1] = (gSq ? 1u : 0u) | (gRssiBusy ? 2u : 0u);
	p[2] = cause;
	put16(p + 3, rssi);
	p[5] = noise;
	p[6] = glitch;
	EVT_Store(EV_CD, now, p, 7);

	if (busy)
		return;

	const uint16_t n = gBurst.n ? gBurst.n : 1;
	put32(p + 0, gBurst.tOpen);
	put32(p + 4, now - gBurst.tOpen);
	put16(p + 8, gBurst.n);
	put16(p + 10, (uint16_t)(gBurst.rssiSum / n));
	put16(p + 12, gBurst.rssiMax);
	put16(p + 14, gBurst.rssiMin);
	p[16] = (uint8_t)(gBurst.noiseSum / n);
	p[17] = gBurst.noiseMin;
	p[18] = (uint8_t)(gBurst.glitchSum / n);
	p[19] = gBurst.glitchMax;
	put16(p + 20, gBurst.afN ? (uint16_t)(gBurst.afSum / gBurst.afN) : 0xFFFF);
	put16(p + 22, gBurst.afN ? gBurst.afMax : 0xFFFF);
	p[24] = gBurst.agcOpen;
	p[25] = read ? MON_AgcByte(BK4819_ReadRegister(BK4819_REG_7E)) : gBurst.agcOpen;
	put16(p + 26, 0x7FFF);          // frequency error: not available (Q1)
	p[28] = cause;
	EVT_Store(EV_RX_BURST, now, p, 29);
}

void MON_ForceClose(uint8_t cause)
{
	const bool wasSq = gSq, wasRssi = gRssiBusy;
	gSq = gRssiBusy = gBelow = false;
	if (gBusy)
		Edge(false, cause | (wasSq ? CD_CAUSE_SQUELCH : 0) | (wasRssi ? CD_CAUSE_RSSI : 0), g_ms, false);
}

void MON_Retune(void)
{
	TONE_Stop(TONE_END_RETUNE);
	MON_ForceClose(CD_CAUSE_RETUNE);
}

// 8.2: the OR of the enabled sources, the squelch result every 1 ms and
// the RSSI every 2 ms
static void BusyPoll(uint32_t now)
{
	const uint8_t src = gV2.BUSY_SOURCE;
	uint8_t cause = 0;

	if (src & BUSY_SOURCE_SQUELCH) {
		const bool sq = (BK4819_ReadRegister(BK4819_REG_0C) >> 1) & 1u;
		if (sq != gSq)
			cause |= CD_CAUSE_SQUELCH;
		gSq = sq;
	}
	else
		gSq = false;

	if (!(src & BUSY_SOURCE_RSSI))
		gRssiBusy = gBelow = false;
	else if (Due(now, gNextRssiMs)) {
		gNextRssiMs = now + 2;
		const uint16_t rssi = BK4819_ReadRegister(BK4819_REG_67) & 0x01FF;
		gLastRssi = rssi;
		if (rssi >= gV2.BUSY_RSSI_OPEN) {
			gBelow = false;
			if (!gRssiBusy)
				cause |= CD_CAUSE_RSSI;
			gRssiBusy = true;
		}
		else if (rssi < gV2.BUSY_RSSI_CLOSE) {
			if (!gBelow) {
				gBelow      = true;
				gBelowStart = now;
			}
			if (gRssiBusy && now - gBelowStart >= gV2.BUSY_HANG_MS) {
				gRssiBusy = false;
				cause |= CD_CAUSE_RSSI;
			}
		}
		else
			gBelow = false;
	}

	const bool busy = gSq || gRssiBusy;
	if (busy != gBusy)
		Edge(busy, cause, now, true);
	else if (busy && Due(now, gBurst.nextMs))
		Sample(now);
}

// ---------------------------------------------------- the 1 ms work --

static void Stream(uint32_t now, bool rx, bool deferred)
{
	if (!gSub.rssiPeriodMs || !EVT_Wanted(EV_RSSI_STREAM) || !rx) {
		gStreamN = 0;             // paused while transmitting
		return;
	}
	if (!Due(now, gNextStreamMs))
		return;
	gNextStreamMs = now + gSub.rssiPeriodMs;

	uint8_t *s = gStream + 2 + 4 * gStreamN;
	put16(s, BK4819_ReadRegister(BK4819_REG_67) & 0x01FF);
	s[2] = BK4819_ReadRegister(BK4819_REG_65) & 0x007F;
	s[3] = BK4819_ReadRegister(BK4819_REG_63) & 0x00FF;
	if (++gStreamN >= gSub.rssiBatch) {
		gStream[0] = gSub.rssiPeriodMs;
		gStream[1] = gStreamN;
		EVT_Ephemeral(EV_RSSI_STREAM, now, gStream, 2 + 4 * gStreamN, deferred, now);
		gStreamN = 0;
	}
}

static void Heartbeat(uint32_t now, bool rx, bool deferred)
{
	if (!gSub.heartbeatMs || !EVT_Wanted(EV_HEARTBEAT) || !Due(now, gNextHeartbeatMs))
		return;
	gNextHeartbeatMs = now + gSub.heartbeatMs;

	uint8_t p[12];
	put16(p, 0);                                    // t_us, filled in when queued
	p[2] = V2_Flags1();
	p[3] = V2_State();
	put16(p + 4, rx ? (BK4819_ReadRegister(BK4819_REG_67) & 0x01FF) : 0);
	put16(p + 6, gBatteryVoltageAverage * 10u);
	put16(p + 8, (gBusyTimeMs > 0xFFFF) ? 0xFFFF : (uint16_t)gBusyTimeMs);
	put16(p + 10, gSerialLockMs);
	gBusyTimeMs = 0;
	EVT_Ephemeral(EV_HEARTBEAT, now, p, sizeof(p), deferred, now);
}

void MON_Service(void)
{
	const uint32_t now = g_ms;
	if (now == gLastMs)
		return;
	const uint32_t elapsed = now - gLastMs;
	gLastMs = now;

	const bool tx = gCurrentFunction == FUNCTION_TRANSMIT;
	PttState_t ptt;
	PTT_GetState(&ptt);
	if (tx || ptt.pressed || ptt.candidate)
		gLastActiveMs = now;
	const bool deferred = MON_Deferred();

	if (gBusy)
		gBusyTimeMs += elapsed;

	const bool rx = !tx && !gReducedService;
	if (rx)
		BusyPoll(now);

	if (tx && Due(now, gTx.nextMicMs)) {
		// TX mic amplitude, every 10 ms
		gTx.nextMicMs = now + 10;
		const uint16_t mic = BK4819_ReadRegister(BK4819_REG_64) & 0x7FFF;
		gTx.micSum += mic;
		if (mic > gTx.micMax)
			gTx.micMax = mic;
		gTx.micN++;
	}

	Stream(now, rx, deferred);
	Heartbeat(now, rx, deferred);

	if (gTone.on && Due(now, gTone.endMs))
		TONE_Stop(TONE_END_ELAPSED);

	// a RAM override trial ends at its bound, never during a transmission
	if (gRegOverrideRamCount && rx && ((gOvrTimed && Due(now, gOvrDeadline)) || gOvrKeyups == 0)) {
		uint8_t p[2] = { gOvrKeyups == 0, gRegOverrideRamCount };
		OVR_ClearRam();
		MON_Retune();
		RADIO_SetupRegisters(true);
		EVT_Store(EV_OVERRIDE_EXPIRED, now, p, sizeof(p));
	}
}

// ------------------------------------------------------- transmit --

void MON_KeyInfo(uint32_t tPress, uint16_t lockDelay, bool late, bool busyAtPress)
{
	gTx.tPress    = tPress;
	gTx.lockDelay = lockDelay;
	gTx.flags     = (busyAtPress ? 1u : 0u) | (late ? 2u : 0u);
	if (late)
		gCounters[CNT_LATE_KEYS]++;
}

void MON_TxStarted(void)
{
	const uint32_t now = g_ms;
	uint8_t p[15];

	MON_ForceClose(CD_CAUSE_TX);
	gStreamN     = 0;
	gTx.tStart   = now;
	gTx.micSum   = 0;
	gTx.micMax   = 0;
	gTx.micN     = 0;
	gTx.nextMicMs = now;
	gCounters[CNT_TRANSMISSIONS]++;
	if (gRegOverrideRamCount && gOvrKeyups != 0xFF && gOvrKeyups)
		gOvrKeyups--;

	const bool narrow = gVfo->CHANNEL_BANDWIDTH == BANDWIDTH_NARROW;
	uint16_t dev = narrow ? gEeprom.DEVIATION_NARROW : gEeprom.DEVIATION_WIDE;
	if (dev > PKT_DEVIATION_MAX)
		dev = PKT_DEVIATION_MAX;

	put32(p + 0, gTx.tPress);
	put32(p + 4, gVfo->Frequency * 10u);
	p[8] = gVfo->OUTPUT_POWER;
	p[9] = gVfo->CHANNEL_BANDWIDTH;
	put16(p + 10, dev);
	put16(p + 12, gTx.lockDelay);
	p[14] = gTx.flags;
	EVT_Store(EV_TX_START, now, p, sizeof(p));
}

void MON_TxEnded(uint8_t reason, uint32_t tRelease)
{
	uint8_t p[19];
	const uint32_t off = gTxCarrierOffMs;

	gBusy = gSq = gRssiBusy = gBelow = false;   // re-evaluated from now on
	if (reason == TXEND_TIMEOUT)
		gCounters[CNT_TX_TIMEOUTS]++;

	put32(p + 0, gTx.tStart);
	put32(p + 4, reason == TXEND_RELEASE ? tRelease : off);
	put32(p + 8, g_ms);
	p[12] = reason;
	put16(p + 13, gTx.micN ? (uint16_t)(gTx.micSum / gTx.micN) : 0xFFFF);
	put16(p + 15, gTx.micN ? gTx.micMax : 0xFFFF);
	put16(p + 17, gTx.micN);
	EVT_Store(EV_TX_END, off, p, sizeof(p));
}

void MON_TxRefused(uint32_t tPress, uint8_t reason, uint16_t detail)
{
	uint8_t p[7];
	put32(p, tPress);
	p[4] = reason;
	put16(p + 5, detail);
	gCounters[CNT_TX_REFUSED]++;
	EVT_Store(EV_TX_REFUSED, g_ms, p, sizeof(p));
}

void MON_Slice500ms(void)
{
	const uint8_t c = BatteryClass(gBatteryDisplayLevel);
	if (c == gBatteryClass)
		return;
	gBatteryClass = c;
	uint8_t p[4] = { c, gBatteryDisplayLevel };
	put16(p + 2, gBatteryVoltageAverage * 10u);
	EVT_Store(EV_BATTERY, g_ms, p, sizeof(p));
}

// ----------------------------------------------------------- tone --

static void ToneApply(void)
{
	// tone 1 into the AF output instead of the receiver audio (REG_47
	// AF source 2), whatever the squelch; the speaker amplifier (the K1
	// audio out) is otherwise first switched on at the first squelch open
	AUDIO_AudioPathOn();
	BK4819_WriteRegister(BK4819_REG_71, gTone.word);
	BK4819_WriteRegister(BK4819_REG_70, 0x8000u | ((uint16_t)gTone.gain << 8));
	BK4819_SetAF(BK4819_AF_ALAM);
}

bool TONE_Active(void)
{
	return gTone.on;
}

void TONE_Start(uint16_t word, uint8_t gain, uint16_t durationMs)
{
	TONE_Stop(TONE_END_REPLACED);
	gTone.on    = true;
	gTone.word  = word;
	gTone.gain  = gain;
	gTone.endMs = g_ms + durationMs;
	ToneApply();
}

void TONE_Stop(uint8_t reason)
{
	if (!gTone.on)
		return;
	gTone.on = false;
	BK4819_WriteRegister(BK4819_REG_70, 0);
	if (gCurrentFunction == FUNCTION_RECEIVE || gCurrentFunction == FUNCTION_MONITOR)
		BK4819_SetAF(BK4819_AF_FM);
	else if (gCurrentFunction != FUNCTION_TRANSMIT)
		BK4819_SetAF(BK4819_AF_MUTE);
	EVT_Store(EV_TONE_END, g_ms, &reason, 1);
}

void MON_AfterRxSetup(void)
{
	if (gTone.on)
		ToneApply();
}

// ------------------------------------------------ override trials --

void OVR_SetExpiry(uint16_t seconds, uint8_t keyups)
{
	gOvrTimed    = seconds != 0;
	gOvrDeadline = g_ms + seconds * 1000u;
	gOvrKeyups   = keyups ? keyups : 0xFF;
}

void OVR_ClearRam(void)
{
	gRegOverrideRamCount = 0;
	gOvrTimed  = false;
	gOvrKeyups = 0xFF;
}

uint16_t OVR_SecondsLeft(void)
{
	if (!gOvrTimed)
		return 0xFFFF;
	const int32_t left = (int32_t)(gOvrDeadline - g_ms);
	return (left <= 0) ? 0 : (uint16_t)((left + 999) / 1000);
}

uint8_t OVR_KeyupsLeft(void)
{
	return gOvrKeyups;
}
