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

// What the radio reports on its own (protocol v2, 8): busy (carrier
// detect) and burst reports, the RSSI stream, heartbeats, transmission
// start, end and refusal, battery class changes; plus the level tone
// (LEVEL_TONE) and the expiry of RAM register overrides. All BK4819 access
// here is from the main loop.

#ifndef APP_MONITOR_H
#define APP_MONITOR_H

#include <stdbool.h>
#include <stdint.h>

enum { TXEND_RELEASE = 0, TXEND_TIMEOUT, TXEND_SERIAL, TXEND_OTHER };

#define CD_CAUSE_SQUELCH  0x01u
#define CD_CAUSE_RSSI     0x02u
#define CD_CAUSE_RETUNE   0x04u
#define CD_CAUSE_TX       0x08u

enum { TONE_END_ELAPSED = 0, TONE_END_STOPPED, TONE_END_PTT, TONE_END_RETUNE, TONE_END_REPLACED };

#define BURST_PERIOD_DEFAULT_MS  5u

void     MON_Init(void);
void     MON_Service(void);                 // every main-loop pass
bool     MON_Deferred(void);                // hold events back now
void     MON_SubscriptionChanged(void);
void     MON_ForceClose(uint8_t cause);     // busy closed by a key-up or a retune
void     MON_Retune(void);                  // before a receiver set-up that retunes
void     MON_KeyInfo(uint32_t tPress, uint16_t lockDelay, bool late, bool busyAtPress);
void     MON_TxStarted(void);               // RF ready
void     MON_TxEnded(uint8_t reason, uint32_t tRelease);   // receiver set up again
void     MON_TxRefused(uint32_t tPress, uint8_t reason, uint16_t detail);
void     MON_Slice500ms(void);
void     MON_AfterRxSetup(void);            // after the chip's receive set-up
bool     MON_Busy(void);
bool     MON_DetectorOpen(void);            // the chip's squelch detector, now
uint16_t MON_BusyAge(void);
uint8_t  MON_AgcByte(uint16_t reg7e);

bool     TONE_Active(void);
void     TONE_Start(uint16_t word, uint8_t gain, uint16_t durationMs);
void     TONE_Stop(uint8_t reason);

void     OVR_SetExpiry(uint16_t seconds, uint8_t keyups);
void     OVR_ClearRam(void);
uint16_t OVR_SecondsLeft(void);
uint8_t  OVR_KeyupsLeft(void);

#endif
