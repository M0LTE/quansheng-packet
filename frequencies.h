/* Copyright 2023 Dual Tachyon
 * https://github.com/DualTachyon
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

#ifndef FREQUENCIES_H
#define FREQUENCIES_H

#include <stdbool.h>
#include <stdint.h>

#define _1GHz_in_KHz 100000000

typedef struct {
	const uint32_t lower;
	const uint32_t upper;
} freq_band_table_t;

extern const freq_band_table_t BX4819_band1;
extern const freq_band_table_t BX4819_band2;

typedef enum  {
	BAND_NONE = -1,
	BAND1_50MHz = 0,
	BAND2_108MHz,
	BAND3_137MHz,
	BAND4_174MHz,
	BAND5_350MHz,
	BAND6_400MHz,
	BAND7_470MHz,
	BAND_N_ELEM
} FREQUENCY_Band_t;

extern const freq_band_table_t frequencyBandTable[];

typedef enum {
// standard steps
	STEP_2_5kHz,
	STEP_5kHz,
	STEP_6_25kHz,
	STEP_10kHz,
	STEP_12_5kHz,
	STEP_25kHz,
	STEP_8_33kHz,
// custom steps
	STEP_0_01kHz,
	STEP_0_05kHz,
	STEP_0_1kHz,
	STEP_0_25kHz,
	STEP_0_5kHz,
	STEP_1kHz,
	STEP_1_25kHz,
	STEP_9kHz,
	STEP_15kHz,
	STEP_20kHz,
	STEP_30kHz,
	STEP_50kHz,
	STEP_100kHz,
	STEP_125kHz,
	STEP_200kHz,
	STEP_250kHz,
	STEP_500kHz,
	STEP_N_ELEM
} STEP_Setting_t;


extern const uint16_t gStepFrequencyTable[];


FREQUENCY_Band_t FREQUENCY_GetBand(uint32_t Frequency);
uint8_t          FREQUENCY_CalculateOutputPower(uint8_t TxpLow, uint8_t TxpMid, uint8_t TxpHigh, int32_t LowerLimit, int32_t Middle, int32_t UpperLimit, int32_t Frequency);
uint32_t 		 FREQUENCY_RoundToStep(uint32_t freq, uint16_t step);

STEP_Setting_t   FREQUENCY_GetStepIdxFromSortedIdx(uint8_t sortedIdx);
uint32_t		 FREQUENCY_GetSortedIdxFromStepIdx(uint8_t step);

// Transmit is allowed only from 136 up to 174 MHz and from 400 up to 470 MHz
// (10 Hz units, upper edges excluded): the ranges the PA and its filters
// are designed for. Fixed at build time.
#define TX_VHF_LOWER     13600000u
#define TX_VHF_UPPER     17400000u
#define TX_UHF_LOWER     40000000u
#define TX_UHF_UPPER     47000000u
// GET_INFO byte 35: this fixed policy. 0 to 7 were the upstream F_LOCK
// plans (v1.0.0 and earlier), so 8 cannot be mistaken for any of them.
#define TX_BAND_POLICY_FIXED  8u

int32_t          TX_freq_check(uint32_t Frequency);      // 0 if TX is allowed
int32_t          RX_freq_check(uint32_t Frequency);
bool             FREQUENCY_IsReceivable(uint32_t Frequency);   // inside the band table

#endif
