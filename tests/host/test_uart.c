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

// Host tests of app/uart.c: frames are built as k5.py builds them (plain
// mode), placed in the DMA ring, and the replies checked.

#include <stdio.h>
#include <string.h>

#include "app/uart.h"
#include "bsp/dp32g030/dma.h"
#include "driver/bk4819.h"
#include "driver/eeprom.h"
#include "misc.h"
#include "settings.h"

#include "uart_shim.h"

volatile DMA_Channel_t host_dma_ch0;
uint8_t UART_DMA_Buffer[256];
const char Version[] = "PKTFW test";

static uint8_t eeprom[0x2000];
static int     cal_writes;
void EEPROM_ReadBuffer(uint16_t a, void *p, uint8_t n) { for (unsigned i = 0; i < n; i++) ((uint8_t *)p)[i] = eeprom[(a + i) & 0x1FFF]; }
bool EEPROM_IsWritable(uint16_t a) { return a < EEPROM_CALIBRATION_START && (a & 7u) == 0; }
void EEPROM_WriteBuffer(uint16_t a, const void *p)
{
	if (!EEPROM_IsWritable(a)) return;
	if (a >= 0x1E00) cal_writes++;
	memcpy(&eeprom[a], p, 8);
}

static uint16_t regs[128];
uint16_t BK4819_ReadRegister(BK4819_REGISTER_t r) { return regs[r & 127]; }
void BK4819_WriteRegister(BK4819_REGISTER_t r, uint16_t v) { regs[r & 127] = v; }
void BOARD_ADC_GetBatteryInfo(uint16_t *v, uint16_t *c) { *v = 2000; *c = 0; }
static int resets;
static int pa_off_before_reset;
static bool pa_enabled = true;
void BK4819_SetupPowerAmplifier(const uint8_t bias, const uint32_t f) { (void)f; if (bias == 0) regs[0x36] = 0; }
void BK4819_ToggleGpioOut(BK4819_GPIO_PIN_t Pin, bool bSet) { if (Pin == BK4819_GPIO1_PIN29_PA_ENABLE) pa_enabled = bSet; }
void NVIC_SystemReset(void)
{
	resets++;
	if (!pa_enabled && regs[0x30] == 0 && regs[0x36] == 0)
		pa_off_before_reset++;
}

static uint8_t  tx[1024];
static unsigned tx_len;
void UART_Send(const void *p, uint32_t n) { memcpy(tx + tx_len, p, n); tx_len += n; }

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

static unsigned ring_pos;

// send one plain frame and run the command handler
static void send(uint16_t id, const uint8_t *body, uint16_t body_len)
{
	uint8_t payload[300];
	payload[0] = id & 0xFF; payload[1] = id >> 8;
	payload[2] = body_len & 0xFF; payload[3] = body_len >> 8;
	memcpy(payload + 4, body, body_len);
	const uint16_t len = 4 + body_len;
	const uint16_t crc = CRC_Calculate(payload, len);

	uint8_t frame[320];
	unsigned n = 0;
	frame[n++] = 0xAB; frame[n++] = 0xCD;
	frame[n++] = len & 0xFF; frame[n++] = len >> 8;
	memcpy(frame + n, payload, len); n += len;
	frame[n++] = crc & 0xFF; frame[n++] = crc >> 8;
	frame[n++] = 0xDC; frame[n++] = 0xBA;

	for (unsigned i = 0; i < n; i++) {
		UART_DMA_Buffer[ring_pos] = frame[i];
		ring_pos = (ring_pos + 1) % sizeof(UART_DMA_Buffer);
	}
	host_dma_ch0.ST = ring_pos;

	tx_len = 0;
	while (UART_IsCommandAvailable())
		UART_HandleCommand();
}

static uint16_t reply_id(void) { return tx_len >= 6 ? (tx[4] | (tx[5] << 8)) : 0; }

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

static const uint32_t session = 0x12345678;

static void read_cmd(uint16_t addr, uint8_t size, uint32_t ts)
{
	uint8_t b[8] = { addr & 0xFF, addr >> 8, size, 0 };
	memcpy(b + 4, &ts, 4);
	send(0x051B, b, 8);
}

static void write_cmd(uint16_t addr, uint8_t size, const uint8_t *data, uint16_t data_len)
{
	uint8_t b[200] = { addr & 0xFF, addr >> 8, size, 0 };
	memcpy(b + 4, &session, 4);
	memcpy(b + 8, data, data_len);
	send(0x051D, b, 8 + data_len);
}

int main(void)
{
	memset(eeprom, 0xFF, sizeof(eeprom));

	// hello, plain mode
	send(0x0514, (const uint8_t *)&session, 4);
	CHECK(reply_id() == 0x0515);
	CHECK(memcmp(&tx[8], "PKTFW test", 10) == 0);
	CHECK(gSerialConfigCountDown_500ms == SERIAL_PTT_LOCK_500ms);

	// reads: calibration may be read, 128 is the limit
	gSerialConfigCountDown_500ms = 0;
	read_cmd(0x1E00, 128, session);
	CHECK(reply_id() == 0x051C);
	CHECK(gSerialConfigCountDown_500ms == SERIAL_PTT_LOCK_500ms);
	read_cmd(0x0000, 129, session);
	CHECK(tx_len == 0);
	read_cmd(0x0000, 16, session + 1);        // wrong session
	CHECK(tx_len == 0);

	// a normal write, applied later
	uint8_t data[64];
	memset(data, 0x11, sizeof(data));
	gReloadSettingsAfterSerial = false;
	write_cmd(SETTINGS_PKT_BLOCK, 16, data, 16);
	CHECK(reply_id() == 0x051E);
	CHECK(eeprom[SETTINGS_PKT_BLOCK] == 0x11 && eeprom[SETTINGS_PKT_BLOCK + 15] == 0x11);
	CHECK(gReloadSettingsAfterSerial);

	// anything touching calibration is refused whole, with no reply
	memset(data, 0x22, sizeof(data));
	write_cmd(0x1DF8, 16, data, 16);
	CHECK(tx_len == 0);
	CHECK(eeprom[0x1DF8] == 0xFF);
	write_cmd(0x1E00, 8, data, 8);
	CHECK(tx_len == 0);
	write_cmd(0x1FF0, 8, data, 8);
	CHECK(tx_len == 0);
	CHECK(cal_writes == 0);

	// unaligned, partial blocks, or more data claimed than sent: refused
	write_cmd(0x0004, 8, data, 8);
	CHECK(tx_len == 0 && eeprom[0x0004] == 0xFF);
	write_cmd(0x0100, 12, data, 12);
	CHECK(tx_len == 0 && eeprom[0x0100] == 0xFF);
	write_cmd(0x0200, 64, data, 8);
	CHECK(tx_len == 0 && eeprom[0x0200] == 0xFF);

	// every valid frame refreshes the PTT lock, register and status commands too
	gSerialConfigCountDown_500ms = 0;
	send(0x0527, NULL, 0);
	CHECK(reply_id() == 0x0528 && gSerialConfigCountDown_500ms == SERIAL_PTT_LOCK_500ms);
	gSerialConfigCountDown_500ms = 0;
	uint8_t r0[1] = { 0x30 };
	send(0x0601, r0, 1);
	CHECK(gSerialConfigCountDown_500ms == SERIAL_PTT_LOCK_500ms);

	// register commands shorter than they should be are ignored
	regs[0x7D] = 0x1234;
	uint8_t w_short[2] = { 0x7D, 0x5A };
	send(0x0602, w_short, 2);
	CHECK(regs[0x7D] == 0x1234);
	send(0x0601, NULL, 0);
	CHECK(tx_len == 0);

	// BK4819 register write and read (0x0602 has no reply)
	uint8_t w[3] = { 0x7D, 0x5A, 0xE9 };
	send(0x0602, w, 3);
	CHECK(tx_len == 0 && regs[0x7D] == 0xE95A);
	uint8_t r[1] = { 0x7D };
	send(0x0601, r, 1);
	CHECK(reply_id() == 0x0601 && tx[8] == 0x7D && tx[9] == 0x5A && tx[10] == 0xE9);

	// reboot de-keys first
	regs[0x30] = 0xC1FE; regs[0x36] = 0xFFFF; pa_enabled = true;
	send(0x05DD, NULL, 0);
	CHECK(resets == 1);
	CHECK(pa_off_before_reset == 1);

	if (failures) {
		printf("%d check(s) failed\n", failures);
		return 1;
	}
	printf("all UART host tests passed\n");
	return 0;
}
