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

// REG_7D: mic sensitivity. <4:0> is the gain, nominally 0.5 dB per step
// (0 to 31). Measured on the bench K5 (2026-09-28): the whole range moves
// the deviation by only about 0.5 dB, so REG_40 sets the level. The upper
// bits are the upstream value.
#define PKT_REG_7D_BASE          0xE940u
#define PKT_MIC_GAIN_MAX         31u
#define PKT_MIC_GAIN_DEFAULT     31u

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
//
// <11:0> is logarithmic (bench K5, 2026-09-28): +0x100 doubles the
// deviation, about 0.0235 dB per LSB. 0x862 gives 3.13 kHz for a 999 Hz
// tone at -6 dBFS from the AIOC (mic level), linear up to 0 dBFS; 0x800
// gives 3.67 kHz at -1.94 dBFS. From 0xB00 up the value wraps to near zero
// deviation, so the settings are clamped to PKT_DEVIATION_MAX.
// Narrow is half the wide deviation: 0x100 below wide.
//
// Defaults: 0x956 wide, 0x856 narrow, matched to the bench AIOC, which
// carries a stored TX EQ that cuts 5.74 dB at 1 kHz (0x956 = 0x862 plus
// 5.74 dB at 0.0235 dB per step). With a stock AIOC 0x862 gives 3 kHz at
// -6 dBFS; the deviation is a setting, so change it to suit the interface.
// (Upstream DIG used 0x383 wide and 0x4D6 narrow.)
#define PKT_REG_40_ENABLE        0x1000u
#define PKT_REG_40_DEV_MASK      0x0FFFu
#define PKT_DEVIATION_MAX        0x0A7Fu
#define PKT_DEVIATION_WIDE_DEFAULT    0x0956u
#define PKT_DEVIATION_NARROW_DEFAULT  0x0856u

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
