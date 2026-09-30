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
#include "app/uart.h"
#include "app/v2.h"
#include "bsp/dp32g030/dma.h"
#include "driver/bk4819.h"
#include "driver/eeprom.h"
#include "functions.h"
#include "helper/battery.h"
#include "misc.h"
#include "outq.h"
#include "radio.h"
#include "settings.h"

#include "harness.h"

void UART_HostReset(void);
#include "uart_shim.h"

int failures;

// ---------------------------------------------------------------- 24C64 --

uint8_t eeprom[0x2000];
int     eeprom_writes_in_cal;
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
			const uint16_t a = i2c_addr;
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

// ------------------------------------------------------------- BK4819 --

uint16_t regs[128];
int      reg_writes[128];
bool     pa_enabled;
int      resets;
int      pa_off_before_reset;

uint16_t BK4819_ReadRegister(BK4819_REGISTER_t r) { return regs[r & 127]; }
void BK4819_WriteRegister(BK4819_REGISTER_t r, uint16_t v) { regs[r & 127] = v; reg_writes[r & 127]++; }
void BK4819_SetRegValue(RegisterSpec s, uint16_t v)
{
	uint16_t reg = BK4819_ReadRegister(s.num);
	reg &= ~(s.mask << s.offset);
	BK4819_WriteRegister(s.num, reg | (v << s.offset));
}
void BK4819_SetAGC(bool enable) { if (enable) regs[0x7E] &= ~0x8000u; }
void BK4819_SetFilterBandwidth(const bool narrow) { regs[0x43] = narrow ? 0x7908 : 0x46A8; }
void BK4819_SetupPowerAmplifier(const uint8_t bias, const uint32_t f) { (void)f; regs[0x36] = (uint16_t)bias << 8; }
void BK4819_SetFrequency(uint32_t f) { regs[0x38] = f & 0xFFFF; regs[0x39] = f >> 16; }
void BK4819_SetupSquelch(uint8_t a, uint8_t b, uint8_t c, uint8_t d, uint8_t e, uint8_t f)
{
	regs[0x70] = 0;
	regs[0x78] = (a << 8) | b;
	regs[0x4F] = (d << 8) | c;
	regs[0x4D] = 0xA000 | e;
	regs[0x4E] = (1u << 14) | (6u << 9) | f;
	regs[0x47] = 0x6040;
}
void BK4819_PickRXFilterPathBasedOnFrequency(uint32_t f) { (void)f; }
void BK4819_ToggleGpioOut(BK4819_GPIO_PIN_t Pin, bool bSet) { if (Pin == BK4819_GPIO1_PIN29_PA_ENABLE) pa_enabled = bSet; }
void BK4819_PrepareDigitalTransmit(const uint8_t micGain, const uint16_t deviation) { regs[0x7D] = micGain; regs[0x40] = deviation; }
void BK4819_ExitSubAu(void) {}
void BK4819_SetAF(BK4819_AF_Type_t AF) { regs[0x47] = (6u << 12) | (AF << 8) | (1u << 6); }
void BACKLIGHT_TurnOn(void) {}
bool audio_path_on;
void AUDIO_AudioPathOnHost(void) { audio_path_on = true; }
void BOARD_ADC_GetBatteryInfo(uint16_t *v, uint16_t *c) { *v = 2000; *c = 0; }
void NVIC_SystemReset(void)
{
	resets++;
	if (!pa_enabled && regs[0x30] == 0 && regs[0x36] == 0)
		pa_off_before_reset++;
}

uint16_t CRC_Calculate(const void *pBuffer, uint16_t Size)
{	// CRC-16/XMODEM, as the DP32G030 CRC unit is set up
	const uint8_t *p = pBuffer;
	uint16_t crc = 0;
	for (unsigned i = 0; i < Size; i++) {
		crc ^= (uint16_t)p[i] << 8;
		for (int b = 0; b < 8; b++)
			crc = (crc & 0x8000) ? (crc << 1) ^ 0x1021 : crc << 1;
	}
	return crc;
}

// --------------------------------------------------- the rest of the app --

FUNCTION_Type_t gCurrentFunction;
void FUNCTION_Init(void) {}
void FUNCTION_Select(FUNCTION_Type_t f) { gCurrentFunction = f; }
bool FUNCTION_IsRx(void) { return gCurrentFunction != FUNCTION_TRANSMIT; }

uint16_t gBatteryCalibration[6];
uint8_t  gBatteryDisplayLevel = 5;
uint16_t gBatteryVoltageAverage = 780;

PttState_t host_ptt;
void PTT_GetState(PttState_t *s) { *s = host_ptt; }

uint8_t host_arb_state;
int     serial_frames;
void    APP_OnSerialFrame(void) { gSerialLockMs = gV2.SERIAL_LOCK_MS; serial_frames++; }
uint8_t APP_PttArbState(void) { return host_arb_state; }
void    APP_TxRefusedAtKeyUp(uint8_t reason) { (void)reason; }

uint16_t host_us;
void CLOCK_Now(uint32_t *ms, uint16_t *us) { *ms = g_ms; *us = host_us; }

const char Version[] = "PKTFW test";

volatile DMA_Channel_t host_dma_ch0;
uint8_t UART_DMA_Buffer[256];

uint8_t  out[OUT_MAX];
unsigned out_len;
bool     tx_ready = true;
bool UART_TxReady(void) { return tx_ready; }
void UART_TxPut(uint8_t b) { if (out_len < OUT_MAX) out[out_len++] = b; }
bool UART_TxEmpty(void) { return true; }
void UART_TxIrq(bool on) { (void)on; }
void UART_Send(const void *p, uint32_t n) { OUTQ_PutWait(p, (uint16_t)n); }

// ------------------------------------------------------------- helpers --

static const uint8_t kObf[16] = { 0x16, 0x6C, 0x14, 0xE6, 0x2E, 0x91, 0x0D, 0x40, 0x21, 0x35, 0xD5, 0x40, 0x13, 0x03, 0xE9, 0x80 };
static bool     obfuscated = true;
static unsigned ring_pos;

void host_set_obfuscated(bool on) { obfuscated = on; }
bool host_obfuscated(void) { return obfuscated; }

static void Blank(void)
{
	memset(eeprom, 0xFF, sizeof(eeprom));
	// a plausible factory calibration: squelch tables, TX power, battery
	for (unsigned i = 0x1E00; i < 0x1EC0; i++) eeprom[i] = 0x40;
	for (unsigned i = 0x1ED0; i < 0x1F40; i++) eeprom[i] = 0x50;
	const uint16_t bat[6] = { 1900, 2000, 2100, 2200, 2300, 2400 };
	memcpy(&eeprom[0x1F40], bat, sizeof(bat));
	eeprom[0x1F8E] = 45;                        // factory RX gain
}

void host_boot_keep_eeprom(void)
{
	memset(regs, 0, sizeof(regs));
	memset(reg_writes, 0, sizeof(reg_writes));
	regs[0x7E] = 0x37C0;
	regs[0x0C] = 0x0280;
	regs[0x67] = 105;
	regs[0x65] = 0x2F49;
	regs[0x63] = 0x5B;
	regs[0x64] = 0x0097;
	g_ms = 1000;
	host_us = 0;
	gSerialLockMs = 0;
	gReloadQuietMs = 0;
	gReloadSettingsAfterSerial = false;
	gCurrentFunction = FUNCTION_FOREGROUND;
	gReducedService = false;
	memset(&host_ptt, 0, sizeof(host_ptt));
	host_arb_state = 0;
	gSqlRawActive = false;
	gAgcFix = 0xFF;
	gAfcOn = true;
	gRegOverrideRamCount = 0;
	audio_path_on = false;
	OUTQ_Reset();
	tx_ready = true;
	memset(gCounters, 0, sizeof(gCounters));
	gEepromBlocksWritten = 0;
	eeprom_writes_in_cal = 0;

	SETTINGS_InitEEPROM();
	SETTINGS_LoadCalibration();
	RADIO_ConfigureChannel();
	RADIO_SetupRegisters(true);
	V2_Init();

	// the UART: power-on mode is obfuscated; the ring starts empty
	UART_HostReset();
	obfuscated = true;
	host_clear_out();
}

void host_boot(void)
{
	Blank();
	host_boot_keep_eeprom();
}

void host_clear_out(void) { out_len = 0; }

unsigned host_build(uint8_t *f, uint16_t id, const void *body, uint16_t n, bool obf)
{
	uint8_t p[300];
	p[0] = id & 0xFF; p[1] = id >> 8;
	p[2] = n & 0xFF;  p[3] = n >> 8;
	if (n) memcpy(p + 4, body, n);
	const uint16_t len = 4 + n;
	const uint16_t crc = CRC_Calculate(p, len);
	p[len] = crc & 0xFF; p[len + 1] = crc >> 8;
	if (obf)
		for (unsigned i = 0; i < len + 2u; i++) p[i] ^= kObf[i % 16];
	unsigned k = 0;
	f[k++] = 0xAB; f[k++] = 0xCD; f[k++] = len & 0xFF; f[k++] = len >> 8;
	memcpy(f + k, p, len + 2); k += len + 2;
	f[k++] = 0xDC; f[k++] = 0xBA;
	return k;
}

void host_rx(const uint8_t *bytes, unsigned n)
{
	for (unsigned i = 0; i < n; i++) {
		UART_DMA_Buffer[ring_pos] = bytes[i];
		ring_pos = (ring_pos + 1) % sizeof(UART_DMA_Buffer);
	}
	host_dma_ch0.ST = ring_pos;
}

void host_poll(void) { UART_Poll(); }

// A frame built in the given mode. As in the radio, a hello whose raw id
// bytes are 14 05 switches to plain mode and raw 02 69 back to obfuscated.
uint8_t  last_req[320];
unsigned last_req_len;

void host_send_mode(uint16_t id, const void *body, uint16_t n, bool obf)
{
	uint8_t *f = last_req;
	const unsigned k = host_build(f, id, body, n, obf);
	last_req_len = k;
	if (f[4] == 0x14 && f[5] == 0x05) obfuscated = false;
	if (f[4] == 0x02 && f[5] == 0x69) obfuscated = true;
	host_rx(f, k);
	host_poll();
}

void host_send(uint16_t id, const void *body, uint16_t n)
{
	host_send_mode(id, body, n, obfuscated);
}

int host_frames(Frame_t *fr, int max)
{
	int n = 0;
	unsigned i = 0;
	while (i + 8 <= out_len && n < max) {
		if (out[i] != 0xAB || out[i + 1] != 0xCD) { i++; continue; }
		const uint16_t len = out[i + 2] | (out[i + 3] << 8);
		if (i + len + 8 > out_len) break;
		if (out[i + 4 + len + 2] != 0xDC || out[i + 4 + len + 3] != 0xBA) { i++; continue; }
		uint8_t p[300];
		memcpy(p, out + i + 4, len + 2);
		if (obfuscated)
			for (unsigned k = 0; k < len + 2u; k++) p[k] ^= kObf[k % 16];
		Frame_t *f = &fr[n++];
		f->id       = p[0] | (p[1] << 8);
		f->body_len = p[2] | (p[3] << 8);
		f->len      = len;
		memcpy(f->body, p + 4, len >= 4 ? len - 4 : 0);
		const uint16_t crc = p[len] | (p[len + 1] << 8);
		f->crc_real = crc == CRC_Calculate(p, len);
		f->crc_ff   = crc == 0xFFFF;
		if (f->body_len != len - 4) {
			printf("radio frame 0x%04X: body_len %u, frame length %u\n", f->id, f->body_len, len);
			failures++;
			f->body_len = len - 4;
		}
		f->offset   = i;
		f->size     = len + 8;
		i += len + 8;
	}
	return n;
}

// Advance the clock as the SysTick handler would (the lock and the reload
// quiet time count down), running the main-loop protocol work each ms.
void host_advance(uint32_t ms)
{
	for (uint32_t i = 0; i < ms; i++) {
		g_ms++;
		if (gSerialLockMs) gSerialLockMs--;
		if (gReloadQuietMs) gReloadQuietMs--;
		OUTQ_Drain();
		MON_Service();
		EVT_Service(MON_Deferred(), g_ms);
		PARAMS_PersistService(gCurrentFunction != FUNCTION_TRANSMIT && !host_ptt.pressed && !host_ptt.candidate);
	}
}

Frame_t v2_frames[64];
int     v2_nframes;

const Frame_t *v2(uint16_t id, uint8_t tag, const uint8_t *req, uint16_t n)
{
	uint8_t b[256];
	b[0] = tag;
	if (n) memcpy(b + 1, req, n);
	host_clear_out();
	host_send(id, b, n + 1);
	v2_nframes = host_frames(v2_frames, 64);
	for (int i = 0; i < v2_nframes; i++)
		if (v2_frames[i].id == id + 0x80)
			return &v2_frames[i];
	return NULL;
}
