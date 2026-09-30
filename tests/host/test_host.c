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
#include "radio.h"
#include "settings.h"

// ---------------------------------------------------------------- 24C64 --

static uint8_t  eeprom[0x2000];
static int      eeprom_writes_in_cal;
static enum { I2C_IDLE, I2C_ADDR_HI, I2C_ADDR_LO, I2C_DATA, I2C_READ_MODE } i2c_state;
static uint16_t i2c_addr;

void I2C_Start(void) { i2c_state = I2C_IDLE; }
void I2C_Stop(void)  { i2c_state = I2C_IDLE; }
uint8_t I2C_Read(bool bFinal) { (void)bFinal; return eeprom[i2c_addr++ & 0x1FFF]; }

int I2C_Write(uint8_t Data)
{
	switch (i2c_state) {
		case I2C_IDLE:
			i2c_state = (Data == 0xA1) ? I2C_READ_MODE : I2C_ADDR_HI;
			break;
		case I2C_ADDR_HI: i2c_addr = Data << 8; i2c_state = I2C_ADDR_LO; break;
		case I2C_ADDR_LO: i2c_addr |= Data;     i2c_state = I2C_DATA;    break;
		case I2C_DATA: {
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
	for (unsigned i = 0; i < Size; i++) p[i] = eeprom[i2c_addr++ & 0x1FFF];
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

static void test_settings_defaults_and_roundtrip(void)
{
	memset(eeprom, 0xFF, sizeof(eeprom));
	eeprom[0x1F8E] = 50;                 // factory VOLUME_GAIN calibration
	SETTINGS_InitEEPROM();
	SETTINGS_LoadCalibration();

	CHECK(gEeprom.BUSY_LEVEL == 1);
	CHECK(gTxTimeoutSeconds[gEeprom.TX_TIMEOUT] == 30);
	CHECK(gEeprom.DEVIATION_WIDE == 0x856);
	CHECK(gEeprom.DEVIATION_NARROW == 0x756);
	CHECK(gEeprom.RX_GAIN == 50);
	CHECK(gEeprom.RX_DAC_GAIN == PKT_RX_DAC_GAIN_DEFAULT);
	CHECK(gEeprom.Vfo.Frequency == RADIO_DEFAULT_FREQUENCY);

	gEeprom.BUSY_LEVEL       = 7;
	gEeprom.TX_TIMEOUT       = 1;
	gEeprom.DEVIATION_WIDE   = 0x0A7F;
	gEeprom.DEVIATION_NARROW = 0x0123;
	gEeprom.RX_GAIN          = 63;
	gEeprom.RX_DAC_GAIN      = 0;
	SETTINGS_SaveSettings();
	CHECK(eeprom[SETTINGS_PKT_BLOCK] == SETTINGS_PKT_VERSION);
	CHECK(eeprom[SETTINGS_PKT_BLOCK + 3] == 0xFF);      // reserved: the retired mic gain

	memset(&gEeprom.BUSY_LEVEL, 0x55, 8);
	SETTINGS_InitEEPROM();
	SETTINGS_LoadCalibration();
	CHECK(gEeprom.BUSY_LEVEL == 7);
	CHECK(gTxTimeoutSeconds[gEeprom.TX_TIMEOUT] == 10);
	CHECK(gEeprom.DEVIATION_WIDE == 0x0A7F);
	CHECK(gEeprom.DEVIATION_NARROW == 0x0123);
	CHECK(gEeprom.RX_GAIN == 63);
	CHECK(gEeprom.RX_DAC_GAIN == 0);

	// without the layout version byte the block is ignored
	eeprom[SETTINGS_PKT_BLOCK] = 0x41;
	SETTINGS_InitEEPROM();
	CHECK(gEeprom.BUSY_LEVEL == 1);
	CHECK(gEeprom.DEVIATION_WIDE == PKT_DEVIATION_WIDE_DEFAULT);
	eeprom[SETTINGS_PKT_BLOCK] = SETTINGS_PKT_VERSION;

	// an old squelch 0 (open) is not a detector level: the default
	eeprom[SETTINGS_PKT_BLOCK + 1] = 0;
	SETTINGS_InitEEPROM();
	CHECK(gEeprom.BUSY_LEVEL == 1);
	eeprom[SETTINGS_PKT_BLOCK + 1] = 7;

	// out of range bytes fall back to the defaults
	eeprom[SETTINGS_PKT_BLOCK + 4] = 0x80;   // wide deviation 0xA80: past the clamp
	eeprom[SETTINGS_PKT_BLOCK + 5] = 0x0A;
	eeprom[SETTINGS_PKT_BLOCK + 7] = 0x10;   // narrow deviation 0x1023
	SETTINGS_InitEEPROM();
	CHECK(gEeprom.DEVIATION_WIDE == PKT_DEVIATION_WIDE_DEFAULT);
	CHECK(gEeprom.DEVIATION_NARROW == PKT_DEVIATION_NARROW_DEFAULT);
}

static void test_timing_block(void)
{
	memset(eeprom, 0xFF, sizeof(eeprom));
	SETTINGS_InitEEPROM();
	CHECK(gEeprom.PTT_PRESS_MS == 5 && gEeprom.PTT_RELEASE_MS == 5);
	CHECK(gEeprom.PA_ENABLE_DELAY_MS == 1 && gEeprom.PA_BIAS_DELAY_MS == 2);

	// values without a valid settings block are ignored
	const uint8_t t[8] = {2, 3, 1, 0, 0xFF, 0xFF, 0xFF, 0xFF};
	memcpy(&eeprom[SETTINGS_TIMING], t, 8);
	SETTINGS_InitEEPROM();
	CHECK(gEeprom.PTT_PRESS_MS == 5);

	eeprom[SETTINGS_PKT_BLOCK] = SETTINGS_PKT_VERSION;
	SETTINGS_InitEEPROM();
	CHECK(gEeprom.PTT_PRESS_MS == 2 && gEeprom.PTT_RELEASE_MS == 3);
	CHECK(gEeprom.PA_ENABLE_DELAY_MS == 1 && gEeprom.PA_BIAS_DELAY_MS == 0);

	// out of range: press 0, release 1, PA enable 0, bias 21 all default
	const uint8_t bad[8] = {0, 1, 0, 21, 0xFF, 0xFF, 0xFF, 0xFF};
	memcpy(&eeprom[SETTINGS_TIMING], bad, 8);
	SETTINGS_InitEEPROM();
	CHECK(gEeprom.PTT_PRESS_MS == 5 && gEeprom.PTT_RELEASE_MS == 5);
	CHECK(gEeprom.PA_ENABLE_DELAY_MS == 1 && gEeprom.PA_BIAS_DELAY_MS == 2);
	const uint8_t hi[8] = {40, 40, 20, 20, 0xFF, 0xFF, 0xFF, 0xFF};
	memcpy(&eeprom[SETTINGS_TIMING], hi, 8);
	SETTINGS_InitEEPROM();
	CHECK(gEeprom.PTT_PRESS_MS == 40 && gEeprom.PTT_RELEASE_MS == 40);
	CHECK(gEeprom.PA_ENABLE_DELAY_MS == 20 && gEeprom.PA_BIAS_DELAY_MS == 20);
	eeprom[SETTINGS_TIMING] = 41;
	SETTINGS_InitEEPROM();
	CHECK(gEeprom.PTT_PRESS_MS == 5);

	// the first menu save over foreign data blanks the timing and v2 blocks
	eeprom[SETTINGS_PKT_BLOCK] = 0x41;
	memset(&eeprom[SETTINGS_V2_BLOCK], 0x01, 16);
	SETTINGS_SaveSettings();
	CHECK(eeprom[SETTINGS_TIMING] == 0xFF && eeprom[SETTINGS_TIMING + 3] == 0xFF);
	for (unsigned i = 0; i < 16; i++)
		CHECK(eeprom[SETTINGS_V2_BLOCK + i] == 0xFF);
}

// Protocol v2 block (0x1D60): used only with a valid settings block and its
// own version byte; a byte out of range means the default.
static void test_v2_block(void)
{
	memset(eeprom, 0xFF, sizeof(eeprom));
	static const uint8_t b[16] = { 1, 5, 2, 100, 0x20, 0x01, 0x10, 0x01, 0x03, 0, 0, 0, 0xE8, 0x03, 1, 60 };
	memcpy(&eeprom[SETTINGS_V2_BLOCK], b, 16);
	SETTINGS_InitEEPROM();
	CHECK(!gV2.valid && gV2.SERIAL_LOCK_MS == 20 && gV2.DEFAULT_MASK == 0);   // no settings block
	eeprom[SETTINGS_PKT_BLOCK] = SETTINGS_PKT_VERSION;
	SETTINGS_InitEEPROM();
	CHECK(gV2.valid && gV2.SERIAL_LOCK_MS == 50 && gV2.BUSY_SOURCE == 2 && gV2.BUSY_HANG_MS == 100);
	CHECK(gV2.BUSY_RSSI_OPEN == 0x120 && gV2.BUSY_RSSI_CLOSE == 0x110);
	CHECK(gV2.DEFAULT_MASK == 3 && gV2.DEFAULT_HEARTBEAT_MS == 1000 && gV2.DEFAULT_OPTIONS == 1 && gV2.TONE_CAL == 60);
	static const uint8_t bad[16] = { 1, 151, 4, 251, 0x00, 0x02, 0x00, 0x02, 0xFF, 0xFF, 0xFF, 0xFF, 50, 0, 2, 128 };
	memcpy(&eeprom[SETTINGS_V2_BLOCK], bad, 16);
	SETTINGS_InitEEPROM();
	CHECK(gV2.SERIAL_LOCK_MS == 20 && gV2.BUSY_SOURCE == 1 && gV2.BUSY_HANG_MS == 20);
	CHECK(gV2.BUSY_RSSI_OPEN == 110 && gV2.BUSY_RSSI_CLOSE == 104);
	CHECK(gV2.DEFAULT_MASK == 0 && gV2.DEFAULT_HEARTBEAT_MS == 0 && gV2.DEFAULT_OPTIONS == 0 && gV2.TONE_CAL == 0);
	eeprom[SETTINGS_V2_BLOCK] = 2;                      // another layout: all defaults
	eeprom[SETTINGS_V2_BLOCK + 1] = 3;
	SETTINGS_InitEEPROM();
	CHECK(!gV2.valid && gV2.SERIAL_LOCK_MS == 20);
}

// The one operating channel (0x1D58), and the one-time import of the
// frequency in use from the old upstream layout.
static void test_operating_channel(void)
{
	// blank EEPROM: 144.800 MHz, low, wide, 12.5 kHz; nothing written
	memset(eeprom, 0xFF, sizeof(eeprom));
	SETTINGS_InitEEPROM();
	SETTINGS_LoadCalibration();
	RADIO_ConfigureChannel();
	CHECK(gVfo->Frequency == 14480000 && gVfo->Band == BAND3_137MHz);
	CHECK(gVfo->CHANNEL_BANDWIDTH == BANDWIDTH_WIDE && gVfo->OUTPUT_POWER == OUTPUT_POWER_LOW);
	CHECK(gVfo->StepFrequency == 1250);
	CHECK(eeprom[SETTINGS_OPERATING] == 0xFF);

	// an old band slot record in use (indices at 0x0E80): taken, read only
	const uint16_t base = 0x0C80 + BAND6_400MHz * 32;
	const uint32_t f = 43362500;
	memcpy(&eeprom[base], &f, 4);
	eeprom[base + 12] = (2u << 2) | (1u << 1);           // HIGH, narrow
	eeprom[base + 14] = STEP_6_25kHz;
	eeprom[0x0E80] = 200 + BAND6_400MHz;
	SETTINGS_InitEEPROM();
	RADIO_ConfigureChannel();
	CHECK(gVfo->Frequency == f && gVfo->OUTPUT_POWER == OUTPUT_POWER_HIGH);
	CHECK(gVfo->CHANNEL_BANDWIDTH == BANDWIDTH_NARROW && gVfo->StepFrequency == 625);
	CHECK(eeprom[SETTINGS_OPERATING] == 0xFF);           // no settings block: nothing written

	// an old memory channel in use, but not receivable: the default
	const uint32_t bad = 8000000;                        // 80 MHz, in no band
	memcpy(&eeprom[5 * 16], &bad, 4);
	eeprom[0x0E80] = 5;
	SETTINGS_InitEEPROM();
	CHECK(gVfo->Frequency == 14480000);
	const uint32_t ok = 14550000;
	memcpy(&eeprom[5 * 16], &ok, 4);
	SETTINGS_InitEEPROM();
	CHECK(gVfo->Frequency == ok);

	// with a valid settings block the import happens once and is written
	eeprom[SETTINGS_PKT_BLOCK] = SETTINGS_PKT_VERSION;
	SETTINGS_InitEEPROM();
	uint32_t fs;
	memcpy(&fs, &eeprom[SETTINGS_OPERATING], 4);
	CHECK(fs == ok && eeprom[SETTINGS_OPERATING + 4] == OUTPUT_POWER_LOW);
	memset(&eeprom[0x0C80], 0x00, 0x100);                // the old layout changes: ignored now
	memset(&eeprom[0x0000], 0x00, 0x100);
	eeprom[0x0E80] = 200;
	SETTINGS_InitEEPROM();
	CHECK(gVfo->Frequency == ok);

	// save and reload; out-of-range bytes mean the defaults
	gVfo->Frequency = 43350000;
	gVfo->OUTPUT_POWER = OUTPUT_POWER_MID;
	gVfo->CHANNEL_BANDWIDTH = BANDWIDTH_NARROW;
	gVfo->STEP_SETTING = STEP_25kHz;
	SETTINGS_SaveOperating();
	SETTINGS_InitEEPROM();
	RADIO_ConfigureChannel();
	CHECK(gVfo->Frequency == 43350000 && gVfo->OUTPUT_POWER == OUTPUT_POWER_MID);
	CHECK(gVfo->CHANNEL_BANDWIDTH == BANDWIDTH_NARROW && gVfo->StepFrequency == 2500);
	CHECK(eeprom[SETTINGS_OPERATING + 7] == 0xFF);
	eeprom[SETTINGS_OPERATING + 4] = 3;
	eeprom[SETTINGS_OPERATING + 5] = 2;
	eeprom[SETTINGS_OPERATING + 6] = STEP_N_ELEM;
	SETTINGS_InitEEPROM();
	RADIO_ConfigureChannel();
	CHECK(gVfo->OUTPUT_POWER == OUTPUT_POWER_LOW && gVfo->CHANNEL_BANDWIDTH == BANDWIDTH_WIDE && gVfo->StepFrequency == 1250);

	// a keypad save over foreign data writes the settings block first
	memset(eeprom, 0xFF, sizeof(eeprom));
	eeprom[SETTINGS_PKT_BLOCK] = 0x41;
	memset(&eeprom[SETTINGS_TIMING], 0x03, 8);
	SETTINGS_InitEEPROM();
	gVfo->Frequency = 14500000;
	SETTINGS_SaveOperating();
	CHECK(eeprom[SETTINGS_PKT_BLOCK] == SETTINGS_PKT_VERSION);
	CHECK(eeprom[SETTINGS_TIMING] == 0xFF);              // foreign data blanked
	memcpy(&fs, &eeprom[SETTINGS_OPERATING], 4);
	CHECK(fs == 14500000);

	// 350 to 400 MHz is receivable only when enabled (0x0F45)
	CHECK(FREQUENCY_IsReceivable(37000000));
	eeprom[0x0F45] = 0;
	memcpy(&eeprom[SETTINGS_OPERATING], &(uint32_t){ 37000000 }, 4);
	SETTINGS_InitEEPROM();
	CHECK(!FREQUENCY_IsReceivable(37000000));
	CHECK(gVfo->Frequency != 37000000);
	gSetting_350EN = true;
	CHECK(eeprom_writes_in_cal == 0);
}

static void test_frequency(void)
{
	// keypad: 144937 -> 144.937 -> rounded to 12.5 kHz step gives 144.9375
	CHECK(FREQUENCY_RoundToStep(14493700, 1250) == 14493750);
	CHECK(FREQUENCY_GetBand(14493750) == BAND3_137MHz);

	gSetting_F_LOCK = F_LOCK_GB;
	CHECK(TX_freq_check(14493750) == 0);
	CHECK(TX_freq_check(13700000) != 0);
	CHECK(TX_freq_check(43350000) == 0);
	gSetting_F_LOCK = F_LOCK_DEF;
	CHECK(TX_freq_check(14480000) == 0);
}

static void test_tx_rx_registers(void)
{
	memset(eeprom, 0xFF, sizeof(eeprom));
	memset(regs, 0, sizeof(regs));
	eeprom[SETTINGS_PKT_BLOCK]     = SETTINGS_PKT_VERSION;
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
// Entries an older build left there are ignored and never written; the RAM
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
	eeprom[SETTINGS_PKT_BLOCK] = SETTINGS_PKT_VERSION;
	put_old_entry(0, REG_OVERRIDE_TX, 0x2B, 0xFFF8, 0x0001);
	put_old_entry(1, REG_OVERRIDE_RX, 0x47, 0xF0FF, 0x0400);
	uint8_t before[0x40];
	memcpy(before, &eeprom[0x1D10], sizeof(before));
	SETTINGS_InitEEPROM();
	SETTINGS_LoadCalibration();
	RADIO_ConfigureChannel();
	gRegOverrideRamCount = 0;

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

	// saves never touch the reserved area, the first over foreign data included
	SETTINGS_SaveSettings();
	eeprom[SETTINGS_PKT_BLOCK] = 0x41;
	SETTINGS_SaveSettings();
	CHECK(memcmp(before, &eeprom[0x1D10], sizeof(before)) == 0);
}

int main(void)
{
	test_reg_overrides();
	test_eeprom_guard();
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
