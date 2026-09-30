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
#include "app/params.h"
#include "app/v2.h"
#include "app/wire.h"
#include "driver/backlight.h"
#include "driver/bk4819.h"
#include "driver/eeprom.h"
#include "frequencies.h"
#include "functions.h"
#include "misc.h"
#include "packet.h"
#include "ptt.h"
#include "radio.h"
#include "settings.h"

#define BIT(id)            (1u << (id))
#define PARAMS_OPERATING   (BIT(P_FREQ_HZ) | BIT(P_POWER) | BIT(P_BANDWIDTH))
#define PARAMS_RETUNE      (PARAMS_OPERATING | BIT(P_BUSY_SQL_LEVEL) | BIT(P_BUSY_SQL_RAW) | BIT(P_AGC_FIX))
#define PARAMS_BLOCK_A     (BIT(P_BUSY_SQL_LEVEL) | BIT(P_TX_TIMEOUT_S) | BIT(P_MIC_GAIN) | BIT(P_DEV_WIDE) | BIT(P_DEV_NARROW))
#define PARAMS_BLOCK_B     (BIT(P_RX_GAIN) | BIT(P_RX_DAC_GAIN) | BIT(P_BACKLIGHT) | BIT(P_KEY_LOCK))
#define PARAMS_TIMING      (BIT(P_PTT_PRESS_MS) | BIT(P_PTT_RELEASE_MS) | BIT(P_PA_ENABLE_DELAY_MS) | BIT(P_PA_BIAS_DELAY_MS))
#define PARAMS_V2          (BIT(P_SERIAL_LOCK_MS) | BIT(P_BUSY_SOURCE) | BIT(P_BUSY_RSSI_OPEN) | BIT(P_BUSY_RSSI_CLOSE) | BIT(P_BUSY_HANG_MS))

// 0x07 (SQUELCH) is retired: size 0, not supported
static const uint8_t kSize[P_LAST + 1] = {
	0, 4, 1, 1, 2, 2, 1, 0, 1, 1, 1, 1, 1, 1, 1, 2, 1, 2, 2, 1, 6, 1, 1, 1, 1, 1
};

// EEPROM blocks waiting to be written, in this order (V2_B before V2_A,
// so a fresh v2 block is blanked before its version byte makes it count)
enum { J_SET_A, J_SET_B, J_TIMING, J_OPERATING, J_V2_B, J_V2_A, J_OVR0 };

static struct {
	EEPROM_Config_t e;
	V2_Config_t     v;
	VFO_Info_t      vfo;
} gStored;                            // what a power-on would load (StoredView)

static uint32_t gLast[P_LAST + 1];    // values at the last PARAMS_Changed

static uint32_t gJobs;
static uint32_t gPMask;               // parameters with a value in gPVal
static uint32_t gPVal[P_LAST + 1];
static uint8_t  gPStep;
static bool     gPSub;
static uint32_t gPSubMask;
static uint16_t gPSubHeartbeat;
static uint8_t  gPSubOptions;

uint8_t PARAMS_Size(uint8_t id)
{
	return (id <= P_LAST) ? kSize[id] : 0;
}

static void SqlRaw(const VFO_Info_t *f, uint8_t *out)
{
	out[0] = f->SquelchOpenRSSIThresh;
	out[1] = f->SquelchCloseRSSIThresh;
	out[2] = f->SquelchOpenNoiseThresh;
	out[3] = f->SquelchCloseNoiseThresh;
	out[4] = f->SquelchOpenGlitchThresh;
	out[5] = f->SquelchCloseGlitchThresh;
}

static uint32_t Value(uint8_t id, const EEPROM_Config_t *e, const V2_Config_t *v, const VFO_Info_t *f)
{
	switch (id) {
		case P_FREQ_HZ:            return f->Frequency * 10u;
		case P_POWER:              return f->OUTPUT_POWER;
		case P_BANDWIDTH:          return f->CHANNEL_BANDWIDTH;
		case P_DEV_WIDE:           return e->DEVIATION_WIDE;
		case P_DEV_NARROW:         return e->DEVIATION_NARROW;
		case P_MIC_GAIN:           return e->MIC_GAIN;
		case P_BUSY_SQL_LEVEL:     return e->BUSY_LEVEL;
		case P_RX_GAIN:            return e->RX_GAIN;
		case P_RX_DAC_GAIN:        return e->RX_DAC_GAIN;
		case P_TX_TIMEOUT_S:       return gTxTimeoutSeconds[e->TX_TIMEOUT];
		case P_PTT_PRESS_MS:       return e->PTT_PRESS_MS;
		case P_PTT_RELEASE_MS:     return e->PTT_RELEASE_MS;
		case P_PA_ENABLE_DELAY_MS: return e->PA_ENABLE_DELAY_MS;
		case P_PA_BIAS_DELAY_MS:   return e->PA_BIAS_DELAY_MS;
		case P_SERIAL_LOCK_MS:     return v->SERIAL_LOCK_MS;
		case P_BUSY_SOURCE:        return v->BUSY_SOURCE;
		case P_BUSY_RSSI_OPEN:     return v->BUSY_RSSI_OPEN;
		case P_BUSY_RSSI_CLOSE:    return v->BUSY_RSSI_CLOSE;
		case P_BUSY_HANG_MS:       return v->BUSY_HANG_MS;
		case P_BUSY_SQL_RAW: {          // compared only: fold the six bytes
			uint8_t r[6];
			SqlRaw(f, r);
			return get32(r) ^ ((uint32_t)get16(r + 4) << 7);
		}
		case P_AGC_FIX:            return gAgcFix;
		case P_AFC:                return gAfcOn;
		case P_BACKLIGHT:          return e->BACKLIGHT_TIME;
		case P_KEY_LOCK:           return e->KEY_LOCK;
		default:                   return 0;
	}
}

static uint32_t Live(uint8_t id)
{
	return Value(id, &gEeprom, &gV2, gVfo);
}

// gStored follows every EEPROM write, from any source: the driver flags
// each block it actually writes (gEepromChanged), and the view is re-read
// here, the next time it is asked for. So a keypad or menu save needs no
// call of its own, a save of several blocks is re-read once, and nothing
// is read unless a host asks. Never during a transmission (nothing is
// written then either, and every command frame ends one first): the view
// stays as it was and is re-read after.
static void StoredView(void)
{
	if (gEepromChanged && gCurrentFunction != FUNCTION_TRANSMIT)
		PARAMS_RefreshStored();
}

static uint32_t Stored(uint8_t id)
{
	StoredView();
	return Value(id, &gStored.e, &gStored.v, &gStored.vfo);
}

uint8_t PARAMS_Get(uint8_t id, bool stored, uint8_t *out)
{
	const uint8_t size = PARAMS_Size(id);
	if (stored)
		StoredView();
	if (id == P_BUSY_SQL_RAW)
		SqlRaw(stored ? &gStored.vfo : gVfo, out);
	else {
		const uint32_t u = stored ? Stored(id) : Live(id);
		for (uint8_t i = 0; i < size; i++)
			out[i] = (uint8_t)(u >> (8 * i));
	}
	return size;
}

void PARAMS_RefreshStored(void)
{
	uint8_t d[16], t[8];

	gEepromChanged = false;

	EEPROM_ReadBuffer(SETTINGS_PKT_BLOCK, d, 16);
	const bool valid = d[0] == SETTINGS_PKT_VERSION;
	EEPROM_ReadBuffer(SETTINGS_TIMING, t, 8);
	if (!valid) {
		memset(d, 0xFF, sizeof(d));
		memset(t, 0xFF, sizeof(t));
	}
	SETTINGS_Decode(d, t, &gStored.e);
	if (gStored.e.RX_GAIN > PKT_RX_GAIN_MAX)
		gStored.e.RX_GAIN = SETTINGS_FactoryRxGain();
	EEPROM_ReadBuffer(SETTINGS_V2_BLOCK, d, 16);
	SETTINGS_DecodeV2(d, valid, &gStored.v);

	// the operating channel as a power-on would load it
	EEPROM_ReadBuffer(SETTINGS_OPERATING, d, 8);
	memset(&gStored.vfo, 0, sizeof(gStored.vfo));
	if (!valid || !SETTINGS_DecodeOperating(d, &gStored.vfo))
		SETTINGS_ImportOldFrequency(&gStored.vfo);
}

static void Snapshot(uint32_t *v)
{
	for (uint8_t id = 1; id <= P_LAST; id++)
		v[id] = Live(id);
}

void PARAMS_Init(void)
{
	PARAMS_RefreshStored();
	Snapshot(gLast);
}

void PARAMS_Changed(uint8_t source)
{
	uint32_t mask = 0;
	for (uint8_t id = 1; id <= P_LAST; id++) {
		const uint32_t v = Live(id);
		if (v != gLast[id])
			mask |= BIT(id);
		gLast[id] = v;
	}
	if (!mask)
		return;
	uint8_t p[5] = { source };
	put32(p + 1, mask);
	EVT_Store(EV_PARAMS_CHANGED, g_ms, p, sizeof(p));
}

bool PARAMS_LiveDiffers(void)
{
	for (uint8_t id = 1; id <= P_LAST; id++)
		if (!(PARAMS_RAM_ONLY & BIT(id)) && Live(id) != Stored(id))
			return true;
	return false;
}

bool PARAMS_PersistPending(void)
{
	return gJobs != 0;
}

// A legacy EEPROM write session takes over: queued v2 writes are dropped
// rather than written over it later (the reload then reads the result).
void PARAMS_CancelPersist(void)
{
	gJobs  = 0;
	gPMask = 0;
	gPSub  = false;
}

// ------------------------------------------------------ validation --

static int8_t TimeoutIndex(uint32_t s)
{
	for (uint8_t i = 0; i < ARRAY_SIZE(gTxTimeoutSeconds); i++)
		if (gTxTimeoutSeconds[i] == s)
			return (int8_t)i;
	return -1;
}

static bool InRange(uint8_t id, uint32_t u, const uint8_t *raw)
{
	switch (id) {
		case P_FREQ_HZ:            return u >= 50000000u && u <= 600000000u && (u % 10u) == 0 && FREQUENCY_IsReceivable(u / 10u);
		case P_POWER:              return u <= OUTPUT_POWER_HIGH;
		case P_BANDWIDTH:          return u <= BANDWIDTH_NARROW;
		case P_DEV_WIDE:
		case P_DEV_NARROW:         return u <= PKT_DEVIATION_MAX;
		case P_MIC_GAIN:           return u <= PKT_MIC_GAIN_MAX;
		case P_BUSY_SQL_LEVEL:     return u >= 1 && u <= 9;
		case P_RX_GAIN:            return u <= PKT_RX_GAIN_MAX;
		case P_RX_DAC_GAIN:        return u <= PKT_RX_DAC_GAIN_MAX;
		case P_TX_TIMEOUT_S:       return TimeoutIndex(u) >= 0;
		case P_PTT_PRESS_MS:       return u >= PTT_PRESS_MIN_MS && u <= PTT_DEBOUNCE_MAX_MS;
		case P_PTT_RELEASE_MS:     return u >= PTT_RELEASE_MIN_MS && u <= PTT_DEBOUNCE_MAX_MS;
		case P_PA_ENABLE_DELAY_MS: return u >= PA_ENABLE_DELAY_MIN_MS && u <= PA_DELAY_MAX_MS;
		case P_PA_BIAS_DELAY_MS:   return u <= PA_DELAY_MAX_MS;
		case P_SERIAL_LOCK_MS:     return u <= SERIAL_LOCK_MAX_MS && (u % 10u) == 0;
		case P_BUSY_SOURCE:        return u >= 1 && u <= 3;
		case P_BUSY_RSSI_OPEN:
		case P_BUSY_RSSI_CLOSE:    return u <= 511;
		case P_BUSY_HANG_MS:       return u <= 250;
		case P_BUSY_SQL_RAW:            return raw[2] <= 127 && raw[3] <= 127;
		case P_AGC_FIX:            return u == 0xFF || u <= 7;
		case P_AFC:
		case P_KEY_LOCK:           return u <= 1;
		case P_BACKLIGHT:          return u <= 7;
		default:                   return false;
	}
}

static uint8_t Lowest(uint32_t mask)
{
	uint8_t id = 0;
	while (!(mask & 1u)) {
		mask >>= 1;
		id++;
	}
	return id;
}

// ----------------------------------------------------------- apply --

static void SetFrequency(uint32_t f10)
{
	gVfo->Frequency = f10;
	gVfo->Band      = FREQUENCY_GetBand(f10);

	// a step that holds the frequency, so a keypad step starts from it
	if (FREQUENCY_RoundToStep(f10, gVfo->StepFrequency) != f10) {
		for (int i = STEP_N_ELEM - 1; i >= 0; i--) {
			const STEP_Setting_t s = FREQUENCY_GetStepIdxFromSortedIdx(i);
			if (FREQUENCY_RoundToStep(f10, gStepFrequencyTable[s]) == f10) {
				gVfo->STEP_SETTING  = s;
				gVfo->StepFrequency = gStepFrequencyTable[s];
				break;
			}
		}
	}
}

static void Apply(uint32_t set, const uint32_t *val, const uint8_t *raw, uint32_t changed)
{
	for (uint8_t id = 1; id <= P_LAST; id++) {
		if (!(set & BIT(id)))
			continue;
		const uint32_t u = val[id];
		switch (id) {
			case P_FREQ_HZ:            SetFrequency(u / 10u); break;
			case P_POWER:              gVfo->OUTPUT_POWER = u; break;
			case P_BANDWIDTH:          gVfo->CHANNEL_BANDWIDTH = u; break;
			case P_DEV_WIDE:           gEeprom.DEVIATION_WIDE = u; break;
			case P_DEV_NARROW:         gEeprom.DEVIATION_NARROW = u; break;
			case P_MIC_GAIN:           gEeprom.MIC_GAIN = u; break;
			case P_BUSY_SQL_LEVEL:     gEeprom.BUSY_LEVEL = u; gSqlRawActive = false; break;
			case P_RX_GAIN:            gEeprom.RX_GAIN = u; break;
			case P_RX_DAC_GAIN:        gEeprom.RX_DAC_GAIN = u; break;
			case P_TX_TIMEOUT_S:       gEeprom.TX_TIMEOUT = TimeoutIndex(u); break;
			case P_PTT_PRESS_MS:       gEeprom.PTT_PRESS_MS = u; break;
			case P_PTT_RELEASE_MS:     gEeprom.PTT_RELEASE_MS = u; break;
			case P_PA_ENABLE_DELAY_MS: gEeprom.PA_ENABLE_DELAY_MS = u; break;
			case P_PA_BIAS_DELAY_MS:   gEeprom.PA_BIAS_DELAY_MS = u; break;
			case P_SERIAL_LOCK_MS:     gV2.SERIAL_LOCK_MS = u; break;
			case P_BUSY_SOURCE:        gV2.BUSY_SOURCE = u; break;
			case P_BUSY_RSSI_OPEN:     gV2.BUSY_RSSI_OPEN = u; break;
			case P_BUSY_RSSI_CLOSE:    gV2.BUSY_RSSI_CLOSE = u; break;
			case P_BUSY_HANG_MS:       gV2.BUSY_HANG_MS = u; break;
			case P_BUSY_SQL_RAW:            memcpy(gSqlRaw, raw, 6); gSqlRawActive = true; break;
			case P_AGC_FIX:            gAgcFix = u; break;
			case P_AFC:                gAfcOn = u; break;
			case P_BACKLIGHT:          gEeprom.BACKLIGHT_TIME = u; break;
			case P_KEY_LOCK:           gEeprom.KEY_LOCK = u; break;
		}
	}

	if (changed & PARAMS_RETUNE) {
		// the receiver is set up once, for everything that changed
		MON_Retune();
		RADIO_ConfigureSquelchAndOutputPower(gVfo);
		RADIO_SetupRegisters(true);
	}
	else if (changed & (BIT(P_RX_GAIN) | BIT(P_RX_DAC_GAIN))) {
		RADIO_SetRxAudio();
		RADIO_ApplyRegOverrides(REG_OVERRIDE_RX);
	}
	if (changed & BIT(P_AFC))
		BK4819_SetRegValue(afcDisableRegSpec, !gAfcOn);
	if (changed & BIT(P_BACKLIGHT))
		BACKLIGHT_TurnOn();

	gUpdateDisplay = true;
	gUpdateStatus  = true;
}

// ------------------------------------------------------- persisting --

// Queue the live values of these parameters for writing (called after
// they were applied, so live is what was asked for).
static void QueuePersist(uint32_t mask)
{
	if (mask & BIT(P_FREQ_HZ))
		mask |= BIT(P_POWER) | BIT(P_BANDWIDTH);   // stored together, with the step
	for (uint8_t id = 1; id <= P_LAST; id++)
		if (mask & BIT(id))
			gPVal[id] = Live(id);
	gPMask |= mask;

	if (mask & PARAMS_OPERATING) {
		gPStep = gVfo->STEP_SETTING;
		gJobs |= BIT(J_OPERATING);
	}
	if (mask & PARAMS_BLOCK_A) gJobs |= BIT(J_SET_A);
	if (mask & PARAMS_BLOCK_B) gJobs |= BIT(J_SET_B);
	if (mask & PARAMS_TIMING)  gJobs |= BIT(J_TIMING);
	if (mask & PARAMS_V2)      gJobs |= BIT(J_V2_A) | (gV2.valid ? 0 : BIT(J_V2_B));
}

bool PARAMS_PersistSubscription(uint32_t mask, uint8_t options, uint16_t heartbeatMs)
{
	if (!gSettingsBlockValid)
		return false;
	gPSub          = true;
	gPSubMask      = mask;
	gPSubOptions   = options;
	gPSubHeartbeat = heartbeatMs;
	gJobs |= BIT(J_V2_B) | (gV2.valid ? 0 : BIT(J_V2_A));
	return true;
}

bool PARAMS_PersistOverrides(void)
{
	if (!gSettingsBlockValid)
		return false;
	gJobs |= 0xFFu << J_OVR0;
	return true;
}

static bool Has(uint8_t id)
{
	return (gPMask >> id) & 1u;
}

static void Patch8(uint8_t *b, uint8_t id)
{
	if (Has(id))
		*b = (uint8_t)gPVal[id];
}

static void Patch16(uint8_t *b, uint8_t id)
{
	if (Has(id))
		put16(b, (uint16_t)gPVal[id]);
}

static uint8_t ReadByte(uint16_t a)
{
	uint8_t v;
	EEPROM_ReadBuffer(a, &v, 1);
	return v;
}

void PARAMS_PersistService(bool allowed)
{
	if (!gJobs || !allowed)
		return;

	const uint8_t j = Lowest(gJobs);
	gJobs &= ~BIT(j);

	uint8_t  b[8];
	uint16_t addr;
	const bool blockValid = ReadByte(SETTINGS_PKT_BLOCK) == SETTINGS_PKT_VERSION;
	const bool v2Valid    = blockValid && ReadByte(SETTINGS_V2_BLOCK) == SETTINGS_V2_VERSION;

	if (j >= J_OVR0) {
		const uint8_t i = j - J_OVR0;
		if (!blockValid)
			goto done;               // the settings block went away meanwhile
		addr = SETTINGS_REG_OVERRIDES + i * 8u;
		memset(b, 0xFF, sizeof(b));
		if (i < gRegOverrideCount) {
			const RegOverride_t *o = &gRegOverrides[i];
			b[0] = o->phase;
			b[1] = o->reg;
			put16(b + 2, o->andMask);
			put16(b + 4, o->orValue);
		}
	}
	else {
		static const uint16_t kAddr[] = { 0x1D00, 0x1D08, SETTINGS_TIMING, SETTINGS_OPERATING, SETTINGS_V2_BLOCK + 8, SETTINGS_V2_BLOCK };
		addr = kAddr[j];
		if (!blockValid)
			goto done;               // the settings block went away meanwhile
		EEPROM_ReadBuffer(addr, b, 8);
		switch (j) {
			case J_SET_A:
				Patch8(b + 1, P_BUSY_SQL_LEVEL);
				if (Has(P_TX_TIMEOUT_S))
					b[2] = TimeoutIndex(gPVal[P_TX_TIMEOUT_S]);
				Patch8(b + 3, P_MIC_GAIN);
				Patch16(b + 4, P_DEV_WIDE);
				Patch16(b + 6, P_DEV_NARROW);
				break;
			case J_SET_B:
				Patch8(b + 0, P_RX_GAIN);
				Patch8(b + 1, P_RX_DAC_GAIN);
				Patch8(b + 2, P_BACKLIGHT);
				Patch8(b + 4, P_KEY_LOCK);
				break;
			case J_OPERATING:
				if (!Has(P_FREQ_HZ) && !FREQUENCY_IsReceivable(get32(b)))
					put32(b, gVfo->Frequency);   // never leave the block unusable
				if (Has(P_FREQ_HZ))
					put32(b, gPVal[P_FREQ_HZ] / 10u);
				Patch8(b + 4, P_POWER);
				Patch8(b + 5, P_BANDWIDTH);
				b[6] = gPStep;
				break;
			case J_TIMING:
				Patch8(b + 0, P_PTT_PRESS_MS);
				Patch8(b + 1, P_PTT_RELEASE_MS);
				Patch8(b + 2, P_PA_ENABLE_DELAY_MS);
				Patch8(b + 3, P_PA_BIAS_DELAY_MS);
				break;
			case J_V2_B:
				if (!v2Valid)
					memset(b, 0xFF, sizeof(b));
				if (gPSub) {
					put32(b, gPSubMask);
					put16(b + 4, gPSubHeartbeat);
					b[6] = gPSubOptions;
				}
				break;
			case J_V2_A:
				if (!v2Valid)
					memset(b, 0xFF, sizeof(b));
				b[0] = SETTINGS_V2_VERSION;
				if (Has(P_SERIAL_LOCK_MS))
					b[1] = (uint8_t)(gPVal[P_SERIAL_LOCK_MS] / 10u);
				Patch8(b + 2, P_BUSY_SOURCE);
				Patch8(b + 3, P_BUSY_HANG_MS);
				Patch16(b + 4, P_BUSY_RSSI_OPEN);
				Patch16(b + 6, P_BUSY_RSSI_CLOSE);
				break;
		}
	}

	EEPROM_WriteBuffer(addr, b);   // refuses 0x1E00 and up

done:
	if (!gJobs) {
		gPMask = 0;
		gPSub  = false;
		if (gSettingsBlockValid)
			gV2.valid = ReadByte(SETTINGS_V2_BLOCK) == SETTINGS_V2_VERSION;
	}
}

// ---------------------------------------------------------- commands --

uint8_t PARAMS_Set(uint8_t flags, const uint8_t *rec, uint16_t n, uint8_t *detail, uint8_t *out, uint16_t *outLen)
{
	uint32_t val[P_LAST + 1];
	uint8_t  raw[6] = { 0 };
	uint8_t  order[P_LAST];
	uint8_t  count = 0;
	uint32_t set   = 0;

	*detail = 0;
	for (uint16_t i = 0; i < n; ) {
		const uint8_t id   = rec[i++];
		const uint8_t size = PARAMS_Size(id);
		*detail = id;
		if (id == P_RETIRED_SQUELCH)
			return V2_UNSUPPORTED;   // there is no squelch
		if (!size || (set & BIT(id)))
			return V2_BAD_PARAM;
		if (i + size > n)
			return V2_BAD_LENGTH;
		uint32_t u = 0;
		for (uint8_t k = 0; k < size && k < 4; k++)
			u |= (uint32_t)rec[i + k] << (8 * k);
		if (id == P_BUSY_SQL_RAW)
			memcpy(raw, rec + i, 6);
		val[id] = u;
		i += size;
		set |= BIT(id);
		order[count++] = id;
	}

	for (uint8_t k = 0; k < count; k++) {
		*detail = order[k];
		if (!InRange(order[k], val[order[k]], raw))
			return V2_RANGE;
	}

	const uint32_t open  = (set & BIT(P_BUSY_RSSI_OPEN))  ? val[P_BUSY_RSSI_OPEN]  : gV2.BUSY_RSSI_OPEN;
	const uint32_t close = (set & BIT(P_BUSY_RSSI_CLOSE)) ? val[P_BUSY_RSSI_CLOSE] : gV2.BUSY_RSSI_CLOSE;
	if (close > open) {
		*detail = (set & BIT(P_BUSY_RSSI_CLOSE)) ? P_BUSY_RSSI_CLOSE : P_BUSY_RSSI_OPEN;
		return V2_RANGE;
	}

	const bool persist = (flags & SETP_PERSIST) && set;
	if (persist) {
		if (set & PARAMS_RAM_ONLY) {
			*detail = Lowest(set & PARAMS_RAM_ONLY);
			return V2_NOT_PERSISTABLE;
		}
		if (!gSettingsBlockValid) {
			*detail = Lowest(set);
			return V2_EEPROM;
		}
	}

	const uint32_t f10  = (set & BIT(P_FREQ_HZ)) ? val[P_FREQ_HZ] / 10u : gVfo->Frequency;
	const bool     txOk = TX_freq_check(f10) == 0;
	if ((flags & SETP_REQUIRE_TX) && !txOk) {
		*detail = P_FREQ_HZ;
		return V2_TX_BAND;
	}

	uint32_t changed = 0;
	for (uint8_t k = 0; k < count; k++) {
		const uint8_t id = order[k];
		if (id == P_BUSY_SQL_RAW) {
			uint8_t now[6];
			SqlRaw(gVfo, now);
			if (!gSqlRawActive || memcmp(now, raw, 6) != 0)
				changed |= BIT(id);
		}
		else if (val[id] != Live(id))
			changed |= BIT(id);
	}
	// at critical battery the receiver stays off: nothing that sets it up
	const uint32_t rx = PARAMS_RETUNE | BIT(P_RX_GAIN) | BIT(P_RX_DAC_GAIN) | BIT(P_AFC);
	if (gReducedService && (changed & rx) && !(flags & SETP_DRY_RUN)) {
		*detail = Lowest(changed & rx);
		return V2_STATE;
	}
	*detail = 0;

	out[0] = (txOk ? SETR_TX_ALLOWED : 0) | (persist ? SETR_PERSIST : 0) | ((changed & PARAMS_RETUNE) ? SETR_RETUNED : 0);

	if (!(flags & SETP_DRY_RUN)) {
		Apply(set, val, raw, changed);
		if (persist)
			QueuePersist(set);
		PARAMS_Changed(PSRC_SERIAL);
	}

	uint16_t len = 1;
	for (uint8_t k = 0; k < count; k++) {
		const uint8_t id = order[k];
		out[len++] = id;
		if (flags & SETP_DRY_RUN) {
			if (id == P_BUSY_SQL_RAW)
				memcpy(out + len, raw, 6);
			else
				for (uint8_t b = 0; b < kSize[id]; b++)
					out[len + b] = (uint8_t)(val[id] >> (8 * b));
			len += kSize[id];
		}
		else
			len += PARAMS_Get(id, false, out + len);
	}
	*outLen = len;
	return V2_OK;
}

uint8_t PARAMS_Save(uint8_t op, uint32_t *mask)
{
	uint32_t m = 0;
	*mask = 0;

	if (op == 0) {
		// SAVE: every persistable parameter whose live value is not stored
		if (!gSettingsBlockValid)
			return V2_EEPROM;
		for (uint8_t id = 1; id <= P_LAST; id++)
			if (!(PARAMS_RAM_ONLY & BIT(id)) && Live(id) != Stored(id))
				m |= BIT(id);
		if (m)
			QueuePersist(m);
		*mask = m;
		return V2_OK;
	}

	if (op != 1)
		return V2_RANGE;

	// REVERT: reload as at power-on, RAM-only settings dropped
	if (gJobs || gReducedService)
		return V2_STATE;
	uint32_t before[P_LAST + 1];
	Snapshot(before);
	SETTINGS_InitEEPROM();
	SETTINGS_LoadCalibration();
	gSqlRawActive = false;
	gAgcFix       = 0xFF;
	gAfcOn        = true;
	RADIO_ConfigureChannel();
	MON_Retune();
	RADIO_SetupRegisters(true);
	BK4819_SetRegValue(afcDisableRegSpec, 0);
	for (uint8_t id = 1; id <= P_LAST; id++)
		if (Live(id) != before[id])
			m |= BIT(id);
	*mask = m;
	gUpdateDisplay = true;
	gUpdateStatus  = true;
	PARAMS_Changed(PSRC_SERIAL);
	return V2_OK;
}
