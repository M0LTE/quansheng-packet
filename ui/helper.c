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

#include <string.h>

#include "driver/st7565.h"
#include "external/printf/printf.h"
#include "font.h"
#include "ui/helper.h"
#include "ui/inputbox.h"
#include "misc.h"
#include "packet.h"

#ifndef ARRAY_SIZE
	#define ARRAY_SIZE(arr) (sizeof(arr)/sizeof((arr)[0]))
#endif

// Nominal UV-K5 output for the low, mid and high calibration rows, the
// same on VHF and UHF; the tilde says it is not a measurement.
const char gPowerNames[3][6] = { "~0.5W", "~2W", "~5W" };

// 2^(i/16) for i = 0 to 16, in 16.16 fixed point
static const uint32_t Pow2Sixteenths[17] = {
	65536, 68438, 71468, 74632, 77936, 81386, 84990, 88752,
	92682, 96785, 101070, 105545, 110218, 115098, 120194, 125515, 131072
};

unsigned int UI_DeviationTenthsKHz(uint16_t reg)
{
	if (reg > PKT_DEVIATION_MAX)
		reg = PKT_DEVIATION_MAX;

	// reg - REF as 256 * octaves + fraction, kept unsigned by adding 16
	// octaves, which is more than reg can be below REF
	const uint32_t u        = reg + (16u << 8) - PKT_DEVIATION_REF_REG;
	const int      octaves  = (int)(u >> 8) - 16;
	const uint32_t fraction = u & 0xFF;

	// 2^(fraction / 256), interpolating between sixteenths
	const uint32_t lo = Pow2Sixteenths[fraction >> 4];
	const uint32_t hi = Pow2Sixteenths[(fraction >> 4) + 1];
	const uint32_t m  = lo + (((hi - lo) * (fraction & 15)) >> 4);

	// Hz in 16.16: at most 2800 * 2^(0x229 / 256) * 65536, about 8.2e8
	uint32_t hz = PKT_DEVIATION_REF_HZ * m;
	if (octaves >= 0)
		hz <<= octaves;
	else
		hz >>= -octaves;

	return (hz + (50u << 16)) / (100u << 16);
}

void UI_DeviationString(char *pString, uint16_t reg)
{
	const unsigned int tenths = UI_DeviationTenthsKHz(reg);
	if (tenths == 0)
		strcpy(pString, "<0.1kHz");
	else
		sprintf(pString, "~%u.%ukHz", tenths / 10, tenths % 10);
}

void UI_PrintStringBuffer(const char *pString, uint8_t * buffer, uint32_t char_width, const uint8_t *font)
{
	const size_t Length = strlen(pString);
	const unsigned int char_spacing = char_width + 1;
	for (size_t i = 0; i < Length; i++) {
		const unsigned int index = pString[i] - ' ' - 1;
		if (pString[i] > ' ' && pString[i] < 127) {
			const uint32_t offset = i * char_spacing + 1;
			memcpy(buffer + offset, font + index * char_width, char_width);
		}
	}
}

void UI_PrintString(const char *pString, uint8_t Start, uint8_t End, uint8_t Line, uint8_t Width)
{
	size_t i;
	size_t Length = strlen(pString);

	if (End > Start)
		Start += (((End - Start) - (Length * Width)) + 1) / 2;

	for (i = 0; i < Length; i++)
	{
		const unsigned int ofs   = (unsigned int)Start + (i * Width);
		if (pString[i] > ' ' && pString[i] < 127)
		{
			const unsigned int index = pString[i] - ' ' - 1;
			memcpy(gFrameBuffer[Line + 0] + ofs, &gFontBig[index][0], 7);
			memcpy(gFrameBuffer[Line + 1] + ofs, &gFontBig[index][7], 7);
		}
	}
}

void UI_PrintStringSmall(const char *pString, uint8_t Start, uint8_t End, uint8_t Line, uint8_t char_width, const uint8_t *font)
{
	const size_t Length = strlen(pString);
	const unsigned int char_spacing = char_width + 1;

	if (End > Start) {
		Start += (((End - Start) - Length * char_spacing) + 1) / 2;
	}

	UI_PrintStringBuffer(pString, gFrameBuffer[Line] + Start, char_width, font);
}

void UI_PrintStringSmallNormal(const char *pString, uint8_t Start, uint8_t End, uint8_t Line)
{
	UI_PrintStringSmall(pString, Start, End, Line, ARRAY_SIZE(gFontSmall[0]), (const uint8_t *)gFontSmall);
}

void UI_PrintStringSmallBold(const char *pString, uint8_t Start, uint8_t End, uint8_t Line)
{
	const uint8_t *font = (uint8_t *)gFontSmall;
	const uint8_t char_width = ARRAY_SIZE(gFontSmall[0]);

	UI_PrintStringSmall(pString, Start, End, Line, char_width, font);
}

void UI_PrintStringSmallBufferNormal(const char *pString, uint8_t * buffer)
{
	UI_PrintStringBuffer(pString, buffer, ARRAY_SIZE(gFontSmall[0]), (uint8_t *)gFontSmall);
}

void UI_DisplayFrequency(const char *string, uint8_t X, uint8_t Y, bool center)
{
	const unsigned int char_width  = 13;
	uint8_t           *pFb0        = gFrameBuffer[Y] + X;
	uint8_t           *pFb1        = pFb0 + 128;
	bool               bCanDisplay = false;

	uint8_t len = strlen(string);
	for(int i = 0; i < len; i++) {
		char c = string[i];
		if(c=='-') c = '9' + 1;
		if (bCanDisplay || c != ' ')
		{
			bCanDisplay = true;
			if(c>='0' && c<='9' + 1) {
				memcpy(pFb0 + 2, gFontBigDigits[c-'0'],                  char_width - 3);
				memcpy(pFb1 + 2, gFontBigDigits[c-'0'] + char_width - 3, char_width - 3);
			}
			else if(c=='.') {
				*pFb1 = 0x60; pFb0++; pFb1++;
				*pFb1 = 0x60; pFb0++; pFb1++;
				*pFb1 = 0x60; pFb0++; pFb1++;
				continue;
			}

		}
		else if (center) {
			pFb0 -= 6;
			pFb1 -= 6;
		}
		pFb0 += char_width;
		pFb1 += char_width;
	}
}

void UI_DisplayPopup(const char *string)
{
	UI_DisplayClear();

	// for(uint8_t i = 1; i < 5; i++) {
	// 	memset(gFrameBuffer[i]+8, 0x00, 111);
	// }

	// for(uint8_t x = 10; x < 118; x++) {
	// 	UI_DrawPixelBuffer(x, 10, true);
	// 	UI_DrawPixelBuffer(x, 46-9, true);
	// }

	// for(uint8_t y = 11; y < 37; y++) {
	// 	UI_DrawPixelBuffer(10, y, true);
	// 	UI_DrawPixelBuffer(117, y, true);
	// }
	// DrawRectangle(9,9, 118,38, true);
	UI_PrintString(string, 9, 118, 2, 8);
	UI_PrintStringSmallNormal("Press EXIT", 9, 118, 6);
}

void UI_DisplayClear()
{
	memset(gFrameBuffer, 0, sizeof(gFrameBuffer));
}
