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

// Packet firmware: the one audio path this firmware has.
//
// It is the flat ("DIG") path of mobilinkd's ENABLE_DIGITAL_MODULATION build:
// no pre-emphasis or de-emphasis, no TX or RX audio filters, no DC filters,
// no mic AGC, no ALC, no compander. The fixed register values are named here
// so later measurements can change them in one place. The values the bench
// is expected to tune (mic gain, deviation, receive gains) are settings,
// stored in EEPROM (see settings.h) and editable in the menu or over UART.
//
// Register meanings come from the BK4819 vendor list and community notes and
// are hypotheses until measured.

#ifndef PACKET_H
#define PACKET_H

// REG_7D: mic sensitivity. <4:0> is the gain, 0.5 dB per step (0 to 31).
// The upper bits are the upstream value.
#define PKT_REG_7D_BASE          0xE940u
#define PKT_MIC_GAIN_MAX         31u
#define PKT_MIC_GAIN_DEFAULT     0u      // upstream DIG value

// REG_47 while transmitting: AF output muted, <0> = 1 bypasses all AF TX
// filters. <14> is cleared, as upstream DIG does (undocumented).
#define PKT_REG_47_TX            ((2u << 12) | (1u << 6) | 1u)

// REG_7E<5:3> is the TX DC filter, <2:0> the RX DC filter: 0 bypasses them.
// REG_7E<15> is AGC fix mode. Upstream DIG sets <15> on key-up (its comment
// says "Disable TX DC filter"). It is kept for transmit, and cleared again on
// every return to receive so the RX AGC is never left frozen.
#define PKT_REG_7E_DC_FILTERS    0x003Fu
#define PKT_REG_7E_TX_DC_FILTER  0x0038u
#define PKT_REG_7E_AGC_FIX       0x8000u

// REG_2B: <10:8> set = RX HPF300, RX LPF3K and de-emphasis off,
//         <2:0>  set = TX HPF300, TX LPF1 and pre-emphasis off.
#define PKT_REG_2B_FLAT_MASK     0x0707u

// REG_40: TX deviation. <11:0> is the deviation, <12> is set as upstream
// does, <15:13> keep the chip's own power-on value (read after soft reset).
// Upstream DIG used 0x383 wide and 0x4D6 narrow, so wide was 2.8 dB below
// narrow. The default here is the chip default 0x4D0 for both; the chip's
// bandwidth mode (REG_43) does the rest. Measurement decides.
#define PKT_REG_40_ENABLE        0x1000u
#define PKT_DEVIATION_MAX        0x0FFFu
#define PKT_DEVIATION_DEFAULT    0x04D0u

// REG_48: RX audio. <15:12> = 11 (upstream, undocumented), <11:10> AF RX
// gain 1 = 0 dB, <9:4> AF RX gain 2 (0 to 63, 0.5 dB steps), <3:0> AF DAC
// gain (0 to 15, about 2 dB steps). Upstream forced the DAC gain to 15 on
// every squelch open; that is the default here.
#define PKT_REG_48_BASE          ((11u << 12) | (0u << 10))
#define PKT_RX_GAIN_MAX          63u
#define PKT_RX_DAC_GAIN_MAX      15u
#define PKT_RX_DAC_GAIN_DEFAULT  15u
// AF RX gain 2 defaults to the factory calibration value (EEPROM 0x1F8E),
// or this if the calibration byte is invalid.
#define PKT_RX_GAIN_FALLBACK     58u

// REG_31: <1> scrambler, <2> VOX, <3> compander. All kept off.
#define PKT_REG_31_OFF_MASK      0x000Eu

// REG_3D: IF setting written on every squelch open (upstream, not USB).
#define PKT_REG_3D_RX            0x2AABu

#endif
