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

// Host-side tests of the pure logic: the EEPROM write guard (through the
// real driver/eeprom.c on an emulated 24C64), settings load and save,
// the operating channel, frequency rounding and TX limits.
//
// Build and run: tests/host/run.sh (plain gcc, no hardware).

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "driver/bk4819.h"
#include "driver/eeprom.h"
#include "driver/i2c.h"
#include "frequencies.h"
#include "functions.h"
#include "helper/battery.h"
#include "misc.h"
#include "packet.h"
#include "pttarb.h"
#include "radio.h"
#include "settings.h"

// ---------------------------------------------------------------- 24C64 --

static uint8_t  eeprom[0x2000];
static int      eeprom_writes_in_cal;
static enum { I2C_IDLE, I2C_ADDR_HI, I2C_ADDR_LO, I2C_DATA, I2C_READ_MODE } i2c_state;
static uint16_t i2c_addr;
static unsigned i2c_data_bytes;       // in this write transaction
static bool     was_read[0x2000];     // every address read since the last forget_io()
static uint16_t write_log[256];       // start address of every block written
static unsigned write_log_n;

static void forget_io(void)
{
	memset(was_read, 0, sizeof(was_read));
	write_log_n = 0;
}

void I2C_Start(void) { i2c_state = I2C_IDLE; }
void I2C_Stop(void)  { i2c_state = I2C_IDLE; }
uint8_t I2C_Read(bool bFinal) { (void)bFinal; was_read[i2c_addr & 0x1FFF] = true; return eeprom[i2c_addr++ & 0x1FFF]; }

int I2C_Write(uint8_t Data)
{
	switch (i2c_state) {
		case I2C_IDLE:
			i2c_state = (Data == 0xA1) ? I2C_READ_MODE : I2C_ADDR_HI;
			break;
		case I2C_ADDR_HI: i2c_addr = Data << 8; i2c_state = I2C_ADDR_LO; break;
		case I2C_ADDR_LO: i2c_addr |= Data;     i2c_state = I2C_DATA; i2c_data_bytes = 0; break;
		case I2C_DATA: {
			if (i2c_data_bytes++ == 0 && write_log_n < 256)
				write_log[write_log_n++] = i2c_addr;
			// 32-byte pages: the address wraps inside the page, as on the chip
			const uint16_t a = (i2c_addr & ~31u) | ((i2c_addr + 0) & 31u);
			if (a >= 0x1E00) eeprom_writes_in_cal++;
			eeprom[a & 0x1FFF] = Data;
			i2c_addr = (i2c_addr & ~31u) | ((i2c_addr + 1) & 31u);
			break;
		}
		default: break;
	}
	return 0;
}

int I2C_ReadBuffer(void *pBuffer, uint8_t Size)
{
	uint8_t *p = pBuffer;
	for (unsigned i = 0; i < Size; i++) {
		was_read[i2c_addr & 0x1FFF] = true;
		p[i] = eeprom[i2c_addr++ & 0x1FFF];
	}
	return 0;
}

int I2C_WriteBuffer(const void *pBuffer, uint8_t Size)
{
	const uint8_t *p = pBuffer;
	for (unsigned i = 0; i < Size; i++) I2C_Write(p[i]);
	return 0;
}

void SYSTEM_DelayMs(uint32_t Delay) { (void)Delay; }

// ------------------------------------------------------------ BK4819 stub --

static uint16_t regs[128];
uint16_t BK4819_ReadRegister(BK4819_REGISTER_t r) { return (r == BK4819_REG_0C) ? 0 : regs[r & 127]; }
void BK4819_WriteRegister(BK4819_REGISTER_t r, uint16_t v) { regs[r & 127] = v; }
void BK4819_SetAGC(bool enable) { if (enable) regs[0x7E] &= ~PKT_REG_7E_AGC_FIX; }
void BK4819_SetFilterBandwidth(const bool narrow) { (void)narrow; }
void BK4819_SetupPowerAmplifier(const uint8_t bias, const uint32_t frequency) { (void)bias; (void)frequency; }
void BK4819_SetFrequency(uint32_t f) { regs[0x38] = f & 0xFFFF; regs[0x39] = f >> 16; }
void BK4819_SetupSquelch(uint8_t a, uint8_t b, uint8_t c, uint8_t d, uint8_t e, uint8_t f) { (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; }
void BK4819_PickRXFilterPathBasedOnFrequency(uint32_t f) { (void)f; }
void BK4819_ToggleGpioOut(BK4819_GPIO_PIN_t Pin, bool bSet) { (void)Pin; (void)bSet; }
static uint16_t tx_dev;
void BK4819_PrepareDigitalTransmit(const uint16_t deviation) { tx_dev = deviation; regs[0x7E] |= PKT_REG_7E_AGC_FIX; }
void BK4819_ExitSubAu(void) {}

// ------------------------------------------------------- other app stubs --

FUNCTION_Type_t gCurrentFunction;
void FUNCTION_Init(void) {}
void FUNCTION_Select(FUNCTION_Type_t f) { gCurrentFunction = f; }
uint16_t gBatteryCalibration[6];
uint8_t  gBatteryDisplayLevel = 5;
static bool amp_on;
void MON_AfterRxSetup(void) { amp_on = true; }
void BK4819_SetAF(BK4819_AF_Type_t AF) { regs[0x47] = (6u << 12) | (AF << 8) | (1u << 6); }
void BK4819_SetRegValue(RegisterSpec s, uint16_t v) { regs[s.num] = (regs[s.num] & ~(s.mask << s.offset)) | (v << s.offset); }
void APP_TxRefusedAtKeyUp(uint8_t reason) { (void)reason; }

// ----------------------------------------------------------------- tests --

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

static void test_eeprom_guard(void)
{
	uint8_t data[8] = {1, 2, 3, 4, 5, 6, 7, 8};
	memset(eeprom, 0xFF, sizeof(eeprom));
	eeprom_writes_in_cal = 0;

	CHECK(EEPROM_IsWritable(0x0000));
	CHECK(EEPROM_IsWritable(0x1DF8));
	CHECK(!EEPROM_IsWritable(0x1E00));
	CHECK(!EEPROM_IsWritable(0x1FF0));
	CHECK(!EEPROM_IsWritable(0x1FF8));
	CHECK(!EEPROM_IsWritable(0x0004));   // unaligned
	CHECK(!EEPROM_IsWritable(0xFFF8));

	for (uint32_t a = 0x1E00; a < 0x2000; a += 8)
		EEPROM_WriteBuffer(a, data);
	EEPROM_WriteBuffer(0x1DFC, data);    // unaligned, would reach 0x1E03
	CHECK(eeprom_writes_in_cal == 0);

	EEPROM_WriteBuffer(0x1DF8, data);
	uint8_t back[8];
	EEPROM_ReadBuffer(0x1DF8, back, 8);
	CHECK(memcmp(back, data, 8) == 0);
	CHECK(eeprom[0x1E00] == 0xFF);
}

static uint32_t get_u32_le(const uint8_t *p)
{
	return p[0] | (p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// A signed family (the magic and the layout at 0x1D10) with everything else
// in it blank, as if a host had written only the signature.
static void sign(void)
{
	static const uint8_t sig[8] = { 'P', 'K', 'F', 'W', SETTINGS_LAYOUT, 0xFF, 0xFF, 0xFF };
	memcpy(&eeprom[SETTINGS_SIGNATURE], sig, 8);
}

static bool signed_family(void)
{
	return memcmp(&eeprom[SETTINGS_SIGNATURE], "PKFW\x02\xFF\xFF\xFF", 8) == 0;
}

static unsigned writes_at(uint16_t a)
{
	unsigned n = 0;
	for (unsigned i = 0; i < write_log_n; i++)
		n += write_log[i] == a;
	return n;
}

// A plausible factory calibration: squelch tables, TX power, battery,
// crystal, RX gain.
static void factory_calibration(void)
{
	for (unsigned i = 0x1E00; i < 0x1EC0; i++) eeprom[i] = 0x40;
	for (unsigned i = 0x1ED0; i < 0x1F40; i++) eeprom[i] = 0x50;
	const uint16_t bat[6] = { 1900, 2000, 2100, 2200, 2300, 2400 };
	memcpy(&eeprom[0x1F40], bat, sizeof(bat));
	memset(&eeprom[0x1F88], 0, 6);
	eeprom[0x1F8E] = 47;
}

// What a radio that ran the stock or egzumer firmware carries: channel
// records and indices pointing at one in use, S-meter levels a firmware
// would take, the CE TX plan with the 350 MHz band disabled, and DTMF
// contacts filling 0x1C00 up, the first byte at 0x1D00 being 0x01 (the
// v1.0.0 marker) by chance. Plus the factory calibration.
static void foreign_eeprom(void)
{
	for (unsigned i = 0; i < 0x1E00; i++)
		eeprom[i] = (uint8_t)(i * 7u + 3u);
	const uint32_t f = 43362500;                 // memory channel 0: 433.625 MHz, high, narrow
	memcpy(&eeprom[0x0000], &f, 4);
	eeprom[0x000C] = (2u << 2) | (1u << 1);
	memset(&eeprom[0x0E80], 0, 8);               // channel indices: memory channel 0
	eeprom[0x0EA0] = 0; eeprom[0x0EA1] = 140; eeprom[0x0EA2] = 80;   // S0, S9 a firmware would take
	static const uint8_t lock[8] = { 2, 1, 0, 1, 1, 0, 0xFF, 0xFF }; // F_LOCK_CE, 350TX, 200TX, 500TX, 350EN off
	memcpy(&eeprom[0x0F40], lock, 8);
	for (unsigned c = 0; c < 32; c++) {          // DTMF contacts: name and code, 16 bytes each
		memcpy(&eeprom[0x1C00 + c * 16], "CONTACT ", 8);
		memcpy(&eeprom[0x1C08 + c * 16], "12345678", 8);
	}
	eeprom[0x1D00] = 0x01;                       // a contact that starts with byte 0x01
	factory_calibration();
}

// Boot as main.c does, after forgetting earlier EEPROM traffic.
static void boot(void)
{
	forget_io();
	eeprom_writes_in_cal = 0;
	SETTINGS_InitEEPROM();
	SETTINGS_LoadCalibration();
	RADIO_ConfigureChannel();
}

static void check_defaults(void)
{
	CHECK(gVfo->Frequency == 14480000 && gVfo->Band == BAND3_137MHz);
	CHECK(gVfo->OUTPUT_POWER == OUTPUT_POWER_DEFAULT && OUTPUT_POWER_DEFAULT == OUTPUT_POWER_LOW);
	CHECK(gVfo->CHANNEL_BANDWIDTH == BANDWIDTH_WIDE && gVfo->StepFrequency == 1250);
	CHECK(gEeprom.BUSY_LEVEL == 1);
	CHECK(gTxTimeoutSeconds[gEeprom.TX_TIMEOUT] == 30);
	CHECK(gEeprom.DEVIATION_WIDE == PKT_DEVIATION_WIDE_DEFAULT && gEeprom.DEVIATION_NARROW == PKT_DEVIATION_NARROW_DEFAULT);
	CHECK(gEeprom.RX_GAIN == (eeprom[0x1F8E] <= PKT_RX_GAIN_MAX ? eeprom[0x1F8E] : PKT_RX_GAIN_FALLBACK));
	CHECK(gEeprom.RX_DAC_GAIN == PKT_RX_DAC_GAIN_DEFAULT);
	CHECK(gEeprom.BACKLIGHT_TIME == 3 && gEeprom.KEY_LOCK == 0);
	CHECK(gEeprom.PTT_PRESS_MS == 5 && gEeprom.PTT_RELEASE_MS == 5);
	CHECK(gEeprom.PA_ENABLE_DELAY_MS == 1 && gEeprom.PA_BIAS_DELAY_MS == 2);
	CHECK(gV2.valid && gV2.SERIAL_LOCK_MS == 20 && gV2.BUSY_SOURCE == BUSY_SOURCE_SQUELCH && gV2.BUSY_HANG_MS == 20);
	CHECK(gV2.BUSY_RSSI_OPEN == 110 && gV2.BUSY_RSSI_CLOSE == 104);
	CHECK(gV2.DEFAULT_MASK == 0 && gV2.DEFAULT_HEARTBEAT_MS == 0 && gV2.DEFAULT_OPTIONS == 0 && gV2.TONE_CAL == 0);
	CHECK(gSettingsBlockValid);
}

// Flashing over any other firmware is a factory reset: nothing it saved is
// read or kept, the defaults are written once with the signature, and only
// the factory calibration is read outside the family.
static void test_first_boot_over_foreign_eeprom(void)
{
	foreign_eeprom();
	uint8_t before[0x2000];
	memcpy(before, eeprom, sizeof(before));

	boot();
	check_defaults();
	CHECK(gEeprom.RX_GAIN == 47);                 // the factory calibration
	CHECK(signed_family());
	CHECK(writes_at(SETTINGS_SIGNATURE) == 1 && write_log[write_log_n - 1] == SETTINGS_SIGNATURE);   // signed last
	CHECK(write_log_n == 14);                     // 0x1D00 to 0x1D6F, every block once
	for (unsigned a = SETTINGS_FAMILY_START; a < SETTINGS_FAMILY_END; a += 8)
		CHECK(writes_at(a) == 1);
	for (unsigned a = SETTINGS_SIGNATURE + 8; a < SETTINGS_TIMING; a++)
		if (eeprom[a] != 0xFF) { CHECK(eeprom[a] == 0xFF); break; }   // reserved: blank
	CHECK(eeprom[SETTINGS_PKT_BLOCK] == 0xFF);    // no v1.0.0 marker
	CHECK(eeprom[SETTINGS_V2_BLOCK] == SETTINGS_V2_VERSION);

	// nothing outside the family written; nothing outside it and the
	// calibration read: not the channels, not 0x0E80, 0x0EA0 or 0x0F40
	for (unsigned a = 0; a < 0x2000; a++) {
		if (a >= SETTINGS_FAMILY_START && a < SETTINGS_FAMILY_END)
			continue;
		if (eeprom[a] != before[a]) { CHECK(eeprom[a] == before[a]); break; }
		if (was_read[a] && a < 0x1E00) { printf("read 0x%04X\n", a); CHECK(!was_read[a]); break; }
	}
	CHECK(!was_read[0x0E80] && !was_read[0x0EA0] && !was_read[0x0EA1] && !was_read[0x0F40] && !was_read[0x0F45]);
	CHECK(eeprom_writes_in_cal == 0);
	// the calibration is still read: squelch table (VHF, level 1), PA bias
	// (137 to 174 MHz, low), battery, crystal and RX gain
	CHECK(was_read[0x1E61] && was_read[0x1EF0] && was_read[0x1F40] && was_read[0x1F88] && was_read[0x1F8E]);

	// the fixed TX policy, whatever the CE plan at 0x0F40 said
	static const struct { uint32_t f; bool tx; } kTx[] = {
		{ 14690000, true }, { 43500000, true }, { 5000000, false },
		{ 20000000, false }, { 38000000, false }, { 50000000, false },
	};
	for (unsigned i = 0; i < ARRAY_SIZE(kTx); i++) {
		gVfo->Frequency = kTx[i].f;
		CHECK((RADIO_TxBar() == TXR_NONE) == kTx[i].tx);
		CHECK(kTx[i].tx || RADIO_TxBar() == TXR_TX_BAND);
	}
	CHECK(FREQUENCY_IsReceivable(38000000));      // 350EN off at 0x0F45: not read

	// a second power-on reads the family and writes nothing
	gVfo->Frequency = 14480000;
	boot();
	check_defaults();
	CHECK(write_log_n == 0);

	// user changes stick across power-ons; the signature is never rewritten
	gVfo->Frequency         = 43350000;
	gVfo->OUTPUT_POWER      = OUTPUT_POWER_HIGH;
	gVfo->CHANNEL_BANDWIDTH = BANDWIDTH_NARROW;
	gVfo->STEP_SETTING      = STEP_25kHz;
	forget_io();
	SETTINGS_SaveOperating();
	gEeprom.DEVIATION_WIDE = 0x0800;
	gEeprom.TX_TIMEOUT     = 5;
	SETTINGS_SaveSettings();
	CHECK(writes_at(SETTINGS_SIGNATURE) == 0 && writes_at(SETTINGS_OPERATING) == 1);
	boot();
	CHECK(write_log_n == 0);
	CHECK(gVfo->Frequency == 43350000 && gVfo->OUTPUT_POWER == OUTPUT_POWER_HIGH);
	CHECK(gVfo->CHANNEL_BANDWIDTH == BANDWIDTH_NARROW && gVfo->StepFrequency == 2500);
	CHECK(gEeprom.DEVIATION_WIDE == 0x0800 && gTxTimeoutSeconds[gEeprom.TX_TIMEOUT] == 60);
	CHECK(eeprom_writes_in_cal == 0);
}

// A radio that ran v1.0.0 of this firmware: its settings (marker byte 1 at
// 0x1D00, no signature) are foreign too. It resets once, then keeps what is
// set from then on.
static void test_v1_0_0_layout_is_foreign(void)
{
	memset(eeprom, 0xFF, sizeof(eeprom));
	factory_calibration();
	static const uint8_t v100[16] = { 1, 7, 5, 0xFF, 0x00, 0x09, 0x00, 0x08, 30, 9, 7, 0xFF, 1, 0xFF, 0xFF, 0xFF };
	memcpy(&eeprom[SETTINGS_PKT_BLOCK], v100, 16);
	static const uint8_t timing[8] = { 3, 3, 5, 10, 0xFF, 0xFF, 0xFF, 0xFF };
	memcpy(&eeprom[SETTINGS_TIMING], timing, 8);
	const uint32_t f = 43350000;
	memcpy(&eeprom[SETTINGS_OPERATING], &f, 4);
	eeprom[SETTINGS_OPERATING + 4] = OUTPUT_POWER_HIGH;
	static const uint8_t v2[16] = { 1, 5, 2, 100, 0x20, 0x01, 0x10, 0x01, 0x03, 0, 0, 0, 0xE8, 0x03, 1, 60 };
	memcpy(&eeprom[SETTINGS_V2_BLOCK], v2, 16);

	boot();
	check_defaults();
	CHECK(signed_family() && writes_at(SETTINGS_SIGNATURE) == 1);
	CHECK(get_u32_le(&eeprom[SETTINGS_OPERATING]) == 14480000);

	boot();
	check_defaults();
	CHECK(write_log_n == 0);
}

static void test_settings_defaults_and_roundtrip(void)
{
	memset(eeprom, 0xFF, sizeof(eeprom));
	eeprom[0x1F8E] = 50;                 // factory VOLUME_GAIN calibration
	boot();
	check_defaults();
	CHECK(gEeprom.RX_GAIN == 50);
	CHECK(signed_family());
	CHECK(eeprom[SETTINGS_PKT_BLOCK + 8] == 50);        // written explicitly

	gEeprom.BUSY_LEVEL       = 7;
	gEeprom.TX_TIMEOUT       = 1;
	gEeprom.DEVIATION_WIDE   = 0x0A7F;
	gEeprom.DEVIATION_NARROW = 0x0123;
	gEeprom.RX_GAIN          = 63;
	gEeprom.RX_DAC_GAIN      = 0;
	SETTINGS_SaveSettings();
	CHECK(eeprom[SETTINGS_PKT_BLOCK] == 0xFF);          // reserved: was the layout version
	CHECK(eeprom[SETTINGS_PKT_BLOCK + 3] == 0xFF);      // reserved: the retired mic gain
	CHECK(eeprom[SETTINGS_PKT_BLOCK + 11] == 0xFF);     // reserved: the retired battery type

	memset(&gEeprom.BUSY_LEVEL, 0x55, 8);
	boot();
	CHECK(gEeprom.BUSY_LEVEL == 7);
	CHECK(gTxTimeoutSeconds[gEeprom.TX_TIMEOUT] == 10);
	CHECK(gEeprom.DEVIATION_WIDE == 0x0A7F);
	CHECK(gEeprom.DEVIATION_NARROW == 0x0123);
	CHECK(gEeprom.RX_GAIN == 63);
	CHECK(gEeprom.RX_DAC_GAIN == 0);

	// a blank RX gain means the factory calibration
	eeprom[SETTINGS_PKT_BLOCK + 8] = 0xFF;
	boot();
	CHECK(gEeprom.RX_GAIN == 50);
	eeprom[SETTINGS_PKT_BLOCK + 8] = 63;

	// an old squelch 0 (open) is not a detector level: the default
	eeprom[SETTINGS_PKT_BLOCK + 1] = 0;
	boot();
	CHECK(gEeprom.BUSY_LEVEL == 1);
	eeprom[SETTINGS_PKT_BLOCK + 1] = 7;

	// out of range bytes fall back to the defaults
	eeprom[SETTINGS_PKT_BLOCK + 4] = 0x80;   // wide deviation 0xA80: past the clamp
	eeprom[SETTINGS_PKT_BLOCK + 5] = 0x0A;
	eeprom[SETTINGS_PKT_BLOCK + 7] = 0x10;   // narrow deviation 0x1023
	boot();
	CHECK(gEeprom.DEVIATION_WIDE == PKT_DEVIATION_WIDE_DEFAULT);
	CHECK(gEeprom.DEVIATION_NARROW == PKT_DEVIATION_NARROW_DEFAULT);
	CHECK(gEeprom.BUSY_LEVEL == 7);

	// any other signature or layout: the family is foreign, so defaults,
	// written over it, and signed again
	for (unsigned i = 0; i < 5; i++) {
		sign();
		eeprom[SETTINGS_PKT_BLOCK + 1] = 7;
		eeprom[SETTINGS_SIGNATURE + i] ^= (i == 4) ? 0x03 : 0x20;   // "pKFW" ... layout 1
		boot();
		CHECK(gEeprom.BUSY_LEVEL == 1 && signed_family());
		CHECK(eeprom[SETTINGS_PKT_BLOCK + 1] == 1);
	}
	CHECK(eeprom_writes_in_cal == 0);
}

static void test_timing_block(void)
{
	memset(eeprom, 0xFF, sizeof(eeprom));
	boot();
	CHECK(gEeprom.PTT_PRESS_MS == 5 && gEeprom.PTT_RELEASE_MS == 5);
	CHECK(gEeprom.PA_ENABLE_DELAY_MS == 1 && gEeprom.PA_BIAS_DELAY_MS == 2);
	CHECK(eeprom[SETTINGS_TIMING] == 5 && eeprom[SETTINGS_TIMING + 3] == 2);   // written explicitly

	const uint8_t t[8] = {2, 3, 1, 0, 0xFF, 0xFF, 0xFF, 0xFF};
	memcpy(&eeprom[SETTINGS_TIMING], t, 8);
	boot();
	CHECK(gEeprom.PTT_PRESS_MS == 2 && gEeprom.PTT_RELEASE_MS == 3);
	CHECK(gEeprom.PA_ENABLE_DELAY_MS == 1 && gEeprom.PA_BIAS_DELAY_MS == 0);

	// out of range: press 0, release 1, PA enable 0, bias 21 all default
	const uint8_t bad[8] = {0, 1, 0, 21, 0xFF, 0xFF, 0xFF, 0xFF};
	memcpy(&eeprom[SETTINGS_TIMING], bad, 8);
	boot();
	CHECK(gEeprom.PTT_PRESS_MS == 5 && gEeprom.PTT_RELEASE_MS == 5);
	CHECK(gEeprom.PA_ENABLE_DELAY_MS == 1 && gEeprom.PA_BIAS_DELAY_MS == 2);
	const uint8_t hi[8] = {40, 40, 20, 20, 0xFF, 0xFF, 0xFF, 0xFF};
	memcpy(&eeprom[SETTINGS_TIMING], hi, 8);
	boot();
	CHECK(gEeprom.PTT_PRESS_MS == 40 && gEeprom.PTT_RELEASE_MS == 40);
	CHECK(gEeprom.PA_ENABLE_DELAY_MS == 20 && gEeprom.PA_BIAS_DELAY_MS == 20);
	eeprom[SETTINGS_TIMING] = 41;
	boot();
	CHECK(gEeprom.PTT_PRESS_MS == 5);

	// values in an unsigned family are ignored and replaced by the defaults
	memcpy(&eeprom[SETTINGS_TIMING], t, 8);
	eeprom[SETTINGS_SIGNATURE] = 0xFF;
	boot();
	CHECK(gEeprom.PTT_PRESS_MS == 5 && eeprom[SETTINGS_TIMING] == 5);

	// a menu save that finds the family unsigned (a host overwrote the
	// signature since the load) writes all of it, from RAM, and signs it
	gEeprom.PTT_PRESS_MS = 9;
	gEeprom.BUSY_LEVEL   = 4;
	eeprom[SETTINGS_SIGNATURE] = 0x00;
	memset(&eeprom[SETTINGS_V2_BLOCK], 0x01, 16);
	forget_io();
	SETTINGS_SaveSettings();
	CHECK(signed_family() && write_log[write_log_n - 1] == SETTINGS_SIGNATURE);
	CHECK(eeprom[SETTINGS_TIMING] == 9 && eeprom[SETTINGS_PKT_BLOCK + 1] == 4);
	CHECK(eeprom[SETTINGS_V2_BLOCK] == SETTINGS_V2_VERSION && eeprom[SETTINGS_V2_BLOCK + 1] == 2);
	CHECK(eeprom[SETTINGS_V2_BLOCK + 15] == 0xFF);      // no tone calibration
}

// Protocol v2 block (0x1D60): used with the signature and its own version
// byte; a byte out of range means the default.
static void test_v2_block(void)
{
	memset(eeprom, 0xFF, sizeof(eeprom));
	static const uint8_t b[16] = { 1, 5, 2, 100, 0x20, 0x01, 0x10, 0x01, 0x03, 0, 0, 0, 0xE8, 0x03, 1, 60 };
	memcpy(&eeprom[SETTINGS_V2_BLOCK], b, 16);
	boot();
	CHECK(gV2.valid && gV2.SERIAL_LOCK_MS == 20 && gV2.DEFAULT_MASK == 0);   // not signed: defaults, written
	static const uint8_t def[16] = { 1, 2, 1, 20, 110, 0, 104, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF };
	CHECK(memcmp(&eeprom[SETTINGS_V2_BLOCK], def, 16) == 0);
	memcpy(&eeprom[SETTINGS_V2_BLOCK], b, 16);
	boot();
	CHECK(gV2.valid && gV2.SERIAL_LOCK_MS == 50 && gV2.BUSY_SOURCE == 2 && gV2.BUSY_HANG_MS == 100);
	CHECK(gV2.BUSY_RSSI_OPEN == 0x120 && gV2.BUSY_RSSI_CLOSE == 0x110);
	CHECK(gV2.DEFAULT_MASK == 3 && gV2.DEFAULT_HEARTBEAT_MS == 1000 && gV2.DEFAULT_OPTIONS == 1 && gV2.TONE_CAL == 60);
	static const uint8_t bad[16] = { 1, 151, 4, 251, 0x00, 0x02, 0x00, 0x02, 0xFF, 0xFF, 0xFF, 0xFF, 50, 0, 2, 128 };
	memcpy(&eeprom[SETTINGS_V2_BLOCK], bad, 16);
	boot();
	CHECK(gV2.SERIAL_LOCK_MS == 20 && gV2.BUSY_SOURCE == 1 && gV2.BUSY_HANG_MS == 20);
	CHECK(gV2.BUSY_RSSI_OPEN == 110 && gV2.BUSY_RSSI_CLOSE == 104);
	CHECK(gV2.DEFAULT_MASK == 0 && gV2.DEFAULT_HEARTBEAT_MS == 0 && gV2.DEFAULT_OPTIONS == 0 && gV2.TONE_CAL == 0);
	eeprom[SETTINGS_V2_BLOCK] = 2;                      // another layout: all defaults
	eeprom[SETTINGS_V2_BLOCK + 1] = 3;
	boot();
	CHECK(!gV2.valid && gV2.SERIAL_LOCK_MS == 20);
}

// The one operating channel (0x1D58).
static void test_operating_channel(void)
{
	// blank EEPROM: 144.800 MHz, low, wide, 12.5 kHz, written
	memset(eeprom, 0xFF, sizeof(eeprom));
	boot();
	check_defaults();
	uint32_t fs;
	memcpy(&fs, &eeprom[SETTINGS_OPERATING], 4);
	CHECK(fs == 14480000 && eeprom[SETTINGS_OPERATING + 4] == OUTPUT_POWER_LOW);
	CHECK(eeprom[SETTINGS_OPERATING + 5] == BANDWIDTH_WIDE && eeprom[SETTINGS_OPERATING + 6] == STEP_12_5kHz);

	// save and reload; out-of-range bytes mean the defaults
	gVfo->Frequency = 43350000;
	gVfo->OUTPUT_POWER = OUTPUT_POWER_MID;
	gVfo->CHANNEL_BANDWIDTH = BANDWIDTH_NARROW;
	gVfo->STEP_SETTING = STEP_25kHz;
	SETTINGS_SaveOperating();
	boot();
	CHECK(gVfo->Frequency == 43350000 && gVfo->OUTPUT_POWER == OUTPUT_POWER_MID);
	CHECK(gVfo->CHANNEL_BANDWIDTH == BANDWIDTH_NARROW && gVfo->StepFrequency == 2500);
	CHECK(eeprom[SETTINGS_OPERATING + 7] == 0xFF);
	eeprom[SETTINGS_OPERATING + 4] = 3;
	eeprom[SETTINGS_OPERATING + 5] = 2;
	eeprom[SETTINGS_OPERATING + 6] = STEP_N_ELEM;
	boot();
	CHECK(gVfo->OUTPUT_POWER == OUTPUT_POWER_LOW && gVfo->CHANNEL_BANDWIDTH == BANDWIDTH_WIDE && gVfo->StepFrequency == 1250);

	// a frequency that cannot be received: the default channel, not written
	memcpy(&eeprom[SETTINGS_OPERATING], &(uint32_t){ 8000000 }, 4);   // 80 MHz, in no band
	eeprom[SETTINGS_OPERATING + 4] = OUTPUT_POWER_HIGH;
	boot();
	CHECK(gVfo->Frequency == 14480000 && gVfo->OUTPUT_POWER == OUTPUT_POWER_LOW && write_log_n == 0);

	// a keypad save that finds the family unsigned writes all of it
	memset(eeprom, 0xFF, sizeof(eeprom));
	eeprom[SETTINGS_PKT_BLOCK] = 1;                     // the v1.0.0 marker is not a signature
	gVfo->Frequency = 14500000;
	SETTINGS_SaveOperating();
	CHECK(signed_family() && eeprom[SETTINGS_PKT_BLOCK] == 0xFF);
	memcpy(&fs, &eeprom[SETTINGS_OPERATING], 4);
	CHECK(fs == 14500000);

	// 350 to 400 MHz is always receivable: upstream's enable (0x0F45) is not read
	eeprom[0x0F45] = 0;
	memcpy(&eeprom[SETTINGS_OPERATING], &(uint32_t){ 37000000 }, 4);
	boot();
	CHECK(FREQUENCY_IsReceivable(37000000));
	CHECK(gVfo->Frequency == 37000000);
	CHECK(!was_read[0x0F45]);
	CHECK(eeprom_writes_in_cal == 0);
}

static void test_frequency(void)
{
	// keypad: 144937 -> 144.937 -> rounded to 12.5 kHz step gives 144.9375
	CHECK(FREQUENCY_RoundToStep(14493700, 1250) == 14493750);
	CHECK(FREQUENCY_GetBand(14493750) == BAND3_137MHz);

	// the fixed TX policy: 136 up to 174 MHz and 400 up to 470 MHz only
	CHECK(TX_freq_check(14493750) == 0);
	CHECK(TX_freq_check(14690000) == 0);
	CHECK(TX_freq_check(43500000) == 0);
	CHECK(TX_freq_check(13600000) == 0);
	CHECK(TX_freq_check(17399999) == 0);
	CHECK(TX_freq_check(40000000) == 0);
	CHECK(TX_freq_check(46999999) == 0);
	CHECK(TX_freq_check(13599999) != 0);
	CHECK(TX_freq_check(17400000) != 0);
	CHECK(TX_freq_check(39999999) != 0);
	CHECK(TX_freq_check(47000000) != 0);
	CHECK(TX_freq_check(5000000) != 0);     // 50 MHz
	CHECK(TX_freq_check(20000000) != 0);    // 200 MHz
	CHECK(TX_freq_check(38000000) != 0);    // 380 MHz
	CHECK(TX_freq_check(50000000) != 0);    // 500 MHz
	CHECK(TX_freq_check(0) != 0 && TX_freq_check(0xFFFFFFFFu) != 0);

	// receive: the whole band table, 350 to 400 MHz included; not the gaps
	CHECK(FREQUENCY_IsReceivable(5000000) && FREQUENCY_IsReceivable(38000000) && FREQUENCY_IsReceivable(60000000));
	CHECK(!FREQUENCY_IsReceivable(4999999) && !FREQUENCY_IsReceivable(9000000) && !FREQUENCY_IsReceivable(60000001));
}

static void test_tx_rx_registers(void)
{
	memset(eeprom, 0xFF, sizeof(eeprom));
	memset(regs, 0, sizeof(regs));
	sign();
	eeprom[SETTINGS_PKT_BLOCK + 3] = 7;       // a mic gain from an older build: ignored
	SETTINGS_InitEEPROM();
	SETTINGS_LoadCalibration();
	gEeprom.DEVIATION_WIDE = 0x600;
	gEeprom.DEVIATION_NARROW = 0x300;
	gEeprom.RX_GAIN = 40;
	gEeprom.RX_DAC_GAIN = 9;
	RADIO_ConfigureChannel();

	regs[0x31] = 0xFFFF;
	RADIO_SetupRegisters(true);
	CHECK(regs[0x7D] == (PKT_REG_7D_BASE | 31));    // mic gain fixed at the maximum
	CHECK(regs[0x48] == (PKT_REG_48_BASE | (40u << 4) | 9u));
	CHECK((regs[0x31] & PKT_REG_31_OFF_MASK) == 0);
	CHECK(regs[0x3F] == 0);                      // no squelch interrupts: a detector only
	CHECK(regs[0x47] == ((6u << 12) | (BK4819_AF_FM << 8) | (1u << 6)));   // audio always open
	CHECK(amp_on);

	gVfo->CHANNEL_BANDWIDTH = BANDWIDTH_WIDE;
	RADIO_SetTxParameters();
	CHECK(tx_dev == 0x600);
	CHECK(regs[0x7E] & PKT_REG_7E_AGC_FIX);
	RADIO_SetupRegisters(false);            // back to receive
	CHECK((regs[0x7E] & PKT_REG_7E_AGC_FIX) == 0);

	gVfo->CHANNEL_BANDWIDTH = BANDWIDTH_NARROW;
	RADIO_SetTxParameters();
	CHECK(tx_dev == 0x300);

	// key-down takes the chip out of TX straight away
	regs[0x30] = 0xC1FE;
	RADIO_SendEndOfTransmission();
	CHECK(regs[0x30] == 0);
}

// 0x1D10..0x1D4F held a stored register override table until 2026-09-30.
// Entries an older build left there are never applied, and the first
// power-on of this layout replaces them (signature, then blank); the RAM
// table (protocol v2 REG_OVERRIDE) still applies in its phase.
static void put_old_entry(unsigned i, uint8_t phase, uint8_t reg, uint16_t andMask, uint16_t orValue)
{
	uint8_t *e = &eeprom[0x1D10 + i * 8];
	e[0] = phase; e[1] = reg;
	e[2] = andMask & 0xFF; e[3] = andMask >> 8;
	e[4] = orValue & 0xFF; e[5] = orValue >> 8;
	e[6] = 0xFF; e[7] = 0xFF;
}

static void test_reg_overrides(void)
{
	memset(eeprom, 0xFF, sizeof(eeprom));
	eeprom[SETTINGS_PKT_BLOCK] = 1;              // a build of the v1.0.0 layout
	put_old_entry(0, REG_OVERRIDE_TX, 0x2B, 0xFFF8, 0x0001);
	put_old_entry(1, REG_OVERRIDE_RX, 0x47, 0xF0FF, 0x0400);
	SETTINGS_InitEEPROM();
	SETTINGS_LoadCalibration();
	RADIO_ConfigureChannel();
	gRegOverrideRamCount = 0;
	CHECK(signed_family());
	for (unsigned a = 0x1D18; a < 0x1D50; a++)
		if (eeprom[a] != 0xFF) { CHECK(eeprom[a] == 0xFF); break; }

	memset(regs, 0, sizeof(regs));
	regs[0x2B] = 0x0707;
	RADIO_SetTxParameters();
	CHECK(regs[0x2B] == 0x0707);                 // the old stored entry is not applied
	RADIO_SetupRegisters(false);
	CHECK((regs[0x47] & 0x0F00) != 0x0400);

	// the RAM table: its phase only, REG_40 clamped
	gRegOverridesRam[0] = (RegOverride_t){ REG_OVERRIDE_TX, 0x2B, 0xFFF8, 0x0001 };
	gRegOverridesRam[1] = (RegOverride_t){ REG_OVERRIDE_RX, 0x47, 0xF0FF, 0x0400 };
	gRegOverridesRam[2] = (RegOverride_t){ REG_OVERRIDE_TX, 0x40, 0xF000, 0x0FFF };
	gRegOverrideRamCount = 3;
	regs[0x2B] = 0x0707; regs[0x47] = 0x6040;
	RADIO_SetTxParameters();
	CHECK(regs[0x2B] == 0x0701);
	CHECK(regs[0x47] == 0x6040);                 // RX entry not applied on TX
	CHECK((regs[0x40] & 0x0FFF) == PKT_DEVIATION_MAX);
	RADIO_SetupRegisters(false);
	CHECK((regs[0x47] & 0x0F00) == 0x0400);
	gRegOverrideRamCount = 0;

	// saves never touch the reserved area
	memset(&eeprom[0x1D18], 0x5A, 8);
	SETTINGS_SaveSettings();
	SETTINGS_SaveOperating();
	CHECK(eeprom[0x1D18] == 0x5A && eeprom[0x1D1F] == 0x5A);
}

int main(void)
{
	test_reg_overrides();
	test_eeprom_guard();
	test_first_boot_over_foreign_eeprom();
	test_v1_0_0_layout_is_foreign();
	test_timing_block();
	test_settings_defaults_and_roundtrip();
	test_v2_block();
	test_operating_channel();
	test_frequency();
	test_tx_rx_registers();
	if (failures) {
		printf("%d check(s) failed\n", failures);
		return 1;
	}
	printf("all host tests passed\n");
	return 0;
}
