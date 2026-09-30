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

// Host harness for the serial protocol: the real app/uart.c, app/v2.c,
// app/events.c, app/params.c, app/monitor.c, outq.c, settings.c, radio.c,
// frequencies.c, misc.c and driver/eeprom.c run against an emulated 24C64
// (behind the real EEPROM driver and its calibration guard), a BK4819
// register file, the UART DMA ring and a byte sink for the UART output.

#ifndef HOST_HARNESS_H
#define HOST_HARNESS_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "ptt.h"

extern int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

extern uint8_t  eeprom[0x2000];
extern int      eeprom_writes_in_cal;
extern int      eeprom_reads;       // EEPROM read transactions
extern uint16_t regs[128];
extern int      reg_writes[128];
extern bool     pa_enabled;
extern int      resets;
extern int      pa_off_before_reset;
extern PttState_t host_ptt;
extern uint8_t  host_arb_state;
extern int      serial_frames;
extern uint16_t host_us;
extern bool     tx_ready;           // the UART FIFO takes bytes
extern bool     audio_path_on;      // the speaker amplifier (K1 audio out)

#define OUT_MAX 16384
extern uint8_t  last_req[320];      // the last frame host_send built
extern unsigned last_req_len;
extern uint8_t  out[OUT_MAX];
extern unsigned out_len;

typedef struct {
	uint16_t id;
	uint16_t len;          // payload length (id + body_len + body)
	uint16_t body_len;
	uint8_t  body[256];
	bool     crc_real;     // the CRC field held the real CRC
	bool     crc_ff;       // the CRC field held FF FF (legacy)
	unsigned offset;       // where the frame started in out[]
	unsigned size;         // bytes on the wire
} Frame_t;

void     host_boot(void);                 // blank EEPROM defaults, power-on state
void     host_boot_keep_eeprom(void);
void     host_set_obfuscated(bool on);    // what the harness expects the radio's mode to be
bool     host_obfuscated(void);
unsigned host_build(uint8_t *f, uint16_t id, const void *body, uint16_t n, bool obf);
void     host_rx(const uint8_t *bytes, unsigned n);     // bytes arrive in the DMA ring
void     host_send(uint16_t id, const void *body, uint16_t n);   // frame in the current mode, then poll
void     host_send_mode(uint16_t id, const void *body, uint16_t n, bool obf);
void     host_poll(void);
int      host_frames(Frame_t *f, int max);              // parse out[] in the current mode
void     host_clear_out(void);
void     host_advance(uint32_t ms);                     // g_ms moves, SysTick counters too

// the tag-carrying v2 request helper: returns the reply frame, or NULL
const Frame_t *v2(uint16_t id, uint8_t tag, const uint8_t *req, uint16_t n);
extern Frame_t v2_frames[64];
extern int     v2_nframes;

#endif
