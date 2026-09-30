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

#include <assert.h>
#include <string.h>

#include "ARMCM0.h"
#include "app/app.h"
#include "app/events.h"
#include "app/params.h"
#include "app/uart.h"
#include "app/v2.h"
#include "app/wire.h"
#include "board.h"
#include "bsp/dp32g030/dma.h"
#include "bsp/dp32g030/gpio.h"
#include "driver/backlight.h"
#include "driver/bk4819.h"
#include "driver/crc.h"
#include "driver/eeprom.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "driver/systick.h"
#include "functions.h"
#include "misc.h"
#include "outq.h"
#include "settings.h"
#include "version.h"



#define DMA_INDEX(x, y) (((x) + (y)) % sizeof(UART_DMA_Buffer))

typedef struct {
	uint16_t ID;
	uint16_t Size;
} Header_t;

typedef struct {
	Header_t Header;
	uint32_t Timestamp;
} CMD_0514_t;

typedef struct {
	Header_t Header;
	struct {
		char     Version[16];
		bool     bHasCustomAesKey;
		bool     bIsInLockScreen;
		uint8_t  Padding[2];
		uint32_t Challenge[4];
	} Data;
} REPLY_0514_t;

typedef struct {
	Header_t Header;
	uint16_t Offset;
	uint8_t  Size;
	uint8_t  Padding;
	uint32_t Timestamp;
} CMD_051B_t;

typedef struct {
	Header_t Header;
	struct {
		uint16_t Offset;
		uint8_t  Size;
		uint8_t  Padding;
		uint8_t  Data[128];
	} Data;
} REPLY_051B_t;

typedef struct {
	Header_t Header;
	uint16_t Offset;
	uint8_t  Size;
	bool     bAllowPassword;
	uint32_t Timestamp;
	uint8_t  Data[0];
} CMD_051D_t;

typedef struct {
	Header_t Header;
	struct {
		uint16_t Offset;
	} Data;
} REPLY_051D_t;

typedef struct {
	Header_t Header;
	struct {
		uint16_t RSSI;
		uint8_t  ExNoiseIndicator;
		uint8_t  GlitchIndicator;
	} Data;
} REPLY_0527_t;

typedef struct {
	Header_t Header;
	struct {
		uint16_t Voltage;
		uint16_t Current;
	} Data;
} REPLY_0529_t;

typedef struct {
	Header_t Header;
	uint32_t Timestamp;
} CMD_052F_t;

static const uint8_t Obfuscation[16] =
{
	0x16, 0x6C, 0x14, 0xE6, 0x2E, 0x91, 0x0D, 0x40, 0x21, 0x35, 0xD5, 0x40, 0x13, 0x03, 0xE9, 0x80
};

// The legacy handlers cast this buffer to structs holding a u32 at offset
// 4 or 8, and the Cortex-M0 faults on an unaligned word load, so it must be
// word aligned. (Before v2 the linker happened to place it so.)
static union
{
	uint32_t Align;
	uint8_t  Buffer[256];
	struct
	{
		Header_t Header;
		uint8_t Data[252];
	};
} UART_Command;

static_assert(_Alignof(UART_Command) >= 4, "UART_Command must be word aligned");

static uint32_t Timestamp;
static uint16_t gUART_WriteIndex;
static uint16_t gUART_CommandSize;   // payload length of the command in UART_Command
static bool     bIsEncrypted = true;

static uint16_t gLastDma;            // DMA write position at the last look
static uint32_t gLastRxMs;           // when it last moved

uint32_t gRxMs;                      // when the command in UART_Command was seen complete
uint16_t gRxUs;

// Outgoing frames are built here: id, length, body, CRC, footer.
static uint8_t  gTx[4 + UART_BODY_MAX + 4];

// Send the frame header, then the payload in gTx[0 .. len) with its CRC and
// footer. Legacy replies carry FF FF instead of a CRC, v2 frames a real
// CRC-16/XMODEM. Obfuscation, when on, covers the payload and CRC.
static void SendHeader(uint16_t len)
{
	const uint8_t h[4] = { 0xAB, 0xCD, (uint8_t)len, (uint8_t)(len >> 8) };
	UART_Send(h, sizeof(h));
}

static void SendPayload(uint16_t len, bool realCrc)
{
	const uint16_t crc = realCrc ? CRC_Calculate(gTx, len) : 0xFFFF;
	gTx[len + 0] = (uint8_t)crc;
	gTx[len + 1] = (uint8_t)(crc >> 8);
	if (bIsEncrypted)
		for (unsigned int i = 0; i < len + 2u; i++)
			gTx[i] ^= Obfuscation[i % 16];
	gTx[len + 2] = 0xDC;
	gTx[len + 3] = 0xBA;
	UART_Send(gTx, len + 4u);
	OUTQ_Kick();
}

static void SendReply(void *pReply, uint16_t Size)
{
	memcpy(gTx, pReply, Size);
	SendHeader(Size);
	SendPayload(Size, false);
}

uint8_t *UART_FrameBody(void)
{
	return gTx + 4;
}

static void SetId(uint16_t id, uint16_t n)
{
	put16(gTx, id);
	put16(gTx + 2, n);
}

void UART_SendFrameBody(uint16_t id, uint16_t n)
{
	SetId(id, n);
	SendHeader(n + 4);
	SendPayload(n + 4, true);
}

// For frames that carry the time their first byte left: the header goes
// out first, and the time is taken just before it. Exact if nothing was
// queued ahead of it.
bool UART_SendStampedHeader(uint16_t n, uint32_t *ms, uint16_t *us)
{
	const bool exact = OUTQ_Idle();
	CLOCK_Now(ms, us);
	SendHeader(n + 4);
	OUTQ_Kick();
	return exact;
}

void UART_SendStampedBody(uint16_t id, uint16_t n)
{
	SetId(id, n);
	SendPayload(n + 4, true);
}

static void SendVersion(void)
{
	REPLY_0514_t Reply;

	memset(&Reply, 0, sizeof(Reply));
	Reply.Header.ID = 0x0515;
	Reply.Header.Size = sizeof(Reply.Data);
	strncpy(Reply.Data.Version, Version, sizeof(Reply.Data.Version) - 1);
	// No AES key or lock screen. The challenge, which tools read only when
	// the AES flag is set, carries the v2 marker (protocol v2, 3).
	Reply.Data.Challenge[0] = V2_MAGIC;
	Reply.Data.Challenge[1] = V2_PROTOCOL_VERSION;

	SendReply(&Reply, sizeof(Reply));
}

// session init, sends back version info and state
// timestamp is a session id really
static void CMD_0514(const uint8_t *pBuffer)
{
	const CMD_0514_t *pCmd = (const CMD_0514_t *)pBuffer;

	Timestamp = pCmd->Timestamp;


	SendVersion();
}

// read eeprom
static void CMD_051B(const uint8_t *pBuffer)
{
	const CMD_051B_t *pCmd = (const CMD_051B_t *)pBuffer;
	REPLY_051B_t      Reply;

	if (pCmd->Timestamp != Timestamp)
		return;

	// the reply buffer holds 128 bytes; upstream did not check
	if (pCmd->Size > sizeof(Reply.Data.Data))
		return;

	memset(&Reply, 0, sizeof(Reply));
	Reply.Header.ID   = 0x051C;
	Reply.Header.Size = pCmd->Size + 4;
	Reply.Data.Offset = pCmd->Offset;
	Reply.Data.Size   = pCmd->Size;

	EEPROM_ReadBuffer(pCmd->Offset, Reply.Data.Data, pCmd->Size);

	SendReply(&Reply, pCmd->Size + 8);
}

// write eeprom
static void CMD_051D(const uint8_t *pBuffer, const uint16_t CommandSize)
{
	const CMD_051D_t *pCmd = (const CMD_051D_t *)pBuffer;
	REPLY_051D_t Reply;

	if (pCmd->Timestamp != Timestamp)
		return;

	// the data must be whole 8-byte blocks, all inside the received frame
	if ((pCmd->Size % 8) != 0 || sizeof(CMD_051D_t) + pCmd->Size > CommandSize)
		return;

	// Packet firmware: refuse the whole command, with no reply, if any block
	// is unaligned or reaches the factory calibration (0x1E00 and up), so the
	// host sees a failure instead of a silent partial write.
	for (unsigned int i = 0; i < (pCmd->Size / 8); i++)
		if (!EEPROM_IsWritable(pCmd->Offset + (i * 8U)))
			return;

	Reply.Header.ID   = 0x051E;
	Reply.Header.Size = sizeof(Reply.Data);
	Reply.Data.Offset = pCmd->Offset;

	for (unsigned int i = 0; i < (pCmd->Size / 8); i++)
		EEPROM_WriteBuffer(pCmd->Offset + (i * 8U), &pCmd->Data[i * 8U]);

	// apply it once the host has gone quiet (APP_TimeSlice10ms); queued v2
	// EEPROM writes would land on top of this session: drop them
	PARAMS_CancelPersist();
	gReloadSettingsAfterSerial = true;
	gReloadQuietMs             = SERIAL_RELOAD_QUIET_MS;

	SendReply(&Reply, sizeof(Reply));
}

// read RSSI
static void CMD_0527(void)
{
	REPLY_0527_t Reply;

	Reply.Header.ID             = 0x0528;
	Reply.Header.Size           = sizeof(Reply.Data);
	Reply.Data.RSSI             = BK4819_ReadRegister(BK4819_REG_67) & 0x01FF;
	Reply.Data.ExNoiseIndicator = BK4819_ReadRegister(BK4819_REG_65) & 0x007F;
	Reply.Data.GlitchIndicator  = BK4819_ReadRegister(BK4819_REG_63);

	SendReply(&Reply, sizeof(Reply));
}

// read ADC
static void CMD_0529(void)
{
	REPLY_0529_t Reply;

	Reply.Header.ID   = 0x52A;
	Reply.Header.Size = sizeof(Reply.Data);

	// Original doesn't actually send current!
	BOARD_ADC_GetBatteryInfo(&Reply.Data.Voltage, &Reply.Data.Current);

	SendReply(&Reply, sizeof(Reply));
}

// Same as 0x0514 (the vendor programming software's hello). Upstream also
// changed several VFO settings here; none of them exist in this firmware.
static void CMD_052F(const uint8_t *pBuffer)
{
	const CMD_052F_t *pCmd = (const CMD_052F_t *)pBuffer;

	Timestamp = pCmd->Timestamp;

	SendVersion();
}

static void CMD_0601_ReadBK4819Reg(const uint8_t *pBuffer)
{
	typedef struct  __attribute__((__packed__)) {
		Header_t header;
		uint8_t reg;
	} CMD_0601_t;

	CMD_0601_t *cmd = (CMD_0601_t*) pBuffer;

	struct __attribute__((__packed__)) {
		Header_t header;
		struct __attribute__((__packed__)) {
			uint8_t reg;
			uint16_t value;
		} data;
	} reply;

	reply.header.ID = 0x0601;
	reply.header.Size = sizeof(reply.data);
	reply.data.reg = cmd->reg;
	reply.data.value = BK4819_ReadRegister(cmd->reg);
	SendReply(&reply, sizeof(reply));
}

static void CMD_0602_WriteBK4819Reg(const uint8_t *pBuffer)
{
	typedef struct __attribute__((__packed__)) {
		Header_t header;
		uint8_t reg;
		uint16_t value;
	} CMD_0602_t;

	CMD_0602_t *cmd = (CMD_0602_t*) pBuffer;
	BK4819_WriteRegister(cmd->reg, cmd->value);
}

static uint16_t Ahead(uint16_t from, uint16_t to)
{
	return (to + sizeof(UART_DMA_Buffer) - from) % sizeof(UART_DMA_Buffer);
}

// Find the next complete frame in the DMA ring and copy its payload to
// UART_Command. Resynchronises by dropping only the AB of anything that
// cannot be a frame (bad second byte, oversize, bad footer), and of a
// frame still incomplete after UART_GAP_MS without new bytes (truncated,
// for example by PTT pulling the shared line low mid-frame).
bool UART_IsCommandAvailable(void)
{
	const uint16_t DmaLength = DMA_CH0->ST & 0xFFFU;

	if (DmaLength != gLastDma) {
		gLastDma  = DmaLength;
		gLastRxMs = g_ms;
	}
	const bool stale = (uint32_t)(g_ms - gLastRxMs) > UART_GAP_MS;

	while (1)
	{
		while (gUART_WriteIndex != DmaLength && UART_DMA_Buffer[gUART_WriteIndex] != 0xABU)
			gUART_WriteIndex = DMA_INDEX(gUART_WriteIndex, 1);

		if (gUART_WriteIndex == DmaLength)
			return false;

		const uint16_t CommandLength = Ahead(gUART_WriteIndex, DmaLength);
		uint16_t       Size          = 0;
		bool           bad           = false;
		bool           complete      = false;

		if (CommandLength >= 2 && UART_DMA_Buffer[DMA_INDEX(gUART_WriteIndex, 1)] != 0xCD)
			bad = true;
		else if (CommandLength >= 4) {
			const uint16_t Index = DMA_INDEX(gUART_WriteIndex, 2);
			Size = (UART_DMA_Buffer[DMA_INDEX(Index, 1)] << 8) | UART_DMA_Buffer[Index];
			if ((Size + 8u) >= sizeof(UART_DMA_Buffer)) {   // could never complete in the ring
				gCounters[CNT_FRAMES_DROPPED]++;
				bad = true;
			}
			else if (CommandLength >= Size + 8u) {
				const uint16_t TailIndex = DMA_INDEX(Index, Size + 4);
				if (UART_DMA_Buffer[TailIndex] != 0xDC || UART_DMA_Buffer[DMA_INDEX(TailIndex, 1)] != 0xBA) {
					gCounters[CNT_FRAMES_BAD]++;
					bad = true;
				}
				else
					complete = true;
			}
		}

		if (!complete) {
			if (!bad && !stale)
				return false;               // wait for the rest
			if (!bad)
				gCounters[CNT_FRAMES_BAD]++;  // truncated
			gUART_WriteIndex = DMA_INDEX(gUART_WriteIndex, 1);
			continue;
		}

		uint16_t Index     = DMA_INDEX(gUART_WriteIndex, 4);
		uint16_t TailIndex = DMA_INDEX(Index, Size + 2);

		if (TailIndex < Index)
		{
			const uint16_t ChunkSize = sizeof(UART_DMA_Buffer) - Index;
			memcpy(UART_Command.Buffer, UART_DMA_Buffer + Index, ChunkSize);
			memcpy(UART_Command.Buffer + ChunkSize, UART_DMA_Buffer, TailIndex);
		}
		else
			memcpy(UART_Command.Buffer, UART_DMA_Buffer + Index, TailIndex - Index);

		TailIndex = DMA_INDEX(TailIndex, 2);
		if (TailIndex < gUART_WriteIndex)
		{
			memset(UART_DMA_Buffer + gUART_WriteIndex, 0, sizeof(UART_DMA_Buffer) - gUART_WriteIndex);
			memset(UART_DMA_Buffer, 0, TailIndex);
		}
		else
			memset(UART_DMA_Buffer + gUART_WriteIndex, 0, TailIndex - gUART_WriteIndex);

		gUART_WriteIndex = TailIndex;
		gUART_CommandSize = Size;

		if (UART_Command.Header.ID == 0x0514)
			bIsEncrypted = false;

		if (UART_Command.Header.ID == 0x6902)
			bIsEncrypted = true;

		if (bIsEncrypted)
		{
			unsigned int i;
			for (i = 0; i < (Size + 2u); i++)
				UART_Command.Buffer[i] ^= Obfuscation[i % 16];
		}

		const uint16_t CRC = UART_Command.Buffer[Size] | (UART_Command.Buffer[Size + 1] << 8);

		if (CRC_Calculate(UART_Command.Buffer, Size) != CRC) {
			gCounters[CNT_FRAMES_BAD]++;
			continue;
		}

		CLOCK_Now(&gRxMs, &gRxUs);
		return true;
	}
}

// Stop transmitting before a reboot: PA off, red LED off, BK4819 off.
static void DeKey(void)
{
	BK4819_SetupPowerAmplifier(0, 0);
	BK4819_ToggleGpioOut(BK4819_GPIO1_PIN29_PA_ENABLE, false);
	BK4819_ToggleGpioOut(BK4819_GPIO5_PIN1_RED, false);
	BK4819_WriteRegister(BK4819_REG_30, 0);
}

void UART_HandleCommand(void)
{
	// Every valid frame, whatever it asks for, starts the serial PTT lock
	// and ends any transmission: the UART line shares a contact with PTT on
	// the K1 connector (protocol v2, 5.3).
	APP_OnSerialFrame();
	gCounters[CNT_FRAMES_OK]++;

	const uint16_t id = UART_Command.Header.ID;

	if ((id & 0xFF00u) == 0x5000u) {
		// the tag, if there is one, is the first body byte
		V2_Handle(id, UART_Command.Buffer + 4, UART_Command.Header.Size, gUART_CommandSize);
		return;
	}

	switch (id)
	{
		case 0x0514:
			EVT_ResetSubscription();
			CMD_0514(UART_Command.Buffer);
			break;
	
		case 0x051B:
			CMD_051B(UART_Command.Buffer);
			break;
	
		case 0x051D:
			CMD_051D(UART_Command.Buffer, gUART_CommandSize);
			break;
	
		case 0x0527:
			CMD_0527();
			break;
	
		case 0x0529:
			CMD_0529();
			break;
	
		case 0x052F:
			EVT_ResetSubscription();
			CMD_052F(UART_Command.Buffer);
			break;
	
		case 0x05DD: // reset
			DeKey();
			NVIC_SystemReset();
			break;

		case 0x0601: // id, size, register
			if (gUART_CommandSize >= 5)
				CMD_0601_ReadBK4819Reg(UART_Command.Buffer);
			break;

		case 0x0602: // id, size, register, value
			if (gUART_CommandSize >= 7)
				CMD_0602_WriteBK4819Reg(UART_Command.Buffer);
			break;
	}
}

#ifdef HOST_TEST
// the host tests reboot the radio without reloading this file
void UART_HostReset(void)
{
	bIsEncrypted     = true;
	gUART_WriteIndex = DMA_CH0->ST & 0xFFFU;
	gLastDma         = gUART_WriteIndex;
}
#endif

// Every complete frame, on every pass of the main loop.
void UART_Poll(void)
{
	while (UART_IsCommandAvailable())
		UART_HandleCommand();
}
