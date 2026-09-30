# Serial control protocol v2 (packet firmware)

Status: implemented in the firmware, host-tested and verified on the bench radio; section 13 lists what is implemented and where the firmware settled a point this text left open. Written 29 September 2026 against the v1 firmware (2951c48); amended 30 September 2026 with the implementation and the release changes (retired parameters and ops are marked where they were).

This is the serial protocol a TNC or soundmodem (pdn-soundmodem, say) uses to control and watch the UV-K5 packet firmware through an AIOC: AIOC sound card for audio, AIOC CM108 HID for PTT, AIOC CDC serial for this protocol. It adds a command set in its own ID range (0x50xx) to the existing framing. Every existing command and the framing stay as they are on the wire.

It is written so that one engineer can build the firmware side and another, independently, a client and a simulator. "MUST" and "SHOULD" are normative. All multi-byte fields are little-endian and packed, with no padding. Types: `u8 u16 u32 s16`, `char[n]` (NUL-padded ASCII).

Contents: 1 the shared contact, 2 framing, 3 identification, 4 message IDs and common headers, 5 host timing rules, 6 commands, 7 parameters, 8 events, 9 firmware requirements, 10 frame and MCU budget, 11 rationale, 12 open questions and wanted measurements.

## 1. The shared contact and what it means

The K1 plug is two TRS jacks. From the AIOC schematic (`kicad/k1-aioc/k1-aioc.kicad_sch`, the pinout note):

| Contact | Signal |
|---|---|
| 2.5 mm tip | speaker out (radio to AIOC audio in) |
| 2.5 mm ring | **radio TX data** (radio to host), its own contact |
| 3.5 mm ring | mic (AIOC audio out to radio) |
| 3.5 mm sleeve | **PTT1 and radio RX data** (host to radio), one contact |

So PTT is shared with the **radio's receive line** (host-to-radio data, AIOC `USART1-TX`, net `RADIO-RX`). The radio reads that contact as PTT on GPIOC5 and as UART1 RX. The radio-to-host line is electrically separate.

Consequences:

**(a) Host-to-radio commands while keyed.** PTT is the AIOC pulling the contact low; UART data needs the contact free. The AIOC resolves this with `TXFRCPTT` (register 0x60 bits 11:8, default PTT1): the moment the host writes to the CDC port, the AIOC releases PTT1 (`usb_serial.c` `tud_cdc_rx_cb`) and, from the source, does not re-assert it until the host sends its next HID report. The radio sees the line go high, counts it as a release (5 ms release debounce, or at once when the frame completes, see 5.3), ends the transmission, then receives and executes the frame normally. A command sent while keyed therefore ends the transmission. `TXFRCPTT` MUST stay enabled for PTT1: without it the AIOC's push-pull UART output would fight the PTT pull-down on one wire.

**(b) Radio-to-host frames while keyed.** Nothing on the radio side stops them, but the AIOC drops every byte it receives from the radio while PTT1 is asserted (`RXIGNPTT`, register 0x60 bits 19:16, default PTT1, `usb_serial.c` RXNE handler). On the K1 wiring this is policy, not physics, because the radio's TX data has its own contact. So:
- By default the radio holds its unsolicited frames back while PTT is asserted and sends them after the release (8.1). A frame already on the wire when PTT is asserted is truncated; clients resynchronise (2.3) and notice the gap in event sequence numbers.
- A host MAY write AIOC register 0x60 = `0x00000100` (TXFRCPTT = PTT1, RXIGNPTT = none) in RAM only (HID feature report, ctrl bit 0, never the store bit) at start-up and then subscribe with `LIVE_TX` (6.3). Events then arrive during a transmission, including `TX_START` the moment RF is ready. Measurement M2 must confirm this before clients rely on it.

**(c) The post-command PTT lock.** After every valid frame the firmware ignores PTT for a while and ends any transmission. In v1 this is `SERIAL_PTT_LOCK_500ms = 3`: 1.0 to 1.5 s, refreshed by every valid frame. Measured on the bench radio (2951c48, 29 September): after a hello plus RSSI session, a 2.0 s AIOC key D seconds after the session gave 1.085 s of RF at D = 0.1 s, 1.665 s at D = 0.4 s and the full 2.0 s from D = 0.7 s (D includes about 0.2 s of tool start-up). So **a press during the lock is not refused: the radio keys late, when the lock runs out, silently eating the start of the frame.** v2 fixes this (5.3): the lock becomes `SERIAL_LOCK_MS` (default 20 ms, 1 ms resolution), every reply tells the host how long it has left, and a press during the lock either keys at most 30 ms late (reported in `TX_START`) or is refused until PTT is released (reported in `TX_REFUSED`).

**Can the lock be that short, and what is the risk?** What stops serial data being taken as a press is the 280 us window in `ptt.c`, not the lock: at 38400 baud every character (260 us) ends in a high stop bit, so no byte stream of any content holds the line low for a whole window, and the host tests prove it for zero bytes and random bytes at every phase. The lock is armed only by a valid 38400-baud frame, so it never protected against the cases the window cannot handle. Those are the real residual risks, with any lock length:
- a BREAK condition (a host asserting break for 5 ms or more),
- a host UART at a wrong, slower rate (by arithmetic, runs of 0x00 at 9600 baud pass about 16 consecutive windows, at 19200 about 4 or 5; the 5 ms press debounce needs 5),
- anything that makes the AIOC hold the contact low for 5 ms or more (port open or close, line-coding change, USB re-enumeration, AIOC reset or DFU; unmeasured, M4).

The long lock covered these only within 1.5 s after a valid frame. Shortening it gives up that small window and nothing else. Hosts MUST open the port at 38400 8N1 and MUST NOT send breaks.

## 2. Framing (unchanged)

```
AB CD | len u16 | payload (len bytes) + crc16 u16 | DC BA
payload = id u16 | body_len u16 | body (body_len bytes)       body_len = len - 4
```

- `crc16` is CRC-16/XMODEM (poly 0x1021, init 0) over the payload. In obfuscated mode payload and CRC are XORed with `16 6C 14 E6 2E 91 0D 40 21 35 D5 40 13 03 E9 80`, cycling from payload byte 0. 38400 8N1; the radio actually runs at about 39056 baud (1.7% fast).
- Mode: a hello whose raw id bytes are `14 05` switches the radio to plain mode; raw `02 69` (an obfuscated 0x0514) switches it back. Power-on mode is obfuscated. The mode applies to both directions and to v2 frames exactly as to legacy ones.
- Frames from the radio: legacy replies keep `FF FF` (obfuscated as usual) in the CRC field, as today. **All 0x50xx frames from the radio carry a real CRC.** Hosts MUST check it on 0x50xx frames and accept either on legacy frames (k5.py already does).
- The radio drops frames with a bad CRC, a bad footer, or `len + 8 >= 256` (its 256-byte DMA ring; a frame filling all of it could never complete), without reply. It resynchronises as a host does (2.3): anything that cannot be a frame (second byte not `CD`, oversize length, bad footer) loses only its `AB`, and a frame still incomplete 5 ms after the last byte arrived is dropped as truncated, so a frame cut short (for example by PTT pulling the shared line low) never swallows the frames after it.
- For v2 IDs the firmware MUST check `body_len == len - 4`; otherwise it replies `BAD_LENGTH`. A 0x50xx frame too short to hold the tag gets `BAD_LENGTH` with tag 0. Frames with ids 0x5080 to 0x50FF from the host are not requests and get no reply.
- v2 request bodies MUST NOT exceed 120 bytes (frame of 132 bytes), so that two frames fit in the 256-byte receive ring.

### 2.1 Legacy commands

0x0514 and 0x052F (hello), 0x051B (EEPROM read), 0x051D (EEPROM write), 0x0527 (RSSI), 0x0529 (battery), 0x05DD (reboot), 0x0601 (BK4819 register read) and, in bench builds only, 0x0602 (register write) behave exactly as in `docs/packet-fw.md`. Release builds ignore 0x0602 like any unknown legacy id. Visible differences in v2 firmware, none of them a change of wire format:
- the 0x0515 hello reply carries the v2 marker in its challenge field (3);
- the PTT lock after any frame is `SERIAL_LOCK_MS` (5.3); the settings reload after an 0x051D write session uses its own timer: 1.0 s after the last 0x051D, never during TX;
- frames are processed within 5 ms instead of one per 10 ms slice, so back-to-back frames are no longer lost (except while an EEPROM block is being written: about 8 ms each, 6.6);
- a hello resets the live event subscription to the stored default (6.3), so a tool that takes over the port does not get event frames it does not expect.

### 2.2 Radio output that is not a frame

At power-on the radio prints an unframed ASCII line, `UV-K5 packet firmware, PKTFW <hash>\r\n`. Hosts MUST skip bytes outside frames.

### 2.3 Host parser

Scan for `AB CD`; read `len`; if `len > 250` drop the `AB` and rescan; wait for `len + 4` more bytes; if the footer is not `DC BA`, drop only the `AB` and rescan from the next byte (a truncated frame followed by a good one must be recoverable); de-obfuscate; check the CRC (0x50xx IDs); dispatch.

## 3. Identification

The 0x0515 reply to a hello (`id, len, char Version[16], u8 HasCustomAesKey, u8 IsInLockScreen, u8 pad[2], u32 Challenge[4]`) is unchanged except that v2 firmware fills the challenge, which tools only read when `HasCustomAesKey` is set (never, here):

| Body offset | Size | Field |
|---|---|---|
| 20 | u32 | magic `0x32544B50` (bytes `50 4B 54 32`, "PKT2") |
| 24 | u16 | protocol version, 0x0200 (major 2, minor 0) |
| 26 | u16 | 0 |
| 28 | u32[2] | 0 |

Host procedure: open at 38400 8N1 with DTR and RTS low; send a hello (either mode; the reference client uses obfuscated, like every other tool); read 0x0515.
- magic present: v2, protocol version as given; then `GET_INFO` for capabilities.
- no magic, version starts `PKTFW`: packet firmware v1 (legacy commands only).
- otherwise: stock or another firmware. Do not send 0x50xx commands to it.

Unknown IDs outside 0x50xx are ignored silently by every firmware, so a stray `GET_INFO` is harmless; a v1 or stock radio simply does not answer within the 100 ms reply timeout.

## 4. Message IDs and common headers

### 4.1 ID range

This firmware owns 0x5000 to 0x50FF. Known community IDs, all avoided: 0x0514 to 0x0530 (vendor and firmware), 0x0518, 0x0519, 0x051A, 0x0530 (bootloader 2.x), 0x057A to 0x057D (bootloader 5.x), 0x05DD, 0x0601 and 0x0602 (register access), 0x0700 to 0x0709 and 0x0B01 to 0x0B85 (TA1JS APRS firmware, as used by uv-k5-igate). 0x50xx never equals the raw hello ids 0x0514 or 0x6902 in either mode (obfuscated raw is `id ^ 0x6C16`).

| Range | Use |
|---|---|
| 0x5000 to 0x507F | requests (host to radio) |
| 0x5080 to 0x50FF except events | replies: reply id = request id + 0x80, always, also for errors and for undefined request ids in 0x5000 to 0x507F |
| 0x50C0 to 0x50DF | events (radio to host, unsolicited); event n is 0x50C0 + n and subscription bit n |
| 0x5020 | reserved (serial keying, not defined; see 11) |

### 4.2 Request header (first body bytes of every 0x50xx request)

| Off | Type | Field |
|---|---|---|
| 0 | u8 | tag, chosen by the host (SHOULD increment per command), echoed in the reply |

### 4.3 Reply header (first body bytes of every 0x50xx reply)

| Off | Type | Field |
|---|---|---|
| 0 | u8 | tag, echoed |
| 1 | u8 | status (table below) |
| 2 | u16 | lock_ms: serial PTT lock remaining when the reply was queued, ms |

On an error status the reply body after the header is one `u8 detail` (the offending parameter id, register or field index, else 0).

| Status | Name | Meaning |
|---|---|---|
| 0x00 | OK | |
| 0x01 | UNKNOWN_CMD | undefined request id in 0x5000 to 0x507F |
| 0x02 | BAD_LENGTH | body too short or too long, or body_len mismatch |
| 0x03 | BAD_PARAM | unknown or repeated parameter id (detail = id) |
| 0x04 | RANGE | value out of range (detail = parameter id or field offset) |
| 0x05 | TX_BAND | frequency not TX-allowed and REQUIRE_TX_OK was set |
| 0x06 | STATE | not possible in the current state (reduced service) |
| 0x07 | REFUSED | register not allowed (detail = register) |
| 0x08 | UNSUPPORTED | capability absent or uncalibrated |
| 0x09 | EEPROM | settings block not valid, cannot persist |
| 0x0A | NOT_PERSISTABLE | RAM-only parameter with PERSIST (detail = id) |

### 4.4 Event header (first body bytes of every event)

| Off | Type | Field |
|---|---|---|
| 0 | u16 | seq: stored events number 0, 1, 2 ... from boot, wrapping at 65536; ephemeral events carry the seq the next stored event will get |
| 2 | u32 | t_ms: radio clock, ms since boot (meaning of the instant defined per event) |
| 6 | u8 | flags: bit 0 REPLAY, bit 1 DEFERRED (held while PTT was asserted), bit 2 QUEUED (waited more than 2 ms behind other output), bit 3 EPHEMERAL (not stored, not replayable), bit 4 TIME_EXACT (the output queue was empty, so the frame's first byte left within 1 ms of t_ms) |

An event is stored, and takes a sequence number, only if the live subscription includes it when it happens (`EVENTS_LOST`: if any stored event is subscribed), so a client never sees gaps for events it did not ask for. A client keeps `expected = last_seq + 1`. A stored event with a larger seq, or an ephemeral event whose seq differs from `expected`, means events were missed; fetch them with `EVENT_REPLAY` when idle.

## 5. Host timing rules

### 5.1 Commands

- One outstanding 0x50xx command at a time: wait for its reply (matching tag) or a 100 ms timeout. Events may arrive in between.
- The radio replies within 5 ms of the last command byte (plus its own output queue, at most about 15 ms). `EVENT_REPLAY` sends its events before the reply.
- On timeout, retry once. All commands are safe to repeat except `REG_WRITE` and `REG_OVERRIDE` ADD; re-read state instead of repeating those.
- Legacy commands have no tag; 0x0602 (bench builds) has no reply at all.

### 5.2 When the host may key

- After a 0x50xx command: at or after `t_reply + lock_ms`, where `t_reply` is when the reply's last byte arrived. The lock started before the reply was queued, so this is always safe. With the default lock that is 20 ms.
- After a legacy command with a reply: `t_reply + SERIAL_LOCK_MS + 2 ms` (`SERIAL_LOCK_MS` from `GET_INFO`).
- After a command with no reply (0x0602) or a lost reply: `t_write_done + 0.27 ms x frame_bytes + 12 ms + SERIAL_LOCK_MS`.
- With v1 firmware: 1.5 s after the last frame.

A press inside the lock is handled as in 5.3; clients MUST NOT rely on it.

### 5.3 Radio PTT behaviour (normative for firmware and simulator)

Evaluated on every 1 ms tick. `press` is the v1 debounced press (280 us windows, `PTT_PRESS_MS`), now computed even while the lock runs.

1. Every valid frame (any id, legacy included) sets `lock_ms = SERIAL_LOCK_MS` and ends any transmission (`TX_END` reason SERIAL, if it had not already ended as a release), before its command runs. A transmission ended this way, and a pending late key, need PTT released and pressed again.
2. On a press edge:
   - `lock_ms == 0` and no other bar: key normally.
   - `0 < lock_ms <= 30` (LATE_KEY_MAX_MS): key when `lock_ms` reaches 0 if still pressed; `TX_START.lock_delay_ms` gives the delay.
   - `lock_ms > 30`: refuse and latch; `TX_REFUSED` reason LOCK, detail = lock_ms. No transmission until PTT is released and pressed again.
   - frequency not TX-allowed, battery empty, over-voltage, reduced service: refuse and latch, `TX_REFUSED` with the reason (v1 already refuses these; v2 reports them).
   - The radio never keys more than LATE_KEY_MAX_MS after the press edge. A press it cannot act on in time (the main loop held up, for example by a long legacy EEPROM write session) is refused and latched, `TX_REFUSED` reason LATE with the delay as detail.
   - A frame that arrives between the press edge and the radio acting on it cancels the press (rule 1): refused and latched, `TX_REFUSED` reason LOCK with the lock remaining as detail.
3. On a release edge: clear any pending late key and any latch.
4. TX timeout: end the transmission (`TX_END` reason TIMEOUT) and latch until release, as v1.

### 5.4 Other rules

- MUST NOT write to the port while PTT is asserted (1a). If a client does, it MUST treat the transmission as ended.
- After unkeying, a client SHOULD wait for `TX_END` (or `PTT_RELEASE_MS + 2 ms`) before sending, so the release is recorded as a release.
- MUST NOT send breaks, MUST NOT open the port at another rate while the radio is connected (1c).
- Prefer events to polling: every command costs a lock. Polling 0x0527 now costs 20 ms of lock, not 1.5 s, but still costs air time if it collides with a key-up.
- If `LIVE_TX` is active, a client MAY key and then start data only when `TX_START` arrives (RF ready), aborting on `TX_REFUSED`. That makes TXDELAY track what the radio really did, including a late key.

## 6. Commands

Each table gives the body after the 1-byte request header (4.2) or the 4-byte reply header (4.3). Offsets are relative to that point.

| Id | Name | Reply |
|---|---|---|
| 0x5000 | GET_INFO | 0x5080 |
| 0x5001 | GET_STATUS | 0x5081 |
| 0x5002 | SUBSCRIBE | 0x5082 |
| 0x5003 | TIME_SYNC | 0x5083 |
| 0x5004 | GET_PARAMS | 0x5084 |
| 0x5005 | SET_PARAMS | 0x5085 |
| 0x5006 | SAVE_PARAMS | 0x5086 |
| 0x5007 | LEVEL_TONE | 0x5087 |
| 0x5008 | REG_READ | 0x5088 |
| 0x5009 | REG_WRITE | 0x5089 |
| 0x500A | REG_OVERRIDE | 0x508A |
| 0x500B | EVENT_REPLAY | 0x508B |
| 0x500C | GET_COUNTERS | 0x508C |

### 6.1 GET_INFO (0x5000)

Request: empty. Reply (40 bytes):

| Off | Type | Field |
|---|---|---|
| 0 | u16 | protocol version (0x0200) |
| 2 | char[16] | firmware version, as in 0x0515 |
| 18 | u32 | caps (below) |
| 22 | u32 | params: bit n set = parameter id n supported |
| 26 | u32 | events: bit n set = event 0x50C0 + n supported |
| 30 | u8 | max request body, bytes (at least 120) |
| 31 | u8 | event ring capacity, events (at least 16; 20 in this firmware) |
| 32 | u16 | SERIAL_LOCK_MS in force |
| 34 | u8 | LATE_KEY_MAX_MS (30) |
| 35 | u8 | TX band plan (F_LOCK, 0 to 7 as `settings.h`) |
| 36 | u8 | TX band flags: bit 0 200TX, bit 1 350TX, bit 2 500TX, bit 3 350EN |
| 37 | u8 | settings block layout (1), 0 if the block is not valid |
| 38 | u8 | v2 block layout (1), 0 if not valid |
| 39 | u8 | default burst sample period, ms |

Caps: bit 0 LIVE_TX option, bit 1 RSSI busy detector, bit 2 LEVEL_TONE raw, bit 3 LEVEL_TONE deviation mode calibrated, bit 4 RX AF amplitude meaningful, bit 5 AGC readback meaningful, bit 6 frequency error available, bit 7 TX mic amplitude meaningful, bit 8 RAM register overrides, bit 9 persistence, bit 10 exact TIME_SYNC timestamps, bit 11 legacy raw register write 0x0602 built in (a bench build; never set in a release). Bits 4 to 7 are set only once the matching measurement (12) has validated the register; the raw fields are reported regardless.

### 6.2 GET_STATUS (0x5001)

Request: empty. Reply (34 bytes):

| Off | Type | Field | Units |
|---|---|---|---|
| 0 | u32 | uptime | ms (the event clock) |
| 4 | u32 | frequency | Hz |
| 8 | u8 | state | 0 receiving, nothing detected; 1 receiving, busy (8.2); 2 transmitting; 3 not used (was monitor; the firmware has no squelch); 4 reduced service |
| 9 | u8 | flags1 | bit 0 the chip's squelch detector open (a detector only, 8.2), bit 1 busy (8.2), bit 2 PTT pressed, bit 3 lock active, bit 4 TX allowed at this frequency, bit 5 TX latched (release needed), bit 6 level tone running, bit 7 late key pending |
| 10 | u8 | flags2 | bit 0 live params differ from stored, bit 1 RAM overrides active, bit 2 reserved, always 0 (was EEPROM overrides active; the stored table is retired), bit 3 LIVE_TX on, bit 4 persist in progress, bit 5 reserved (was memory-channel mode; the firmware has no memory channels), 0 |
| 11 | u8 | power | 0 low, 1 mid, 2 high |
| 12 | u8 | bandwidth | 0 wide, 1 narrow |
| 13 | u8 | busy detector level | 1 to 9 (BUSY_SQL_LEVEL; there is no squelch) |
| 14 | u16 | deviation in use | REG_40<11:0> for the current bandwidth |
| 16 | u16 | RSSI | raw REG_67<8:0>, 0.5 dB steps, dBm = raw / 2 - 160 |
| 18 | u8 | noise | REG_65<6:0> |
| 19 | u8 | glitch | REG_63<7:0> |
| 20 | u8 | AGC | REG_7E<15> in bit 7, REG_7E<14:12> in bits 2:0 |
| 21 | u8 | battery level | 0 empty (TX refused), 1 to 6, 7 over-voltage |
| 22 | u16 | battery | mV (10 mV resolution) |
| 24 | u16 | lock remaining | ms |
| 26 | u16 | TX time left | 100 ms units until TX timeout; 0xFFFF if not transmitting |
| 28 | u16 | busy age | ms since the last busy edge, saturating at 65535 |
| 30 | u16 | next event seq | |
| 32 | u8 | channel | 0xFF (the firmware has one operating channel and no memory channels or band slots) |
| 33 | u8 | TX timeout | s |

### 6.3 SUBSCRIBE (0x5002)

Request (10 bytes):

| Off | Type | Field |
|---|---|---|
| 0 | u32 | event mask, bit n = event 0x50C0 + n |
| 4 | u8 | options: bit 0 LIVE_TX (host has disabled the AIOC's RXIGNPTT), bit 1 PERSIST (store mask, options and heartbeat period as the power-on default) |
| 5 | u16 | heartbeat period, ms: 0 off, 100 to 60000 |
| 7 | u8 | RSSI stream period, ms: 0 off, 5 to 250 |
| 8 | u8 | RSSI stream batch, samples per frame: 1 to 20 |
| 9 | u8 | burst sample period, ms: 0 default (5), 2 to 50 |

Reply (8 bytes): `u16 next_seq`, `u16 oldest_seq` (oldest event still in the ring, equal to next_seq if empty), `u32 t_ms` (radio clock now).

Mask bits for events the radio does not support, and option bits other than 0 and 1, are ignored. The batch may be 0 only when the stream period is 0. PERSIST needs a valid settings block (else `EEPROM`); the default is written after the reply, like SET_PARAMS PERSIST. Errors: `RANGE` with the field offset as detail (5 heartbeat, 7 stream period, 8 batch, 9 burst period).

The live subscription is RAM. It returns to the stored default (normally nothing) at power-on and on every legacy hello. A persisted non-empty mask makes the radio send frames unprompted from power-on, which may confuse CHIRP and similar tools; use it only for dedicated stations. Heartbeat needs bit 6 and a period; the stream needs bit 5 and a period.

### 6.4 TIME_SYNC (0x5003)

Request: `u8 host_ref[8]` (opaque, echoed). Reply (21 bytes):

| Off | Type | Field |
|---|---|---|
| 0 | u8[8] | host_ref |
| 8 | u32 | rx_ms: when the firmware first saw the request complete |
| 12 | u16 | rx_us: sub-millisecond part, 0 to 999 |
| 14 | u32 | tx_ms: when the reply's first byte was queued (the frame header goes out first; with flag bit 0 the queue was empty and the byte left within microseconds) |
| 18 | u16 | tx_us |
| 20 | u8 | flags: bit 0 tx time exact (queue was empty) |

Offset estimate (host clock minus radio clock), with `t0` the host write time, `t3` the arrival of the reply's last byte, `Nq` and `Nr` the request and reply frame lengths in bytes: `up = t0 + Nq x 260.4 us`, `down = t3 - Nr x 256 us`, `offset = ((up - rx) + (down - tx)) / 2`, uncertainty about half of `(down - up) - (tx - rx)`, dominated by USB (1 ms frames). Keep the samples with the smallest residual. The radio clock is the MCU's internal RC oscillator, so hosts MUST also fit a rate (skew), for example from heartbeats (8.3); do not assume it is exact (M7).

### 6.5 GET_PARAMS (0x5004)

Request: `u8 flags` (bit 0 STORED: return the stored EEPROM value, or the default a blank would load, instead of the live one), then zero or more `u8 param_id` (none = all supported). Reply: `u8 flags`, then `(u8 id, value)` records in request order, each value in the size the parameter table gives. With STORED, RAM-only parameters are omitted, also when asked for by id. An unknown or repeated id gives `BAD_PARAM` (detail = id), a retired id (0x06, 0x07, section 7) `UNSUPPORTED` (detail = id). STORED values for FREQ_HZ, POWER and BANDWIDTH are those of the operating-channel block (0x1D58), or what a power-on would take when it is not in use (7). STORED always reflects the EEPROM as it is now, whoever wrote it: keypad and menu saves, SET_PARAMS and SAVE_PARAMS persistence, and legacy EEPROM writes.

### 6.6 SET_PARAMS (0x5005)

Request: `u8 flags` (bit 0 PERSIST, bit 1 REQUIRE_TX_OK, bit 2 DRY_RUN), then `(u8 id, value)` records, each id at most once.

Semantics:
- All records are validated first. Any failure rejects the whole command (BAD_PARAM, UNSUPPORTED for a retired id, RANGE, TX_BAND, NOT_PERSISTABLE) and changes nothing.
- DRY_RUN stops after validation and replies as if applied, without applying.
- Otherwise all values are applied together, then the receiver is set up once (`RADIO_ConfigureSquelchAndOutputPower` if frequency, power or the busy detector level changed, then `RADIO_SetupRegisters`). A receiver set-up forces busy closed (`CD` cause RETUNE); the audio stays open. Values that only matter at key-up (deviation, PA delays) take effect at the next key-up.
- A transmission never sees a change: the frame's lock has already ended any transmission (5.3).
- In reduced service (critical battery) a change that needs the receiver set up (frequency, power, bandwidth, BUSY_SQL_LEVEL, BUSY_SQL_RAW, AGC_FIX, AFC, RX gains) is refused with `STATE` (detail = the lowest such id); other changes apply.
- FREQ_HZ keeps the live power and bandwidth; if the step does not hold the frequency, the largest step that does is chosen, so the keypad steps from it.
- BUSY_RSSI_CLOSE must not exceed BUSY_RSSI_OPEN after the command (`RANGE`, detail = the one that was sent, CLOSE if both).
- PERSIST writes the changed values to EEPROM after the reply is sent, one 8-byte block per main-loop pass, never during a transmission (a press defers the rest). Each block takes about 8 ms and can delay a key-up by up to 10 ms, so persist only when idle.

Reply: `u8 result` (bit 0 TX allowed at the resulting frequency, bit 1 persist queued, bit 2 receiver retuned), then `(u8 id, value)` read back for every id sent, in request order.

### 6.7 SAVE_PARAMS (0x5006)

Request: `u8 op`: 0 SAVE (persist every live persistable parameter that differs from its stored value), 1 REVERT (reload all live parameters from EEPROM, as at power-on, and drop RAM-only settings). Reply: `u32 mask` (bit n = parameter id n written or reverted, that is, whose value differed). SAVE needs a valid settings block (else EEPROM); write rules as 6.6. REVERT is refused with `STATE` while a persist is still being written, and in reduced service. Any other op: `RANGE`, detail 0.

### 6.8 LEVEL_TONE (0x5007)

A receive level-set aid: the BK4819 tone generator (REG_70 tone 1 enable and gain, REG_71 frequency word) routed to the radio's own audio output (REG_47<11:8> = 2, "tone out for Rx"), so the operator can set the analogue volume knob against the TNC's level meter.

Request (7 bytes):

| Off | Type | Field |
|---|---|---|
| 0 | u16 | frequency, Hz, 100 to 5000 |
| 2 | u8 | mode: 0 deviation-equivalent, 1 raw gain |
| 3 | u16 | level: mode 0 deviation in Hz, 0 to 8000; mode 1 REG_70<14:8> code, 0 to 127 |
| 5 | u16 | duration, ms: 1 to 60000; 0 stops a running tone |

Reply (3 bytes): `u8 gain code used`, `u16 REG_71 word used` (`round(f x 10.32444)` for the K5's 26 MHz crystal).

While the tone runs the audio output carries only the tone instead of the receive audio: the firmware selects the tone as the AF source (REG_47 = 0x6240) with the speaker amplifier (the K1 audio out) on, and writes REG_70, REG_71 and REG_47 again after every receive set-up. Busy events continue. It ends (`TONE_END`) when the duration elapses, on a stop request, on a new tone, on a PTT press (before key-up) and on a retune; the AF output then returns to the receive audio, which is always open. Mode 0 needs the calibration byte (7, v2 block 0x1D6F) and returns UNSUPPORTED without it; the firmware then uses `code = round(cal x deviation / 3000)`, clamped to 127, which assumes a linear law and is provisional (M8 found it compressive, Q4). A stop request (duration 0) replies code 0 and word 0. Refused with STATE in reduced service. Errors: `RANGE` with the field offset as detail (0 frequency, 2 mode, 3 level, 5 duration).

### 6.9 REG_READ (0x5008)

Request: `u8 first`, `u8 count` (1 to 64, `first + count <= 0x80`), `u8 flags` (bit 0 include 0x5F; otherwise 0x5F, the FSK FIFO, is not read and returns 0). Reply: `u8 first`, `u8 count`, `u16 value[count]`. Two requests dump the whole chip. Errors: `RANGE`, detail 1 for the count, 0 for the end past 0x7F.

### 6.10 REG_WRITE (0x5009)

Request: `u8 n` (1 to 16), then n x `(u8 reg, u16 value)`. Registers `REG_OVERRIDE` refuses (0x00, 0x30, 0x33, 0x36, 0x37, 0x38, 0x39, 0x3B, 0x3C, above 0x7F) reject the whole command with REFUSED. Written in order; read back after the last write. `n` outside 1 to 16: `RANGE` detail 0; a body that is not `1 + 3n` bytes: `BAD_LENGTH`. Reply: `u8 n`, n x `(u8 reg, u16 read-back)`. Registers the firmware manages (7D, 40, 47, 48, 7E, 2B, 43, 31 and the squelch set) are rewritten at the next set-up; use parameters or `REG_OVERRIDE` for those. REG_WRITE is in every build; the unrestricted legacy 0x0602 only in bench builds (caps bit 11).

### 6.11 REG_OVERRIDE (0x500A)

A RAM-only override table for safe trials. In its phase (TX: after the TX set-up at every key-up; RX: after every receive set-up) each entry sets its register to `(value & and) | or` after all of the firmware's own writes, so it wins. Refused registers: 0x00, 0x30, 0x33, 0x36, 0x37, 0x38, 0x39, 0x3B, 0x3C and anything above 0x7F. A REG_40 result is clamped to 0xA7F. The table never outlives its bound or a reboot: there is no stored table (until 30 September 2026 there was one at EEPROM 0x1D10 to 0x1D4F, with ops 3 and 4 to write it; they are retired).

Request:

| Off | Type | Field |
|---|---|---|
| 0 | u8 | op: 0 LIST, 1 ADD, 2 CLEAR; 3 and 4 (COMMIT, CLEAR_EEPROM) are retired and reply `UNSUPPORTED`, detail 0 |
| 1 | u16 | expiry, s: 0 none (ADD only; restarts the timer) |
| 3 | u8 | expiry, key-ups: 0 none (ADD only) |
| 4 | u8 | n entries (ADD only, else 0) |
| 5 | n x 6 | `u8 phase` (bit 0 TX, bit 1 RX), `u8 reg`, `u16 and`, `u16 or` |

At most 8 RAM entries (more: `RANGE`, detail 4). An entry's phase must be 1 to 3 (`RANGE`, detail = the offset of its phase byte in the request after the tag); a refused register gives `REFUSED` (detail = register); the whole command is rejected. Ops other than ADD must carry n = 0 (`RANGE`, detail 4); any op above 4: `RANGE`, detail 0. ADD takes effect at once for RX-phase entries (the RX-phase overrides are re-applied) and at the next key-up for TX-phase entries. CLEAR sets the receiver up again (not in reduced service). An expiry that falls during a transmission takes effect when it ends. When either expiry is reached the RAM table is cleared, the receiver set up again, and `OVERRIDE_EXPIRED` sent: a bad trial cannot outlive its bound. Reply: `u8 n_ram`, `u16 expiry s left` (0xFFFF none), `u8 key-ups left` (0xFF none), n_ram x 6 bytes, `u8 n_eeprom`, always 0 (kept so the layout is unchanged).

### 6.12 EVENT_REPLAY (0x500B)

Request: `u16 from_seq`. The radio re-sends stored events still in the ring with seq at or after from_seq, oldest first, flag REPLAY set, then replies: `u16 first_sent`, `u8 count_sent`, `u16 oldest_available`, `u16 next_seq`. If from_seq is older than the ring, the gap is visible from `oldest_available`.

So that the reply stays inside the host's 100 ms timeout, one request re-sends at most 256 bytes of event frames (about 5 burst reports or 13 busy edges); a host that wants more asks again from `first_sent + count_sent`. Events not delivered yet (held back, 8.1) are not replayed: they follow through normal delivery. `first_sent` is `next_seq` when nothing was sent.

### 6.13 GET_COUNTERS (0x500C)

Request: `u8 flags` (bit 0 clear after reading). Reply: `u8 n`, then n x `u32`, in this order (later versions only append): frames accepted, frames with bad CRC or footer or truncated, frames dropped (oversize; a DMA ring overrun cannot be detected), non-OK replies, events stored, events lost (overwritten before sending), events deferred, transmissions, TX timeouts, TX refused, busy opens, late keys, ephemeral frames dropped (output queue full), EEPROM blocks written.

## 7. Parameters

Ids for `GET_PARAMS` and `SET_PARAMS`. "Stored" is the EEPROM home when persisted; RAM-only parameters return to their power-on value at reboot.

| Id | Name | Type | Range | Default | Stored |
|---|---|---|---|---|---|
| 0x01 | FREQ_HZ | u32 | 50 000 000 to 600 000 000, multiple of 10 | 144 800 000 | 0x1D58 (10 Hz units) |
| 0x02 | POWER | u8 | 0 low, 1 mid, 2 high | 0 | 0x1D5C |
| 0x03 | BANDWIDTH | u8 | 0 wide, 1 narrow | 0 | 0x1D5D |
| 0x04 | DEV_WIDE | u16 | 0 to 0x0A7F | 0x0856 | 0x1D04 |
| 0x05 | DEV_NARROW | u16 | 0 to 0x0A7F | 0x0756 | 0x1D06 |
| 0x06 | (retired: MIC_GAIN) | | | | the mic gain is fixed at the maximum (31), since the whole range moved the deviation by only about 0.5 dB; GET_PARAMS and SET_PARAMS reply `UNSUPPORTED`, detail 0x06; 0x1D03 is reserved and ignored |
| 0x07 | (retired: SQUELCH) | | | | the firmware has no squelch; GET_PARAMS and SET_PARAMS reply `UNSUPPORTED`, detail 0x07 |
| 0x08 | RX_GAIN | u8 | 0 to 63 | factory calibration | 0x1D08 |
| 0x09 | RX_DAC_GAIN | u8 | 0 to 15 | 15 | 0x1D09 |
| 0x0A | TX_TIMEOUT_S | u8 | 5, 10, 15, 20, 30, 60, 120 | 30 | 0x1D02 (as index) |
| 0x0B | PTT_PRESS_MS | u8 | 1 to 40 | 5 | 0x1D50 |
| 0x0C | PTT_RELEASE_MS | u8 | 2 to 40 | 5 | 0x1D51 |
| 0x0D | PA_ENABLE_DELAY_MS | u8 | 1 to 20 | 1 | 0x1D52 |
| 0x0E | PA_BIAS_DELAY_MS | u8 | 0 to 20 | 2 | 0x1D53 |
| 0x0F | SERIAL_LOCK_MS | u16 | 0 to 1500, multiple of 10 | 20 | 0x1D61 (/10) |
| 0x10 | BUSY_SOURCE | u8 | bit 0 the chip's squelch detector, bit 1 RSSI; 1 to 3 | 1 | 0x1D62 |
| 0x11 | BUSY_RSSI_OPEN | u16 | raw RSSI 0 to 511 | 110 (-105 dBm) | 0x1D64 |
| 0x12 | BUSY_RSSI_CLOSE | u16 | raw RSSI, at most OPEN | 104 (-108 dBm) | 0x1D66 |
| 0x13 | BUSY_HANG_MS | u8 | 0 to 250 | 20 | 0x1D63 |
| 0x14 | BUSY_SQL_RAW (was SQL_RAW) | 6 bytes | the squelch detector's thresholds: RSSI open, RSSI close (0 to 255), noise open, noise close (0 to 127), glitch open, glitch close (0 to 255) | from the level table | RAM only |
| 0x15 | AGC_FIX | u8 | 0xFF auto, 0 to 7 fixed index (REG_7E<14:12> code) | 0xFF | RAM only |
| 0x16 | AFC | u8 | 0 off, 1 on | 1 | RAM only |
| 0x17 | BACKLIGHT | u8 | 0 off to 7 on | 3 | 0x1D0A |
| 0x18 | KEY_LOCK | u8 | 0, 1 | 0 | 0x1D0C |
| 0x19 | BUSY_SQL_LEVEL | u8 | 1 to 9: the row of the factory squelch tables the chip's squelch detector uses | 1 | 0x1D01 (was the squelch level; 0 there means 1) |

Notes:
- FREQ_HZ: the radio must be able to receive it (inside the band table, and not 350 to 400 MHz unless the band is enabled at 0x0F45). The firmware has one operating channel, no memory channels or band slots: frequency, power, bandwidth and step live in an 8-byte block at 0x1D58 in the settings family (`0x1D58` u32 frequency in 10 Hz units, `0x1D5C` power, `0x1D5D` bandwidth, `0x1D5E` step index, `0x1D5F` reserved), used only with a valid settings block. REQUIRE_TX_OK rejects a frequency the TX band plan forbids; otherwise the reply's result bit 0 says whether TX would be allowed. PERSIST of any of the three writes the block with all of them and the step, as the keypad does, and like every other stored parameter needs a valid settings block (else `EEPROM`).
- When the block is not in use (a radio coming from another firmware or from the channel-memory builds), the frequency the old upstream layout had in use (channel indices at 0x0E80 and the record they point at) is taken once if receivable, else 144.800 MHz; with a valid settings block it is then written to 0x1D58 and the old layout is never read again.
- There is no squelch: receive audio is always open (the AF output carries the FM demodulator output whenever the radio is not transmitting, and the speaker amplifier is on). The chip's squelch result is only a carrier detector for the busy events (8.2); BUSY_SQL_LEVEL and BUSY_SQL_RAW set its thresholds and never mute anything.
- BUSY_SQL_RAW overrides the thresholds from the level table and survives retunes; setting BUSY_SQL_LEVEL or rebooting drops it. Reading it returns the thresholds in use.
- AGC_FIX and AFC are diagnostics.

### 7.1 v2 EEPROM block (0x1D60 to 0x1D6F)

Used only when the v1 settings block (0x1D00) is valid and 0x1D60 holds its layout version. A single byte out of range means "default". The first menu save over foreign data at 0x1D00 blanks this block too, with the timing and operating blocks.

| Address | Content | Blank means |
|---|---|---|
| 0x1D60 | layout version (1) | block unused |
| 0x1D61 | serial lock, 10 ms units, 0 to 150 | 2 (20 ms) |
| 0x1D62 | busy source, 1 to 3 | 1 |
| 0x1D63 | busy hang, ms, 0 to 250 | 20 |
| 0x1D64 | u16 busy RSSI open | 110 |
| 0x1D66 | u16 busy RSSI close | 104 |
| 0x1D68 | u32 default event mask | 0 (0xFFFFFFFF) |
| 0x1D6C | u16 default heartbeat, ms | 0 |
| 0x1D6E | default subscribe options (bit 0 LIVE_TX) | 0 |
| 0x1D6F | tone calibration: gain code equivalent to 3 kHz deviation at 1 kHz | none (mode 0 unsupported) |

The calibration area 0x1E00 to 0x1FFF is never written by anything in this protocol.

## 8. Events

| Id | Bit | Name | Stored |
|---|---|---|---|
| 0x50C0 | 0 | CD (busy edge) | yes |
| 0x50C1 | 1 | RX_BURST | yes |
| 0x50C2 | 2 | TX_START | yes |
| 0x50C3 | 3 | TX_END | yes |
| 0x50C4 | 4 | TX_REFUSED | yes |
| 0x50C5 | 5 | RSSI_STREAM | ephemeral |
| 0x50C6 | 6 | HEARTBEAT | ephemeral |
| 0x50C7 | 7 | BATTERY | yes |
| 0x50C8 | 8 | PARAMS_CHANGED | yes |
| 0x50C9 | 9 | EVENTS_LOST | yes (always sent when any stored event is subscribed) |
| 0x50CA | 10 | TONE_END | yes |
| 0x50CB | 11 | OVERRIDE_EXPIRED | yes |
| 0x50CC | 12 | BOOT | yes (only via a persisted mask) |

### 8.1 Delivery

- Stored events go into a ring that holds at least 16 of the largest events (this firmware: 20 slots of 36 bytes) and out through the output queue in seq order, each only when the queue is empty (its bytes have all gone to the UART's 8-byte FIFO), so a PTT press loses at most one event frame at the AIOC; the rest stay held back in the ring. Ephemeral events are never stored and are the first dropped when the queue is short of space, while stored events wait, and while events are held back.
- Priority in the output queue: replies, then stored events, then ephemeral events.
- Deferral (default): the radio starts no event frame while the PTT line reads low or the radio is transmitting, and resumes 2 ms after the release; such events carry DEFERRED. Replies are never deferred. With LIVE_TX the radio sends events during transmissions too.
- If the ring overwrites an event that was never sent, the radio sends `EVENTS_LOST` once it can.

### 8.2 Busy (carrier detect)

`busy` is the OR of the enabled sources (BUSY_SOURCE):
- squelch detector: the BK4819 squelch result, REG_0C<1>, polled every 1 ms in receive, whether or not anything is subscribed, with the thresholds of BUSY_SQL_LEVEL or BUSY_SQL_RAW. It is a detector only: the firmware has no squelch, the chip's squelch interrupts are off, and receive audio is always open. The green LED shows busy.
- RSSI: REG_67 sampled every 2 ms in receive; opens at the first sample at or above BUSY_RSSI_OPEN, closes after BUSY_HANG_MS continuously below BUSY_RSSI_CLOSE.

Busy is forced closed when a transmission starts and when the receiver is set up again (retune); it is re-evaluated when receive resumes. A burst is one busy interval.

### 8.3 Layouts (after the 7-byte event header)

**CD 0x50C0** (7 bytes). t_ms = when the firmware saw the edge. When busy is forced closed (a key-up or a retune) the registers are not read: RSSI is the last value sampled, noise and glitch are 0, and the cause carries the source bits that were open.

| Off | Type | Field |
|---|---|---|
| 0 | u8 | busy: 1 open, 0 closed |
| 1 | u8 | sources now: bit 0 squelch detector open, bit 1 RSSI busy |
| 2 | u8 | cause: bit 0 squelch detector edge, bit 1 RSSI edge, bit 2 retune, bit 3 transmission started |
| 3 | u16 | RSSI raw |
| 5 | u8 | noise |
| 6 | u8 | glitch |

**RX_BURST 0x50C1** (29 bytes), sent after the closing `CD`. t_ms = close time. Samples every burst sample period while busy, the first at the open (the values in the opening `CD`); means are `floor(sum / n)`.

| Off | Type | Field |
|---|---|---|
| 0 | u32 | t_open_ms |
| 4 | u32 | duration, ms |
| 8 | u16 | samples |
| 10 | u16 | RSSI mean, raw |
| 12 | u16 | RSSI max |
| 14 | u16 | RSSI min |
| 16 | u8 | noise mean |
| 17 | u8 | noise min |
| 18 | u8 | glitch mean |
| 19 | u8 | glitch max |
| 20 | u16 | AF amplitude mean, REG_64<14:0> (0xFFFF not sampled) |
| 22 | u16 | AF amplitude max |
| 24 | u8 | AGC at open (encoding as status) |
| 25 | u8 | AGC at close |
| 26 | s16 | frequency error, Hz (0x7FFF not available, Q1) |
| 28 | u8 | end cause (as CD cause) |

**TX_START 0x50C2** (15 bytes). t_ms = RF ready: the PA bias and bias delay done (end of `RADIO_SetTxParameters`).

| Off | Type | Field |
|---|---|---|
| 0 | u32 | t_press_ms: first tick counted towards the press |
| 4 | u32 | frequency, Hz |
| 8 | u8 | power |
| 9 | u8 | bandwidth |
| 10 | u16 | deviation register used |
| 12 | u16 | lock delay, ms (5.3) |
| 14 | u8 | flags: bit 0 busy at the press (collision risk), bit 1 late key |

**TX_END 0x50C3** (19 bytes). t_ms = carrier off (PA bias 0, REG_30 = 0). Sent once the receiver is set up.

| Off | Type | Field |
|---|---|---|
| 0 | u32 | t_start_ms (RF ready) |
| 4 | u32 | t_release_ms: first tick counted towards the release (= t_ms for other reasons) |
| 8 | u32 | t_rx_ready_ms: receiver set-up complete |
| 12 | u8 | reason: 0 PTT released, 1 TX timeout, 2 serial frame, 3 other |
| 13 | u16 | mic amplitude mean, REG_64<14:0> sampled every 10 ms (0xFFFF none) |
| 15 | u16 | mic amplitude max |
| 17 | u16 | mic samples |

**TX_REFUSED 0x50C4** (7 bytes). t_ms = decision time.

| Off | Type | Field |
|---|---|---|
| 0 | u32 | t_press_ms |
| 4 | u8 | reason: 1 LOCK, 2 TX_BAND, 3 BATTERY_EMPTY, 4 OVER_VOLTAGE, 5 REDUCED_SERVICE, 6 LATE (could not key within LATE_KEY_MAX_MS of the press) |
| 5 | u16 | detail: lock ms remaining for LOCK, ms since the press edge for LATE, else 0 |

**RSSI_STREAM 0x50C5** (2 + 4 x count). t_ms = time of the last sample; sample i was taken at `t_ms - (count - 1 - i) x period`. Receive only; paused while transmitting.

| Off | Type | Field |
|---|---|---|
| 0 | u8 | period, ms |
| 1 | u8 | count |
| 2 | count x 4 | `u16 RSSI raw`, `u8 noise`, `u8 glitch` |

**HEARTBEAT 0x50C6** (12 bytes). t_ms and t_us = when queued; with TIME_EXACT, that is when the first byte left. Arrival time minus `t + frame_bytes x 256 us`, minimum-filtered, gives offset; a fit over time gives skew, without any host-to-radio traffic.

| Off | Type | Field |
|---|---|---|
| 0 | u16 | t_us, 0 to 999 |
| 2 | u8 | flags1 (as status) |
| 3 | u8 | state |
| 4 | u16 | RSSI raw (0 when not receiving) |
| 6 | u16 | battery, mV |
| 8 | u16 | busy time since the previous heartbeat, ms (channel occupancy) |
| 10 | u16 | lock remaining, ms |

**BATTERY 0x50C7** (4 bytes), on a change of class: `u8 class` (0 normal, levels 2 to 6; 1 low, level 1; 2 empty, level 0, TX refused; 3 over-voltage, level 7, TX refused), `u8 level`, `u16 mV`.

**PARAMS_CHANGED 0x50C8** (5 bytes): `u8 source` (0 keypad or menu, 1 EEPROM reload after legacy writes, 2 serial command, 3 boot), `u32 mask` (bit n = parameter id n). Lets a client follow an operator using the radio's own keys.

**EVENTS_LOST 0x50C9** (4 bytes): `u16 first_lost_seq`, `u16 count`.

**TONE_END 0x50CA** (1 byte): `u8 reason` (0 duration elapsed, 1 stopped, 2 PTT, 3 retune, 4 replaced).

**OVERRIDE_EXPIRED 0x50CB** (2 bytes): `u8 reason` (0 time, 1 key-ups), `u8 entries reverted`.

**BOOT 0x50CC** (3 bytes): `u16 protocol version`, `u8 reset cause` (0 unknown, 1 power-on, 2 software reboot; this firmware always sends 0, the reset-cause register is not identified). With a persisted mask containing PARAMS_CHANGED, a `PARAMS_CHANGED` with source 3 (boot) and every supported id follows it.

## 9. Firmware requirements

1. Output through a queue of at least 512 bytes, drained by the UART TX interrupt or a DMA channel, legacy replies included. This firmware uses the TX FIFO interrupt (lowest priority, enabled only while there is more to send, switching itself off if it ever fires without room in the FIFO), a refill from the main loop on every pass and as soon as a frame is queued, and the 1 ms SysTick handler as a fallback. The tick alone was measured on the bench at 1 to 2.3 bytes/ms, against 3.9 on the line (the transmit FIFO takes about 2 bytes, not 8). `UART_Send`'s busy wait goes. Interrupts are never disabled for more than 50 us (v1 disables them around the whole command handler, which stalls the 1 ms PTT tick for the length of a reply: 136 bytes is 35 ms).
2. Frames parsed on every main-loop pass, all complete frames processed, reply queued within 5 ms of the last byte.
3. A 1 ms clock `u32 g_ms` incremented in the SysTick handler; sub-ms from `SysTick->VAL` (48 counts per us). All event times use it.
4. BK4819 access only from the main loop (the bit-banged SPI is not re-entrant). The 1 ms tick sets a flag; the main loop polls REG_0C each ms, and does the RSSI busy (2 ms), burst, stream and TX mic sampling on their periods. At 5 ms burst sampling, four register reads cost about 8% CPU while busy.
5. The lock counts in ms and follows 5.3 exactly; the 280 us window and debounce are unchanged; the press logic runs during the lock.
6. The settings reload after legacy EEPROM writes has its own 1.0 s quiet timer and never runs during a transmission.
7. EEPROM writes: one 8-byte block per main-loop pass, never during a transmission, deferred while a press is pending; never 0x1E00 and up.
8. Stored events are kept serialized in the ring, so replay re-sends the same bytes with REPLAY set (and a fresh CRC).
9. Budget: at most 8 KB of flash (about 38 KB free) and 2 KB of RAM (about 13 KB free). Measured: the full implementation costs about 12.7 KB of flash (23 520 to 36 188 bytes) and 2.2 KB of RAM (bss 2116 to about 4300 bytes), over the estimate but well inside what is left. With the memory channels removed and the review fixes the image is 35 316 bytes (26 124 left) and bss 4092 bytes.

## 10. Frame and MCU budget

The link carries about 3900 bytes/s (256 us per byte from the radio, 260 us to it). Overhead is 8 bytes of framing plus the 4-byte inner header; replies add 4, events 7.

| Frame | Bytes | Time on the wire |
|---|---|---|
| CD | 26 | 6.7 ms |
| RX_BURST | 48 | 12.3 ms |
| TX_START | 34 | 8.7 ms |
| TX_END | 38 | 9.7 ms |
| HEARTBEAT | 31 | 7.9 ms |
| GET_STATUS reply | 50 | 12.8 ms |
| RSSI_STREAM, 10 samples | 61 | 15.6 ms |

A 1 s heartbeat uses under 1% of the link; a 10 ms RSSI stream in batches of 10 uses about 16%. Each burst costs two `CD` frames and a report, about 100 bytes, so ten bursts a second use about a quarter of the link. The AIOC forwards radio bytes to USB on its receiver timeout, adding about 1 to 2 ms. So a `CD` open reaches the host about 8 to 10 ms after the chip decides.

## 11. Rationale

- **Push, not poll.** Every host-to-radio frame shares the PTT wire and costs a lock, so everything the TNC needs during operation (busy edges, burst reports, TX timing, clock) comes unsolicited. A TNC can run for hours after one `SUBSCRIBE` without sending another byte.
- **Short lock, and the host told the truth.** The window is the real protection against serial data keying the radio; the lock only covered cases the window already handles. 20 ms keeps a margin for host timing slop. `lock_ms` in every reply turns "when may I key?" into arithmetic, and the late-key and refusal rules replace the silent 1.5 s late key the bench found.
- **Late key up to 30 ms, else refuse.** A 20 ms late key is inside any sensible TXDELAY and loses nothing, so refusing it would throw away good frames. A longer wait would eat data, so it is refused and reported, and the TNC retries.
- **Busy from the chip, not the audio.** The squelch result and RSSI are available within a millisecond, independent of modem and baud rate. CSMA and p-persistence stay in the host; the radio only supplies a trustworthy, timestamped busy state, the occupancy per heartbeat, and a "busy at the press" flag on each transmission.
- **Ephemeral versus stored events.** Heartbeats and the RSSI stream are worthless late, so they are never stored and never replayed; they carry the next seq so they still reveal gaps.
- **TLV parameters.** Atomic multi-field sets, read-back of what was really applied, partial reads, and new parameters without new commands. RAM by default; EEPROM only on request, because persisting on every QSY wears the EEPROM and blocks for milliseconds.
- **Trial overrides that expire.** A register experiment that kills receive cannot outlive its time or key-up bound or a reboot. A good one belongs in the firmware source, not in EEPROM.
- **v2 marker in the hello reply.** Every host sends a hello first anyway, so identification costs no extra round trip, and the challenge field is unused by every tool when the AES flag is clear.
- **Real CRC on new frames only.** Legacy replies stay byte-identical; new frames can be checked, which matters because the AIOC truncates frames when PTT is asserted.
- **No serial keying in v2.** The case for it: an atomic listen-before-talk (the radio checks busy and keys in the same millisecond, with no USB race), exact-length timed transmissions for turnaround tests, and radios on cables without a HID PTT. The case against, which wins for now: the AIOC's HID PTT already works, has a host watchdog, and is what pdn-soundmodem uses; a second keying path doubles the failure modes; audio still comes from the AIOC with no shared clock; and v1's "any frame ends TX" rule would have to be broken for the unkey command. If it is ever added, at 0x5020: disabled unless an EEPROM flag enables it, a mandatory duration of at most 10 s and at most the TX timeout, one shot, ended by any other frame or PTT edge, optional refuse-if-busy, reported through `TX_START` and `TX_END` like any other transmission.

## 12. Open questions and wanted measurements

Open questions:
- **Q1** Does the BK4819 expose a frequency-error (AFC) estimate? The vendor list documents none. REG_0D/0E (frequency scan result, 10 Hz units) measures a carrier but needs scan mode, which interrupts reception. Until found, `RX_BURST` reports 0x7FFF.
- **Q2** Does REG_7E<14:12> read back the live AGC index in auto mode, or only the fix index written?
- **Q3** What REG_64 and REG_6F measure in receive and in transmit (the egzumer firmware uses REG_64 as the TX mic level), and their scale against deviation and AIOC drive.
- **Q4** Tone generator: gain law of REG_70<14:8> (linear or dB), whether REG_48 scales it like received audio, and the code equivalent to 3 kHz deviation (needs the reference transmitter).
- **Q5** The radio clock's rate error and drift (internal RC oscillator).
- **Q6** Is it safe and sufficient to clear RXIGNPTT on the AIOC with the K1 wiring (LIVE_TX)?
- **Q7** Does anything the AIOC does hold the contact low for 5 ms or more (port open, line coding, USB events, reset)?
- **Q8** Squelch decision latency (REG_4E open delay 0) and the best busy sources and thresholds for packet.
- **Q9** Is 20 ms the right default lock, and 30 ms the right late-key limit, once M3 and M6 are in?

Measurements planned on the bench radio with the v1 firmware (2951c48, transmit and register pokes, no flashing):

| # | Measurement | Settles |
|---|---|---|
| M1 | Done: lock 1.0 to 1.5 s after the last frame, and a press during the lock keys late when it expires. Worth one more run: a press that ends before the lock expires should give no RF at all. | 1c |
| M2 | AIOC 0x60 = 0x00000100 in RAM (RXIGNPTT off). Send 0x0529 and assert HID PTT 1 to 3 ms later (the lock stops RF). Does the reply arrive intact? Repeat with the default 0x00010100: expect it lost or truncated. Restore afterwards. | Q6, LIVE_TX |
| M3 | HID key for 4 s; at 1.0 s send one 0x0527. Expect RF off within about 5 ms and to stay off; read AIOC 0xD0 bit 16 to confirm PTT1 stays released after the bytes. Then send a fresh HID "on": expect RF after the lock. | 1a, 5.4 |
| M4 | With RF watched: open and close the CDC port 50 times, change baud 9600 and back 50 times, toggle DTR and RTS, unplug and replug the AIOC. Expect no RF. Optionally a 10 ms BREAK (expect a short key-up into the load: shows the window's limit). | Q7, 1c |
| M5 | Send 1000 bytes of 0x00 at 9600 and at 19200 baud (bounded, dummy load). Expect RF at 9600, maybe at 19200. | 1c, the host rule |
| M6 | 1000 x 0x0527 round trips, host timestamps: latency distribution (expect 0 to 10 ms from the 10 ms slice plus USB). | the 5 ms reply rule, TIME_SYNC accuracy |
| M7 | Scope the radio TX data line (2.5 mm ring) during a 128-byte 0x051B reply: bit period gives the RC clock error. | Q5 |
| M8 | Squelch 9, no signal. Via 0x0602: REG_71 = 0x2854 (1 kHz), REG_70 = 0x8000 or (g << 8), REG_47 = 0x6240. Record the AIOC input level and frequency for g = 0, 8, ... 127, at the default REG_48 and at RX gain 40 and DAC gain 8. Check the tone stays until a squelch event. Restore with 0x05DD. | Q4 (all but the deviation equivalence) |
| M9 | Idle reads, 100 samples each: REG_0C<1> at squelch 0 and 9, REG_7E, REG_64, REG_6F, REG_65, REG_63. Time a full 0x00 to 0x7F sweep via 0x0601 (skip 0x5F). | Q2 baseline, busy |
| M10 | TX timeout 5 s (EEPROM 0x1D02 = 0), key 7 s: RF should end between 5.0 and 5.5 s; re-key only after release. | 5.3 rule 4 |

Later, with the reference transmitter: squelch and RSSI busy latency against level (Q8), AF amplitude and AGC against level and deviation (Q2, Q3), carrier offset against any candidate register (Q1), and the tone's deviation equivalence (Q4). TX mic amplitude (Q3, TX side) needs v2 firmware, because host frames end a transmission.

Results reported on 29 September (2951c48): M2, a reply sent while HID PTT was held was lost 4 of 4 times with the AIOC default 0x60 = 0x00010100 and arrived intact 4 of 4 with 0x00000100, so LIVE_TX works once the host clears RXIGNPTT (Q6). M3, one 0x0527 sent 1.2 s into a 4 s key-up ended RF for the rest of it, and AIOC 0xD0 bit 16 read 0 afterwards (1a confirmed). M4, 50 port opens and closes, 50 baud changes and 100 DTR/RTS toggles gave no RF (Q7, partly). M5, 1000 zero bytes keyed the radio for 0.455 s at 9600 baud and 0.120 s at 19200, never at 38400: the host rule is necessary. M6, legacy reply latency 9.9 to 22.1 ms, bimodal at 10 and 20 ms (the v1 10 ms slice); a full 0x00 to 0x7F sweep via 0x0601 took 2.05 s. M8, with the squelch open, REG_71 = 0x2854, REG_70 = 0x8000 or (g << 8), REG_47 = 0x6240 gave a clean 1 kHz tone replacing the receiver audio: g = 16 -13.0 dBFS, 64 -4.8 dBFS, 127 +0.6 dBFS (clipping), so the law is compressive (x2 about +5.4 dB, x4 about +8.2 dB); with the squelch closed nothing reached the AIOC while REG_47 read 0x6040 (AF muted), which is why LEVEL_TONE selects the AF source itself. M9, idle: REG_0C 0x0280 (bit 1 clear, squelch closed), REG_7E 0x37C0, REG_64 0x0097, REG_6F 0x1B5B constant; REG_65 noise-like; REG_63 wanders widely; REG_67 tracks RSSI; REG_0D 0x8000, REG_0E 0. M10, a 5 s TX timeout with a 7 s key gave 4.995 s of RF and no re-key while held. A robustness finding: a frame cut by a key-up 2 ms after it was written left v1 deaf to hellos for a while; v2 drops truncated frames (2).

## 13. Implementation status (firmware)

Everything in sections 2 to 9 is implemented, with the points below settled by the implementation (the text above has been amended to match). Unimplemented request ids reply `UNKNOWN_CMD`; an absent capability replies `UNSUPPORTED`.

| Feature | Status |
|---|---|
| Output queue, frames handled every main-loop pass with interrupts on, parser resynchronisation (2, 9.1, 9.2) | implemented; queue drained from the SysTick tick |
| Legacy commands, PKT2 marker, subscription reset on hello, 1.0 s reload timer (2.1, 3) | implemented, byte-compatible |
| Serial PTT lock in ms, late key up to 30 ms, refusal and latch, lock_ms in every reply (5.3) | implemented |
| GET_INFO, GET_STATUS, SUBSCRIBE, TIME_SYNC, GET_COUNTERS | implemented |
| GET_PARAMS, SET_PARAMS, SAVE_PARAMS, all 23 parameters (0x01 to 0x19 except the retired 0x06 and 0x07), v2 EEPROM block | implemented |
| REG_READ, REG_WRITE, REG_OVERRIDE with expiry (RAM only) | implemented |
| Stored register override table (30 September) | removed: REG_OVERRIDE COMMIT and CLEAR_EEPROM reply UNSUPPORTED, GET_STATUS flags2 bit 2 is always 0, EEPROM 0x1D10 to 0x1D4F is neither read nor written |
| LEVEL_TONE | mode 1 (raw) implemented; mode 0 replies UNSUPPORTED until the calibration byte 0x1D6F is set, and its law is provisional |
| Events: CD, RX_BURST, TX_START, TX_END, TX_REFUSED, RSSI_STREAM, HEARTBEAT, BATTERY, PARAMS_CHANGED, EVENTS_LOST, TONE_END, OVERRIDE_EXPIRED, BOOT | implemented |
| Deferral, LIVE_TX, sequence numbers, timestamps and flags, EVENT_REPLAY | implemented |
| RX_BURST frequency error | always 0x7FFF (Q1) |
| BOOT reset cause | always 0 |
| GET_INFO caps bits 4 to 7 (validated measurements) | clear |
| 0x5020 serial keying | not implemented, reserved; replies UNKNOWN_CMD |
| Squelch (30 September) | removed: receive audio always open; parameter 0x07 retired (UNSUPPORTED); the chip's squelch result kept as the busy detector, level 0x19 and thresholds 0x14 |
| Mic gain (30 September) | fixed at the maximum; parameter 0x06 retired (UNSUPPORTED), EEPROM 0x1D03 ignored |

Points settled by the implementation, beyond the amendments above:
- PARAMS_CHANGED with source 0 (keypad or menu) comes from comparing every parameter with its last reported value every 500 ms, so it follows anything the operator changes, up to 0.5 s late.
- The event deferral also covers a press that is still being debounced, and resumes 2 ms after the last millisecond in which PTT was asserted or the radio transmitted.
- EEPROM persistence (6.6) is also deferred while the PTT rules have a key-up pending.
- The stored view (GET_PARAMS STORED, GET_STATUS flags2 bit 0, SAVE_PARAMS SAVE) is a copy of the EEPROM settings, re-read on the first request after any EEPROM block was written, from any source, and never during a transmission. Until 2026-09-30 (`f988b24` and earlier) it missed keypad and menu saves and could report a value the radio no longer had stored.
- Known limits: a keypad or menu save stores the live value of everything in the block it writes, so a value a host set in RAM in the same block (a deviation trial, say) is persisted with it; and in a bench build (caps bit 11) legacy 0x0602 writes any BK4819 register, REG_30, REG_33 and REG_36 included, which can put out RF outside the transmit state machine (no TX timeout, a PTT release does not stop it): tools must not use it for that. Release builds leave 0x0602 out.
- Golden vectors from the firmware code for the C# client and simulator: `tests/vectors/protocol-v2.json` (format in `tests/vectors/README.md`), regenerated and compared by `tests/host/run.sh`.
