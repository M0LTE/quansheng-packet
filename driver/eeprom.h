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

#ifndef DRIVER_EEPROM_H
#define DRIVER_EEPROM_H

#include <stdbool.h>
#include <stdint.h>

// Factory calibration lives from here to the end of the 8 KiB EEPROM.
// This firmware never writes it.
#define EEPROM_CALIBRATION_START 0x1E00u

extern uint32_t gEepromBlocksWritten;   // 8-byte blocks actually written

// Set on every block actually written, whoever wrote it (keypad, menu,
// protocol, legacy UART). The one place that caches EEPROM contents, the
// protocol's stored parameter view (app/params.c), clears it when it
// re-reads, so no save path has to remember to tell it.
extern bool gEepromChanged;

bool EEPROM_IsWritable(uint16_t Address);
void EEPROM_ReadBuffer(uint16_t Address, void *pBuffer, uint8_t Size);
void EEPROM_WriteBuffer(uint16_t Address, const void *pBuffer);

#endif

