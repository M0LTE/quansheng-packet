# UV-K5 packet firmware (branch `packet-fw`)

A cut-down build of [mobilinkd/uv-k5-firmware-custom](https://github.com/mobilinkd/uv-k5-firmware-custom) (base `e1e2fea`) for a UV-K5 used as a packet radio behind an AIOC. It does one thing: receive and transmit FM through the flat ("DIG") audio path, on one frequency, with as few settings as possible. Apache-2.0, as upstream.

Status: first pass, built and host-tested only. **Not yet run on a radio.**

## Build and test

```sh
/home/tf/src/uvk5-packet-bench/tools/k5/build.sh --src . --out out/NAME   # pinned gcc 10.3.1 in Docker
tests/host/run.sh                                                          # host-side logic tests, system gcc
/home/tf/src/uvk5-work/venv/bin/python3 /home/tf/src/uvk5-packet-bench/tools/k5/k5.py image-info out/NAME/firmware.packed.bin
```

There are no feature flags left in the Makefile, only `ENABLE_CLANG`, `ENABLE_SWD` and `ENABLE_LTO`. The packed image carries `*PKTFW <git hash>` as its version.

## Size

Flash for the firmware is 61440 bytes (60 KiB). All sizes are gcc 10.3.1 (Docker).

| Build | Image | Free |
|---|---|---|
| Upstream, default flags | 61364 | 76 |
| Upstream, DIG + BK register commands, no spectrum | 56080 | 5360 |
| Compile-time features resolved out (commit `7be7800`) | 45624 | 15816 |
| Calibration write guard (`c217d5e`) | 45608 | 15832 |
| App layer replaced by the packet station (`c991086`) | 22432 | 39008 |
| Settings reload after UART writes, version check (`5e77b4b`) | 22524 | 38916 |
| Review fixes (see below) | 22660 | 38780 |
| Bench deviation defaults, register override table (`b2e8a25`) | 22972 | 38468 |
| 1 ms PTT sampling, timing settings, key-up before redraw | 23356 | 38084 |
| Review fixes: REG_30 off at key-down, release debounce | 23520 | 37920 |
| Bench defaults: deviation 0x956/0x856, PA delays 1/2 ms | 23520 | 37920 |
| Deviation defaults 0x856/0x756 (TNC at 0 dBFS) | 23520 | 37920 |
| Serial control protocol v2 (`docs/protocol-v2.md`) | 36188 | 25252 |
| Memory channels and band slots removed: one operating channel | 35136 | 26304 |
| Review fixes (late-key bound, event pacing, tone audio path) | 35316 | 26124 |

## What was removed

FM broadcast radio, spectrum, all scanning (frequency, channel, scan lists, CTCSS/DCS scan), dual watch and cross band, battery save (the receiver no longer sleeps between polls, which would miss the start of a packet), DTMF (calling, ANI, PTT ID, live decoder, side tones), VOX, flashlight, voice prompts, alarm and 1750 Hz tone, roger beep and every other beep (they would go to the TNC), CTCSS/DCS and tail tones, scrambler, compander, AM, USB and the FM voice mode, NOAA, aircopy, power-on password, AES challenge and lock, boot modes and the hidden menu, channel names, TX offset and reverse, the 60-item menu, and the SRAM overlay.

CTCSS/DCS went because packet needs no tone squelch, and dropping it removes the tone scanning, tail detection and interrupt handling with it. The squelch itself went on 30 September (Tom): the TNC decodes from open audio, and a squelch only cut the start of frames.

## What remains

- One VFO, simplex (TX frequency = RX frequency), no squelch: receive audio is always open, and the speaker amplifier (the K1 audio out) always on in receive. The chip's squelch result is kept only as a carrier detector for the protocol's busy events and the green LED ("RX" on the display); it never mutes anything.
- One operating frequency, with its power, bandwidth and step, stored in this firmware's own settings (0x1D58); frequency entry on the keypad and up/down stepping. No memory channels, no band slots, no CHIRP compatibility (see "Operating channel" under Settings).
- Power (low, mid, high from the factory calibration, shown as `~0.5W`, `~2W` and `~5W`), bandwidth (wide, narrow), TX timeout, battery monitoring (TX refused below about 6.3 V and above about 8.9 V, as upstream), backlight, key lock.
- Display: frequency, TX/RX, RSSI in dBm and S-units, and the settings in use (for example `~5W WIDE TOT30`).

**The watts are nominal, not measured.** The screen and the menu show the usual UV-K5 figures for the three power levels, the same on VHF and UHF, with a tilde because nothing measures them. What the radio really puts out depends on its factory PA calibration (0x1ED0 up), which varies from radio to radio: the bench K5's VHF high row is 105, 116 and 123 across 137 to 174 MHz, against 135 in most dumps, so at 145 MHz its high (about 110) is barely above its mid (105) and likely less than 5 W. Measure a radio's output before relying on the figure.
- UART: the upstream EEPROM protocol and the BK4819 register commands, plus the serial control protocol v2 (`docs/protocol-v2.md`): identification, status, events (busy, bursts, TX timing, heartbeats), parameters, register access and overrides, a level tone.

## The audio path

Always the upstream DIG path: no pre-emphasis or de-emphasis, no TX or RX audio filters, no DC filters, no mic AGC, no ALC, no compander. The fixed register values are named in `packet.h`; the ones to tune are settings.

| Register | Value | Where |
|---|---|---|
| REG_7D | `0xE940` or mic gain | settings (mic gain); on the bench the whole 0 to 31 range moves deviation by only about 0.5 dB |
| REG_40 | top 3 bits from the chip, bit 12, deviation | settings (wide and narrow deviation); logarithmic, see below |
| REG_48 | `11<<12`, gain 1 0 dB, gain 2, DAC gain | settings (RX gain, RX DAC gain) |
| REG_47 (TX) | `0x2041`: AF muted, TX filters bypassed | `PKT_REG_47_TX` |
| REG_7E | DC filters off; bit 15 set for TX, cleared on RX | `PKT_REG_7E_*` |
| REG_2B | `|0x0707`: RX and TX filters and emphasis off | `PKT_REG_2B_FLAT_MASK` |
| REG_43 | upstream DIG wide and narrow values | `BK4819_SetFilterBandwidth` |
| REG_31 | scrambler, VOX, compander bits cleared | `PKT_REG_31_OFF_MASK` |
| REG_3D | `0x2AAB` on every receive set-up | `PKT_REG_3D_RX` |
| REG_47 (RX) | `0x6140`: FM demodulator output, always (no squelch) | `RADIO_SetupRegisters` |

## Settings

Stored in a 16-byte block at EEPROM `0x1D00` (the old DTMF contacts area). The block is ignored unless byte 0 is the layout version (1); an out-of-range byte means "use the default".

| Address | Setting | Range | Default |
|---|---|---|---|
| 0x1D00 | layout version | 1 | |
| 0x1D01 | busy detector level: the row of the factory squelch tables the chip's carrier detector uses (not a squelch; v2 parameter BUSY_SQL_LEVEL, not in the menu); was the squelch level, and 0 now means 1 | 1 to 9 | 1 |
| 0x1D02 | TX timeout | 0 to 6 = 5, 10, 15, 20, 30, 60, 120 s | 4 (30 s) |
| 0x1D03 | mic gain, REG_7D<4:0> | 0 to 31 | 31 |
| 0x1D04 | wide deviation, REG_40<11:0>, u16 LE | 0 to 0xA7F | 0x856 (about 3 kHz at 0 dBFS with the bench AIOC EQ) |
| 0x1D06 | narrow deviation, REG_40<11:0>, u16 LE | 0 to 0xA7F | 0x756 (half the wide deviation) |
| 0x1D08 | RX AF gain 2, REG_48<9:4>, 0.5 dB steps | 0 to 63 | factory calibration (0x1F8E) |
| 0x1D09 | RX DAC gain, REG_48<3:0>, about 2 dB steps | 0 to 15 | 15 |
| 0x1D0A | backlight | 0 (off) to 7 (on) | 3 (20 s) |
| 0x1D0B | battery type | 0 = 1600, 1 = 2200 mAh | 0 |
| 0x1D0C | key lock | 0, 1 | 0 |

**Deviation is logarithmic.** Measured on the bench K5 (2026-09-28): +0x100 in REG_40<11:0> doubles the deviation, about 0.0235 dB per step. 0x862 gives 3.13 kHz for a 999 Hz tone at -6 dBFS from the AIOC at mic level, linear up to 0 dBFS; 0x800 gives 3.67 kHz at -1.94 dBFS. From 0xB00 up the chip wraps to near zero deviation, so both settings (and any REG_40 override) are clamped to 0xA7F. Narrow is 0x100 below wide, which halves the deviation. A value above 0xA7F in EEPROM means "use the default".

**Operating rule: drive the audio near full scale and keep REG_40 low.** The K5 adds analogue hiss to its transmitted FM: about 900 Hz rms residual deviation in 3 to 8 kHz at 0x956, and it scales exactly with REG_40 (it is added before the deviation gain). So set the TNC to drive the AIOC near 0 dBFS and choose the deviation to give about 3 kHz there, rather than a quiet TNC and a high REG_40.

The defaults (0x856 wide, 0x756 narrow) do that for the bench AIOC, which carries a stored TX EQ that cuts 5.74 dB at 1 kHz: 0x856 gives about 3 kHz at 0 dBFS. On the bench, fsk9600 decoded 15 of 15 at 0 dBFS with 0x856 (it failed at -6 dBFS with 0x956), and afsk1200 and qpsk3600 decoded 100% at 0 and -3 dBFS. **With a stock AIOC (no EQ) the equivalents are 0x762 wide and 0x662 narrow.** Deviation is a setting (menu DevW and DevN, or EEPROM 0x1D04 and 0x1D06): set it to suit the interface.

### Register override table

For experiments without reflashing (TX filters and so on), 8 entries of 8 bytes at `0x1D10..0x1D4F`. It is used only when the settings block has its version byte, and the first menu save over foreign data in `0x1D00` blanks it.

| Offset | Field |
|---|---|
| +0 | phase: bit 0 = after the TX set-up (every key-up), bit 1 = after the RX set-up (every return to receive); 0 or 0xFF ends the list |
| +1 | BK4819 register; 0xFF ends the list |
| +2 | AND mask, u16 LE |
| +4 | OR value, u16 LE |
| +6 | reserved, 0xFF |

The register becomes `(value & mask) | or`, written after all of the firmware's own writes for that phase, so it wins. Refused (skipped): 0x00 soft reset, 0x30 TX/RX enables, 0x33 GPIO outputs (PA enable, RX enable, LNA switch, LEDs), 0x36 PA bias and gain, 0x37 power and LDOs, 0x38 and 0x39 frequency, 0x3B and 0x3C crystal trim, and anything above 0x7F. A REG_40 result is clamped to 0xA7F. The table is read at power-on and after a UART write session, like the settings; each entry is one 8-byte UART write. Example: `01 2B F8 FF 00 00 FF FF` clears REG_2B<2:0> on every key-up, which turns the TX HPF300, LPF and pre-emphasis back on.

### Operating channel

There are no memory channels or band slots, and the upstream channel layout (CHIRP's) is not used. The one frequency the radio works on, with its power, bandwidth and step, is an 8-byte block at `0x1D58`, used only with a valid settings block and blanked with the others by the first menu save over foreign data. The keypad, the menu (Step, TxPwr, W/N) and the v2 protocol store it; a keypad or menu save over foreign data at `0x1D00` writes the settings block first, so the frequency sticks.

| Address | Setting | Range | Default |
|---|---|---|---|
| 0x1D58 | frequency, u32 LE, 10 Hz units | receivable (inside the band table; 350 to 400 MHz only if enabled) | 144.800 MHz |
| 0x1D5C | power | 0 low, 1 mid, 2 high (shown as ~0.5W, ~2W, ~5W) | 0 |
| 0x1D5D | bandwidth | 0 wide, 1 narrow | 0 |
| 0x1D5E | step, index into the step table | 0 to 23 | 12.5 kHz |
| 0x1D5F | reserved, 0xFF | | |

If the frequency there is not receivable (a blank block, or a radio coming from another firmware or from the channel-memory builds of this one), the frequency the old upstream layout had in use (channel indices at `0x0E80` and the memory channel or band slot record they point at) is taken once if it is receivable, else 144.800 MHz. With a valid settings block it is then written to `0x1D58` at once and the old layout is never read again; without one nothing is written until the first save.

Other EEPROM the firmware reads: S-meter levels at `0x0EA0`, TX band limits at `0x0F40` (no menu). Calibration (`0x1E00` up) is read only.

The same settings are in the menu (MENU, then UP/DOWN, MENU to edit and again to store, EXIT to cancel): Step, TxPwr, W/N, MicG, DevW, DevN, RxG, RxDAC, TxTOut, BackLt, BatTyp, plus the battery voltage and the version.

Keys on the main screen: digits enter a frequency, UP/DOWN step, F then 6 cycles power, F held locks the keypad. The side keys have no function (there is no squelch to open).

## Key-up and key-down

**PTT sampling.** SysTick now runs at 1 ms (upstream 10 ms; the 10 ms and 500 ms slices are derived from it). `ptt.c` samples the PTT line every tick with separate press and release debounce, and the main loop acts on a change at once instead of waiting for the next 10 ms slice (upstream: 3 samples of 10 ms both ways, 20 to 30 ms).

Timing settings, 8 bytes at `0x1D50` (one UART write; not in the menu; used only with a valid settings block, blanked with the override table on the first menu save over foreign data):

| Address | Setting | Range | Default |
|---|---|---|---|
| 0x1D50 | PTT press debounce, ms | 1 to 40 | 5 |
| 0x1D51 | PTT release debounce, ms | 2 to 40 | 5 |
| 0x1D52 | delay after PA enable, before the PA bias, ms | 1 to 20 | 1 (upstream 5) |
| 0x1D53 | delay after the PA bias, ms | 0 to 20 | 2 (upstream 10) |
| 0x1D54 | reserved, 0xFF | | |

**Why a 5 ms press debounce is still safe with UART on the same contact.** While not keyed, a tick only counts towards a press if the line reads low continuously for 280 us (`PTT_WINDOW_US`), read in a tight loop well under 1 us per read. At 38400 baud one character is 260 us and always ends in a high stop bit of 26 us, and the line idles high between characters, so any UART traffic, even an unbroken run of 0x00 bytes (low for 9 of every 10 bits), shows a high level inside every 280 us window and never counts. A real press (the AIOC holding the line low) passes. The busy read runs only while the radio is not keyed and the line reads low, so it costs at most 0.28 ms per ms, and only during a candidate press or during serial traffic. On top of that the payload of obfuscated frames is XORed with a 16-byte key (so zero runs become mixed bytes), frames start with `AB CD`, and every valid frame ends any transmission and starts the serial PTT lock (below). The host tests run the real `PTT_Tick` against a simulated line and SysTick and sweep a 2000-byte stream of zero bytes and of random bytes over every start phase: nothing keys even with a 1 ms debounce; shortening the window to 200 us makes them fail. Limit: this relies on the host sending at 38400 baud. A host at a much lower baud rate (a character longer than 280 us) can key the radio: on the bench 1000 zero bytes keyed it for 0.455 s at 9600 baud and 0.120 s at 19200, never at 38400. Hosts must use 38400 baud and never send a break.

**Key-up path**, with the defaults: 5 ms debounce plus up to 1.3 ms of tick phase and window, then the main loop picks the change up (usually well under 1 ms, longer if it is in the middle of a screen redraw or a UART command), then about 30 register operations (roughly 3 ms of bit-banged SPI: filters, frequency, TX set-up, TX enable), PA enable, 5 ms, PA bias (RF appears here), 10 ms (ready for modulation). The screen is no longer redrawn before key-up; the 10 ms slice redraws it afterwards. Expected: RF about 10 ms and ready for modulation about 12 ms after PTT goes low with the default PA delays of 1 and 2 ms (about 14 and 24 ms with the upstream 5 and 10 ms). Upstream measured 61 to 66 ms to RF.

**Release debounce.** While keyed, a tick counts towards release when the line reads high and stays high for 20 us, or when a check finds the line is no longer low for a whole 280 us window (the host has started sending right after the release). A high spike followed by a whole low window is a held press and restarts the count, so a single-sample spike cannot drop TX. The window check runs on every 4th tick while keyed (about 7% CPU during transmit) and on every tick once a release has started; host tests show UART traffic, even zero bytes, starting right at the release does not delay the unkey. The trade-off in the release time: shorter releases faster but a longer real glitch on the PTT line (a bouncing contact, a loose cable) ends the transmission sooner. The default is 5 ms and the floor 2 ms.

**Key-down path**: 5 ms release debounce plus up to 1 ms, main loop latency, then the PA bias goes to zero, the PA enable and red LED go off, and REG_30 is cleared, which takes the BK4819 out of TX at once (it used to stay in TX until the receive set-up reached the receiver turn-on, several ms later). Then the full receive set-up. Expected: RF gone about 6 to 7 ms after PTT is released, about 3 to 4 ms with the release debounce at 2 (upstream measured 33 to 37 ms). PTT is not acted on at all in the critical-battery reduced-service state, as upstream. The PA bias still steps straight to zero, as upstream; if the RSP1 shows a click, the PA delay settings do not help there and a ramp would be a firmware change.

**Measured on the bench K5 with `a97accc`** (AIOC HID PTT, RSP1; times include an SDR delay of up to 14 ms): PTT to RF about 33 ms, unkey to RF gone about 14 ms (upstream: 61 to 66 ms and 33 to 37 ms). afsk1200 and qpsk3600 decoded 100% down to 0 ms TXDELAY. Adjacent-channel power at key-up was -35 to -40 dB re the channel with PA delays of 1 and 2 ms, no worse than with 5 and 10 ms (measurement floor about -43 dB), so 1 and 2 ms are now the defaults.

**Also moved out of the key-down path:** the LCD re-initialisation after transmit (`ST7565_FixInterfGlitch`) now runs just before the next redraw in the 10 ms slice.

**Left in the release path, could be deferred later** (not changed, because each affects receive readiness or needs measuring):
- `RADIO_SetupRegisters` does a full receive set-up after every transmission: bandwidth and filter registers, the squelch-detector thresholds, frequency, and `BK4819_RX_TurnOn`, which writes REG_30 to 0 and back and so re-runs the VCO calibration. On a simplex frequency most of this is unchanged from before the transmission; skipping the unchanged parts could bring receive audio back sooner.
- The loop that drains pending BK4819 interrupts (REG_0C, with a 1 ms delay per pass).
- (It no longer runs at the end of each received frame: there is no squelch to close.)
- Postponed EEPROM saves (8 ms per 8-byte block) run after the transmission ends if a key was held during it; rare, and after RF is already off.
- TX timeout and the battery ADC read are unaffected.

## UART commands

38400 8N1, upstream framing (see the bench repo's `docs/k5-firmware.md`). Plain mode after a hello whose raw id is `14 05`.

| Command | Does | Change from upstream |
|---|---|---|
| 0x0514 hello | version reply, starts the PTT lock | no backlight change, no AES fields |
| 0x052F hello | same as 0x0514 | no longer changes VFO settings |
| 0x051B read EEPROM | up to 128 bytes | refuses more than 128 (upstream overflowed its stack) |
| 0x051D write EEPROM | 8-byte blocks | refused whole, with no reply, if any block is unaligned or at 0x1E00 and up, or if the data does not fit the frame; applied 1.0 s after the last write, never during TX |
| 0x0527 | RSSI, noise, glitch | none |
| 0x0529 | battery voltage and current | none |
| 0x05DD | reboot | de-keys first (PA off, BK4819 off) |
| 0x0601 / 0x0602 | BK4819 register read / write | always built in; frames too short to hold the register (and value) are ignored |

0x052D (AES challenge), 0x051F and 0x0521 are gone. Caution: 0x0602 writes any BK4819 register unchecked, including REG_30 (TX enable), REG_33 (PA enable) and REG_36 (PA bias), so it can put out RF outside the transmit state machine, with no TX timeout and no unkey on a PTT release; the v2 REG_WRITE refuses those registers. Register writes still get overwritten by the firmware on the next receive set-up or key-up for the registers it manages (7D, 40, 47, 48, 7E, 2B, 43, 31): change those through the settings instead.

**PTT lock.** On the K1 connector the UART receive line shares a contact with PTT. Every valid frame, of any kind, ends any transmission before its command runs and starts the serial PTT lock: `SERIAL_LOCK_MS`, 20 ms by default (a v2 parameter, 0 to 1500 ms, stored at 0x1D61), counted every 1 ms. A press during the lock keys at most 30 ms late, when the lock runs out, or is refused until PTT is released and pressed again; v1 held PTT off for 1.0 to 1.5 s and keyed late when it ran out, eating the start of the frame. After a v2 command a host may key at `t_reply + lock_ms` (the lock is in every reply); after a legacy reply at `t_reply + SERIAL_LOCK_MS + 2 ms`. A transmission ended by a frame, a TX timeout or a refused press needs PTT released first. What stops serial data being taken as a press is the 280 us window, not the lock (`docs/protocol-v2.md` 1c).

**Output and timing.** Everything the radio sends goes through a 512-byte queue drained into the UART FIFO by the UART TX interrupt, the main loop and the 1 ms tick (the tick alone managed only 1 to 2.3 bytes/ms on the bench); the command handler no longer blocks with interrupts off. Frames are handled on every pass of the main loop (v1: one per 10 ms slice, so replies took 10 to 20 ms), all complete frames at once. The parser drops only the `AB` of anything that cannot be a frame and drops a frame still incomplete 5 ms after its last byte, so a frame cut short by PTT no longer leaves the radio deaf to hellos.

**Protocol v2.** Commands 0x5000 to 0x507F, events 0x50C0 up, specified in `docs/protocol-v2.md` (section 13 lists what is implemented). A hello's 0x0515 reply carries the `PKT2` marker in its challenge field; the replies to legacy commands are otherwise byte-identical to v1. v2 settings (lock, busy detection, default subscription, tone calibration) live in the 16-byte block at 0x1D60, used only with a valid settings block and its own version byte, and blanked by the first menu save over foreign data. Nothing in v2 keys the transmitter.

## Fixes

- **AGC.** Upstream DIG sets REG_7E bit 15 (AGC fix) at every key-up and never cleared it, so after the first transmission receive ran at a fixed AGC index (the maximum). It is now cleared on every return to receive. Receive levels measured on upstream DIG after any transmission were taken with the AGC frozen, so expect them to change.
- **Deviation.** Upstream DIG wrote REG_40 = 0x383 wide and 0x4D6 narrow, so wide was 2.8 dB below narrow. Both are now settings, clamped below the wrap at 0xB00, with bench-measured defaults for a TNC at 0 dBFS: 0x856 wide and 0x756 narrow (half) for the bench AIOC with its TX EQ, 0x762 and 0x662 for a stock AIOC, and mic gain 31. The top 3 bits are read from the chip after its reset, not before.
- **Calibration.** Upstream wrote a build-options byte to 0x1FF0 on every boot and could save battery calibration from the menu. This firmware never writes 0x1E00 and up (checked in the EEPROM driver and in the UART handler).
- **TX timeout** returns to receive at once, and the next transmission needs PTT released first. It has 0.5 s resolution.
- **Nothing is saved during a transmission.** Saves postponed while UP/DOWN was held, menu changes and the receiver set-up that follows them wait until TX ends; EXIT held does not turn the monitor off mid-transmission.
- **Settings reload after UART writes** closes an open menu item, so MENU cannot store the value it showed before the reload.

## Other differences from upstream DIG that a measurement could see

- REG_24 (DTMF detector) is cleared at power-on, not only at the first key-up; the DTMF coefficient writes (REG_09) are gone.
- REG_48 uses the RX DAC gain setting (default 15) at all times; upstream used the calibration value (usually 8) while its squelch was closed.
- A blank EEPROM starts on 144.800 MHz (upstream: the bottom of the 2 m band slot, 137.000 MHz).
- The first-power-on defaults differ from upstream: no dual watch, and a 30 s TX timeout instead of 1 minute.

## Left for measurement to decide

Whether narrow (0x100 below wide) really gives half the wide deviation with the chip in its 12.5 kHz mode, the TX low-frequency lift (+6.7 dB at 50 to 63 Hz, +2 dB at 315 Hz, -1.1 dB at 3.15 kHz, -5.5 dB at 6 kHz re 1 kHz, measured), the RX gain defaults (target: 3 kHz deviation near -10 dBFS at the AIOC), whether keeping REG_7E bit 15 during TX matters at all, REG_43 receive bandwidths, REG_47 output select (FM against BASEBAND1), and the rest of the turnaround path listed under "Key-up and key-down".
