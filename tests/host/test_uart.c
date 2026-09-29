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

// Host tests of the legacy commands in app/uart.c, through the real
// frame parser, output queue and EEPROM driver (harness.h): frames are
// built as k5.py builds them, placed in the DMA ring, and the replies
// checked byte for byte. Also the parser's resynchronisation: truncated,
// garbled and back-to-back frames.

#include <string.h>

#include "app/events.h"
#include "app/uart.h"
#include "app/v2.h"
#include "driver/eeprom.h"
#include "misc.h"
#include "settings.h"

#include "harness.h"

static const uint32_t session = 0x12345678;
static Frame_t fr[64];
static int     nfr;

static void send(uint16_t id, const void *body, uint16_t n)
{
	host_clear_out();
	host_send(id, body, n);
	nfr = host_frames(fr, 64);
}

static uint16_t reply_id(void) { return nfr ? fr[0].id : 0; }

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

static void test_hello(void)
{
	host_boot();

	// obfuscated hello (raw 02 69): the reply is obfuscated, with the FF FF CRC
	send(0x0514, &session, 4);
	CHECK(nfr == 1 && reply_id() == 0x0515);
	CHECK(host_obfuscated());
	CHECK(fr[0].crc_ff && !fr[0].crc_real);
	CHECK(fr[0].body_len == 36 && fr[0].len == 40);
	CHECK(memcmp(fr[0].body, "PKTFW test", 10) == 0 && fr[0].body[10] == 0);
	CHECK(fr[0].body[16] == 0 && fr[0].body[17] == 0);          // no AES key, no lock screen
	// the v2 marker in the challenge field: "PKT2", 0x0200, zeros
	static const uint8_t marker[16] = { 0x50, 0x4B, 0x54, 0x32, 0x00, 0x02, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
	CHECK(memcmp(fr[0].body + 20, marker, 16) == 0);
	CHECK(gSerialLockMs == SERIAL_LOCK_DEFAULT_MS);

	// the same reply byte for byte in plain mode (raw 14 05)
	host_clear_out();
	host_send_mode(0x0514, &session, 4, false);
	nfr = host_frames(fr, 64);
	CHECK(!host_obfuscated());
	static const uint8_t plain[48] = {
		0xAB, 0xCD, 0x28, 0x00, 0x15, 0x05, 0x24, 0x00,
		'P', 'K', 'T', 'F', 'W', ' ', 't', 'e', 's', 't', 0, 0, 0, 0, 0, 0,
		0, 0, 0, 0, 0x50, 0x4B, 0x54, 0x32, 0x00, 0x02, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
		0xFF, 0xFF, 0xDC, 0xBA };
	CHECK(out_len == sizeof(plain) && memcmp(out, plain, sizeof(plain)) == 0);

	// 0x052F answers the same
	send(0x052F, &session, 4);
	CHECK(reply_id() == 0x0515 && fr[0].crc_ff);

	// a hello resets the live subscription to the stored default (none)
	gSub.mask = 0xFFFF;
	send(0x0514, &session, 4);
	CHECK(gSub.mask == 0);
}

static void test_eeprom_commands(void)
{
	host_boot();
	host_send_mode(0x0514, &session, 4, false);

	// reads: calibration may be read, 128 is the limit
	gSerialLockMs = 0;
	read_cmd(0x1E00, 128, session);
	CHECK(reply_id() == 0x051C && fr[0].body_len == 132);
	CHECK(fr[0].body[0] == 0x00 && fr[0].body[1] == 0x1E && fr[0].body[2] == 128);
	CHECK(memcmp(fr[0].body + 4, &eeprom[0x1E00], 128) == 0);
	CHECK(gSerialLockMs == SERIAL_LOCK_DEFAULT_MS);
	read_cmd(0x0000, 129, session);
	CHECK(nfr == 0);
	read_cmd(0x0000, 16, session + 1);        // wrong session
	CHECK(nfr == 0);

	// a normal write, applied after the quiet time
	uint8_t data[64];
	memset(data, 0x11, sizeof(data));
	gReloadSettingsAfterSerial = false;
	write_cmd(0x0100, 16, data, 16);
	CHECK(reply_id() == 0x051E && fr[0].body[0] == 0x00 && fr[0].body[1] == 0x01);
	CHECK(eeprom[0x0100] == 0x11 && eeprom[0x010F] == 0x11);
	CHECK(gReloadSettingsAfterSerial);
	CHECK(gReloadQuietMs == SERIAL_RELOAD_QUIET_MS);

	// anything touching calibration is refused whole, with no reply
	memset(data, 0x22, sizeof(data));
	write_cmd(0x1DF8, 16, data, 16);
	CHECK(nfr == 0);
	CHECK(eeprom[0x1DF8] == 0xFF);
	write_cmd(0x1E00, 8, data, 8);
	CHECK(nfr == 0);
	write_cmd(0x1FF0, 8, data, 8);
	CHECK(nfr == 0);
	CHECK(eeprom_writes_in_cal == 0);

	// unaligned, partial blocks, or more data claimed than sent: refused
	write_cmd(0x0004, 8, data, 8);
	CHECK(nfr == 0 && eeprom[0x0004] == 0xFF);
	write_cmd(0x0200, 12, data, 12);
	CHECK(nfr == 0 && eeprom[0x0200] == 0xFF);
	write_cmd(0x0300, 64, data, 8);
	CHECK(nfr == 0 && eeprom[0x0300] == 0xFF);
}

static void test_register_and_status_commands(void)
{
	host_boot();
	host_send_mode(0x0514, &session, 4, false);

	// every valid frame starts the lock, register and status commands too
	gSerialLockMs = 0;
	send(0x0527, NULL, 0);
	CHECK(reply_id() == 0x0528 && gSerialLockMs == SERIAL_LOCK_DEFAULT_MS);
	CHECK(fr[0].body_len == 4 && fr[0].body[0] == 105 && fr[0].body[1] == 0);
	CHECK(fr[0].body[2] == (0x2F49 & 0x7F) && fr[0].body[3] == 0x5B);

	send(0x0529, NULL, 0);
	CHECK(reply_id() == 0x052A && fr[0].body[0] == 0xD0 && fr[0].body[1] == 0x07);

	gSerialLockMs = 0;
	uint8_t r0[1] = { 0x30 };
	send(0x0601, r0, 1);
	CHECK(gSerialLockMs == SERIAL_LOCK_DEFAULT_MS);

	// register commands shorter than they should be are ignored
	regs[0x7D] = 0x1234;
	uint8_t w_short[2] = { 0x7D, 0x5A };
	send(0x0602, w_short, 2);
	CHECK(regs[0x7D] == 0x1234);
	send(0x0601, NULL, 0);
	CHECK(nfr == 0);

	// BK4819 register write and read (0x0602 has no reply)
	uint8_t w[3] = { 0x7D, 0x5A, 0xE9 };
	send(0x0602, w, 3);
	CHECK(nfr == 0 && regs[0x7D] == 0xE95A);
	uint8_t r[1] = { 0x7D };
	send(0x0601, r, 1);
	CHECK(reply_id() == 0x0601 && fr[0].body[0] == 0x7D && fr[0].body[1] == 0x5A && fr[0].body[2] == 0xE9);

	// unknown ids outside 0x50xx: no reply, but the lock still starts
	gSerialLockMs = 0;
	send(0x0700, NULL, 0);
	CHECK(nfr == 0 && gSerialLockMs == SERIAL_LOCK_DEFAULT_MS);

	// the lock length is the SERIAL_LOCK_MS setting
	gV2.SERIAL_LOCK_MS = 150;
	send(0x0527, NULL, 0);
	CHECK(gSerialLockMs == 150);
	gV2.SERIAL_LOCK_MS = SERIAL_LOCK_DEFAULT_MS;

	// reboot de-keys first
	regs[0x30] = 0xC1FE; regs[0x36] = 0xFFFF; pa_enabled = true;
	const int before = resets;
	send(0x05DD, NULL, 0);
	CHECK(resets == before + 1);
	CHECK(pa_off_before_reset >= 1);
}

// The parser resynchronises (M-bench, 29 September: a frame cut by PTT
// left the radio deaf to hellos for a while).
static void test_resync(void)
{
	uint8_t a[320], b[320], junk[8] = { 0xAB, 0x00, 0xAB, 0xCD, 0xFF, 0x7F, 0x12, 0x34 };
	host_boot();
	host_send_mode(0x0514, &session, 4, false);

	// back-to-back frames in one burst are all answered
	unsigned n = host_build(a, 0x0527, NULL, 0, false);
	memcpy(a + n, a, n);
	memcpy(a + 2 * n, a, n);
	host_clear_out();
	host_rx(a, 3 * n);
	host_poll();
	nfr = host_frames(fr, 64);
	CHECK(nfr == 3);

	// a truncated hello followed at once by a good one
	const uint32_t bad0 = gCounters[CNT_FRAMES_BAD];
	n = host_build(a, 0x0514, (const uint8_t *)&session, 4, false);
	unsigned m = host_build(b, 0x0527, NULL, 0, false);
	host_clear_out();
	host_rx(a, 7);                            // cut after 7 of 16 bytes
	host_rx(b, m);
	host_poll();
	nfr = host_frames(fr, 64);
	CHECK(nfr == 1 && reply_id() == 0x0528);

	// a truncated long frame (0x051D, 144 bytes claimed) swallows what
	// follows until the gap timeout, then the next frame is answered
	uint8_t w[136] = { 0x00, 0x01, 128, 0 };
	memcpy(w + 4, &session, 4);
	n = host_build(a, 0x051D, w, sizeof(w), false);
	host_clear_out();
	host_rx(a, 20);
	host_poll();
	host_rx(b, m);                            // within the gap: taken as the long frame's data
	host_poll();
	CHECK(host_frames(fr, 64) == 0);
	host_advance(UART_GAP_MS + 1);            // no new bytes for longer than the gap
	host_poll();
	host_rx(b, m);
	host_poll();
	nfr = host_frames(fr, 64);
	CHECK(nfr >= 1 && fr[nfr - 1].id == 0x0528);
	CHECK(gCounters[CNT_FRAMES_BAD] > bad0);

	// garbage and a false start before a good frame
	host_clear_out();
	host_rx(junk, sizeof(junk));
	host_rx(b, m);
	host_poll();
	host_advance(UART_GAP_MS + 1);
	host_poll();
	nfr = host_frames(fr, 64);
	CHECK(nfr == 1 && reply_id() == 0x0528);

	// a bad CRC is dropped without a reply and the next frame still answered
	n = host_build(a, 0x0527, NULL, 0, false);
	a[n - 3] ^= 0x01;
	host_clear_out();
	host_rx(a, n);
	host_rx(b, m);
	host_poll();
	nfr = host_frames(fr, 64);
	CHECK(nfr == 1 && reply_id() == 0x0528);

	// a bad footer drops only the AB: a frame right behind it is found
	n = host_build(a, 0x0527, NULL, 0, false);
	a[n - 1] = 0x00;
	host_clear_out();
	host_rx(a, n);
	host_rx(b, m);
	host_poll();
	nfr = host_frames(fr, 64);
	CHECK(nfr == 1 && reply_id() == 0x0528);

	// oversize length: dropped, resynchronised
	const uint8_t big[4] = { 0xAB, 0xCD, 0xFF, 0x00 };
	host_clear_out();
	host_rx(big, 4);
	host_rx(b, m);
	host_poll();
	nfr = host_frames(fr, 64);
	CHECK(nfr == 1 && reply_id() == 0x0528);
}

int main(void)
{
	test_hello();
	test_eeprom_commands();
	test_register_and_status_commands();
	test_resync();

	if (failures) {
		printf("%d check(s) failed\n", failures);
		return 1;
	}
	printf("all UART host tests passed\n");
	return 0;
}
