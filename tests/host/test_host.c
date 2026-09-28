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
// channel loading and saving, frequency rounding and TX limits.
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
static uint8_t  tx_mic;
static uint16_t tx_dev;
void BK4819_PrepareDigitalTransmit(const uint8_t micGain, const uint16_t deviation) { tx_mic = micGain; tx_dev = deviation; regs[0x7E] |= PKT_REG_7E_AGC_FIX; }
void BK4819_ExitSubAu(void) {}

// ------------------------------------------------------- other app stubs --

FUNCTION_Type_t gCurrentFunction;
void FUNCTION_Init(void) {}
void FUNCTION_Select(FUNCTION_Type_t f) { gCurrentFunction = f; }
uint16_t gBatteryCalibration[6];
uint8_t  gBatteryDisplayLevel = 5;

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

	CHECK(gEeprom.SQUELCH_LEVEL == 1);
	CHECK(gTxTimeoutSeconds[gEeprom.TX_TIMEOUT] == 30);
	CHECK(gEeprom.MIC_GAIN == PKT_MIC_GAIN_DEFAULT);
	CHECK(gEeprom.DEVIATION_WIDE == PKT_DEVIATION_DEFAULT);
	CHECK(gEeprom.DEVIATION_NARROW == PKT_DEVIATION_DEFAULT);
	CHECK(gEeprom.RX_GAIN == 50);
	CHECK(gEeprom.RX_DAC_GAIN == PKT_RX_DAC_GAIN_DEFAULT);
	CHECK(gEeprom.ScreenChannel == FREQ_CHANNEL_FIRST + BAND3_137MHz);

	gEeprom.SQUELCH_LEVEL    = 0;
	gEeprom.TX_TIMEOUT       = 1;
	gEeprom.MIC_GAIN         = 31;
	gEeprom.DEVIATION_WIDE   = 0x0FFF;
	gEeprom.DEVIATION_NARROW = 0x0123;
	gEeprom.RX_GAIN          = 63;
	gEeprom.RX_DAC_GAIN      = 0;
	SETTINGS_SaveSettings();
	CHECK(eeprom[SETTINGS_PKT_BLOCK] == SETTINGS_PKT_VERSION);

	memset(&gEeprom.SQUELCH_LEVEL, 0x55, 8);
	SETTINGS_InitEEPROM();
	SETTINGS_LoadCalibration();
	CHECK(gEeprom.SQUELCH_LEVEL == 0);
	CHECK(gTxTimeoutSeconds[gEeprom.TX_TIMEOUT] == 10);
	CHECK(gEeprom.MIC_GAIN == 31);
	CHECK(gEeprom.DEVIATION_WIDE == 0x0FFF);
	CHECK(gEeprom.DEVIATION_NARROW == 0x0123);
	CHECK(gEeprom.RX_GAIN == 63);
	CHECK(gEeprom.RX_DAC_GAIN == 0);

	// out of range bytes fall back to the defaults
	eeprom[SETTINGS_PKT_BLOCK + 3] = 32;
	eeprom[SETTINGS_PKT_BLOCK + 5] = 0x10;   // wide deviation 0x10FF
	SETTINGS_InitEEPROM();
	CHECK(gEeprom.MIC_GAIN == PKT_MIC_GAIN_DEFAULT);
	CHECK(gEeprom.DEVIATION_WIDE == PKT_DEVIATION_DEFAULT);
}

static void test_channel_load_save(void)
{
	memset(eeprom, 0xFF, sizeof(eeprom));
	SETTINGS_InitEEPROM();
	SETTINGS_LoadCalibration();

	RADIO_ConfigureChannel();
	CHECK(gVfo->CHANNEL_SAVE == FREQ_CHANNEL_FIRST + BAND3_137MHz);
	CHECK(gVfo->Frequency == 14480000);
	CHECK(gVfo->CHANNEL_BANDWIDTH == BANDWIDTH_WIDE);
	CHECK(gVfo->OUTPUT_POWER == OUTPUT_POWER_LOW);

	// a CHIRP-style record for the 2 m slot with tones and an offset set
	const uint16_t base = 0x0C80 + BAND3_137MHz * 32;
	const uint32_t f = 14493750;
	memcpy(&eeprom[base], &f, 4);
	eeprom[base + 4] = 0x11; eeprom[base + 5] = 0x22; eeprom[base + 6] = 0x33; eeprom[base + 7] = 0x44; // offset
	eeprom[base + 8]  = 0x05;                 // RX tone code
	eeprom[base + 10] = 0x11;                 // tone types
	eeprom[base + 11] = 0x01;                 // FM, offset +
	eeprom[base + 12] = (1u << 4) | (2u << 2) | (1u << 1) | 1u;   // BCL, HIGH, narrow, reverse
	eeprom[base + 14] = STEP_6_25kHz;
	RADIO_ConfigureChannel();
	CHECK(gVfo->Frequency == f);
	CHECK(gVfo->CHANNEL_BANDWIDTH == BANDWIDTH_NARROW);
	CHECK(gVfo->OUTPUT_POWER == OUTPUT_POWER_HIGH);
	CHECK(gVfo->StepFrequency == 625);

	// save changes only frequency, power, bandwidth and step
	gVfo->Frequency = 14480000;
	gVfo->OUTPUT_POWER = OUTPUT_POWER_MID;
	gVfo->CHANNEL_BANDWIDTH = BANDWIDTH_WIDE;
	SETTINGS_SaveChannel(gVfo);
	uint32_t f2; memcpy(&f2, &eeprom[base], 4);
	CHECK(f2 == 14480000);
	CHECK(eeprom[base + 4] == 0x11 && eeprom[base + 7] == 0x44);
	CHECK(eeprom[base + 8] == 0x05 && eeprom[base + 10] == 0x11 && eeprom[base + 11] == 0x01);
	CHECK(eeprom[base + 12] == ((1u << 4) | (1u << 2) | (0u << 1) | 1u));
	CHECK(eeprom[base + 14] == STEP_6_25kHz);

	// memory channel 5 valid only once its attribute byte names a band
	gEeprom.ScreenChannel = 4;
	const uint32_t f3 = 43362500;
	memcpy(&eeprom[4 * 16], &f3, 4);
	SETTINGS_InitEEPROM();
	gEeprom.ScreenChannel = 4;
	RADIO_ConfigureChannel();
	CHECK(IS_FREQ_CHANNEL(gVfo->CHANNEL_SAVE));   // no attribute: falls back
	eeprom[0x0D60 + 4] = BAND6_400MHz;
	SETTINGS_InitEEPROM();
	gEeprom.ScreenChannel = 4;
	RADIO_ConfigureChannel();
	CHECK(gVfo->CHANNEL_SAVE == 4);
	CHECK(gVfo->Frequency == f3);
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
	SETTINGS_InitEEPROM();
	SETTINGS_LoadCalibration();
	gEeprom.MIC_GAIN = 7;
	gEeprom.DEVIATION_WIDE = 0x600;
	gEeprom.DEVIATION_NARROW = 0x300;
	gEeprom.RX_GAIN = 40;
	gEeprom.RX_DAC_GAIN = 9;
	RADIO_ConfigureChannel();

	regs[0x31] = 0xFFFF;
	RADIO_SetupRegisters(true);
	CHECK(regs[0x7D] == (PKT_REG_7D_BASE | 7));
	CHECK(regs[0x48] == (PKT_REG_48_BASE | (40u << 4) | 9u));
	CHECK((regs[0x31] & PKT_REG_31_OFF_MASK) == 0);
	CHECK(regs[0x3F] == (BK4819_REG_3F_SQUELCH_FOUND | BK4819_REG_3F_SQUELCH_LOST));

	gVfo->CHANNEL_BANDWIDTH = BANDWIDTH_WIDE;
	RADIO_SetTxParameters();
	CHECK(tx_mic == 7 && tx_dev == 0x600);
	CHECK(regs[0x7E] & PKT_REG_7E_AGC_FIX);
	RADIO_SetupRegisters(false);            // back to receive
	CHECK((regs[0x7E] & PKT_REG_7E_AGC_FIX) == 0);

	gVfo->CHANNEL_BANDWIDTH = BANDWIDTH_NARROW;
	RADIO_SetTxParameters();
	CHECK(tx_dev == 0x300);
}

int main(void)
{
	test_eeprom_guard();
	test_settings_defaults_and_roundtrip();
	test_channel_load_save();
	test_frequency();
	test_tx_rx_registers();
	if (failures) {
		printf("%d check(s) failed\n", failures);
		return 1;
	}
	printf("all host tests passed\n");
	return 0;
}
