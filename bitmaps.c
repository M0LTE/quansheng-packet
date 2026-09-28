
#include "bitmaps.h"

// all these images are on their right sides
// turn your monitor 90-deg anti-clockwise to see the images

const uint8_t BITMAP_TX[8] =
{	// "TX"
	0b00000000,
	0b00000001,
	0b00000001,
	0b01111111,
	0b00000001,
	0b00000001,
	0b00000000,
	0b00000000
};

const uint8_t BITMAP_RX[8] =
{	// "RX"
	0b00000000,
	0b01111111,
	0b00001001,
	0b00011001,
	0b01100110,
	0b00000000,
	0b00000000,
	0b00000000
};

const uint8_t BITMAP_BatteryLevel[2] =
{
	0b01011101,
	0b01011101
};

	// Quansheng way (+ pole to the left)
	const uint8_t BITMAP_BatteryLevel1[17] =
	{
		0b00000000,
		0b00111110,
		0b00100010,
		0b01000001,
		0b01000001,
		0b01000001,
		0b01000001,
		0b01000001,
		0b01000001,
		0b01000001,
		0b01000001,
		0b01000001,
		0b01000001,
		0b01000001,
		0b01000001,
		0b01000001,
		0b01111111
	};

const uint8_t BITMAP_USB_C[9] =
{	// USB symbol
	0b00000000,
	0b00011100,
	0b00100111,
	0b01000100,
	0b01000100,
	0b01000100,
	0b01000100,
	0b00100111,
	0b00011100
};

const uint8_t BITMAP_KeyLock[6] =
{	// teeny padlock symbol
	0b00000000,
	0b01111100,
	0b01000110,
	0b01000101,
	0b01000110,
	0b01111100
};

const uint8_t BITMAP_F_Key[6] =
{	// F-Key symbol
	0b00000000,
	0b01011111,
	0b01000101,
	0b01000101,
	0b01000101,
	0b01000001
};

