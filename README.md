# UV-K5 packet firmware

Firmware that turns a Quansheng UV-K5 into a clean, fast packet radio. You pair it with an [AIOC](https://github.com/skuep/AIOC) cable and a soundmodem or TNC on your computer (Direwolf, pdn-soundmodem, QtSoundModem and so on).

It does one job: send and receive FM data through a flat audio path, on one frequency. Everything a voice radio needs and a packet station does not has been taken out: squelch, tones (CTCSS/DCS, DTMF, roger beep), pre-emphasis and audio filters, scanning, dual watch, memory channels, FM broadcast radio, the spectrum display, VOX and battery save. Each of those either colours the audio, cuts the start of a packet, or adds a sound that would go out to the TNC.

It is for licensed radio amateurs who want a cheap, dependable packet radio. It is not a voice firmware.

**The quick way: [set up your station in your browser](https://m0lte.github.io/quansheng-packet/).** In Chrome or Edge, that page flashes this firmware on the radio and the [packet AIOC firmware](https://github.com/M0LTE/aioc-packet) on the AIOC, sets the radio's frequency and power, and walks you through your soundmodem settings. Nothing to install.

## What you get

Measured on the bench, with an AIOC running the [packet AIOC firmware](https://github.com/M0LTE/aioc-packet):

- **A flat audio path.** Transmit audio is flat within -0.6 to +0.4 dB from 20 Hz to 6 kHz over the air, with no pre-emphasis, filters, AGC or compression in the way. Receive audio is always open.
- **Clean deviation.** About 2.8 kHz at full-scale audio with the default settings, in proportion all the way up to full scale.
- **Fast turnaround.** RF is up about 16 ms after PTT (the stock firmware takes about 50 to 60 ms). After unkeying, the carrier is off in about 5 ms and the receiver is ready about 14 ms later, with audio back at the AIOC after about 25 ms (stock: 70 to 80 ms).
- **Packets that decode.** AFSK 1200, QPSK 3600 and FSK 9600 all decoded 100% on the bench with 30 to 50 ms of TXDELAY.
- **Software control** over the AIOC's serial port: carrier detect, signal reports and settings for your TNC, plus a command-line tool (`k5ctl`).

## Supported radios

The Quansheng UV-K5 and near relatives whose bootloader reports **version 2.00.06** (`k5ctl` shows this before it flashes anything, and by default refuses other versions). It has been tested on one UV-K5 with bootloader 2.00.06 and an AIOC (hardware revision 1.0).

Newer models built on a different processor, such as the UV-K5 V3 and UV-K1, are not supported.

## Flashing

### Flash from your browser (Chrome or Edge)

The [setup page](https://m0lte.github.io/quansheng-packet/) flashes the latest release with its own flasher, then checks the radio's version. Or use armel's UVTools:

**[Flash the latest release from your browser](https://armel.github.io/uvtools/?firmwareURL=https://raw.githubusercontent.com/M0LTE/quansheng-packet/flash/latest/quansheng-packet.bin)**

1. **Put the radio in flash mode** (bootloader mode): switch it off, then hold PTT while switching it on. The torch LED lights and the screen stays blank.
2. **Connect** the AIOC (or any UV-K5 programming cable) to the radio and the computer.
3. **Open the link above** in Chrome or Edge, pick the cable's serial port and flash. The link opens [armel's UVTools](https://github.com/armel/uvtools), a third-party page, with the latest release of this firmware already loaded from this project's `flash` branch. Each release's notes link to that release's image the same way.
4. Switch the radio off and on. The menu's last item, **Ver**, shows the version.

### Or with k5ctl

You need the firmware file `quansheng-packet-<version>.bin` from the [latest release](https://github.com/M0LTE/quansheng-packet/releases/latest), already packed for the radio's bootloader, and `k5ctl` from the same release (Linux, Windows, macOS). With the radio in flash mode and connected as above, first a dry run, which checks the radio and the file and sends nothing, then the real thing:

```sh
k5ctl -p /dev/ttyACM0 flash quansheng-packet-v1.0.1.bin
k5ctl -p /dev/ttyACM0 flash quansheng-packet-v1.0.1.bin --really-flash
```

On Windows the port is `COM3` or similar; `k5ctl ports` lists them. On macOS, a downloaded binary may need `xattr -d com.apple.quarantine k5ctl` before it will run. `k5ctl` checks the bootloader version before it flashes anything.

### The first start is a factory reset

Flashing over any other firmware (or over v1.0.0 of this one) is like a factory reset. On its first start the radio ignores everything the previous firmware saved and starts with every setting at its default:

- **144.800 MHz**, the European APRS frequency
- **low power** (`~0.5W`), wide (25 kHz) channel, 12.5 kHz step
- deviation `0x856` wide and `0x756` narrow, TX timeout 30 s, backlight 20 s, key lock off
- receive audio gain from the radio's factory calibration

It writes those defaults to its own settings area once, then remembers your changes from then on. Low power is the default because it is kinder to the radio's amplifier and battery on long packet transmissions and on a first key-up into an unknown antenna; raise it with F then 6, the menu or `k5ctl -p PORT set power=high --persist`.

The radio's **factory calibration** (receive thresholds, transmit power, battery and crystal tuning) is kept: it is read, and never written.

### Going back

Flashing the stock firmware or another one works the same way: flash its packed image in flash mode. This firmware never writes the factory calibration and never changes your memory channels. It keeps its own settings in the part of the EEPROM the stock firmware uses for DTMF contacts (0x1D00 to 0x1D6F), and its first start overwrites that part, so re-enter any DTMF contacts stored there if you used them. For extra peace of mind, take a backup before you start: `k5ctl -p PORT backup k5-backup.bin`.

## Using the radio

**Keys on the main screen**

| Key | Does |
|---|---|
| 0 to 9 | enter a frequency in kHz, six digits: `144800` is 144.800 MHz |
| EXIT | delete the last digit |
| UP, DOWN | step the frequency |
| F, then 6 | change TX power |
| F, held | lock or unlock the keypad |
| MENU | settings |
| PTT | transmit (normally the AIOC keys the radio for you) |

The screen shows the frequency, the signal strength, and the settings in use, for example `~5W WIDE TOT30`, `DEV ~2.8kHz` and `RXG58 DAC15`. `RX` appears while a signal is present.

`DEV` is the approximate peak deviation for the channel width in use (DevW on wide, DevN on narrow), worked out from the setting. It assumes full-scale audio through the [packet AIOC firmware](https://github.com/M0LTE/aioc-packet); quieter audio gives less, and with a stock AIOC it reads low (see below). The menu shows the setting itself, with the same estimate under it.

**Menu.** MENU opens it, UP and DOWN move, MENU edits an item and MENU again saves it, EXIT cancels. Changes take effect straight away and are remembered.

| Item | What it sets |
|---|---|
| Step | frequency step for UP and DOWN |
| TxPwr | transmit power: `~0.5W`, `~2W` or `~5W`. These are nominal: the real power depends on each radio's factory calibration |
| W/N | wide (25 kHz) or narrow (12.5 kHz) channel |
| DevW | transmit deviation for wide channels (see below) |
| DevN | transmit deviation for narrow channels |
| RxG | receive audio gain, 0.5 dB steps |
| RxDAC | receive audio output gain, about 2 dB steps |
| TxTOut | transmit timeout: 5 to 120 seconds (default 30) |
| BackLt | backlight time |
| BatVol | battery voltage and charge (read only) |
| Ver | firmware version (read only) |

## Setting up for packet with an AIOC

Two firmwares make the pair: this one on the radio, and the [packet AIOC firmware](https://github.com/M0LTE/aioc-packet) on the AIOC. For the AIOC, flash `aioc-packet-X.Y.Z.bin` from its [releases](https://github.com/M0LTE/aioc-packet/releases) (the [setup page](https://m0lte.github.io/quansheng-packet/) does it in the browser). Its transmit equaliser, tuned for the UV-K5, is on out of the box, so there is nothing to switch on. Flashing it resets the AIOC's settings to the defaults, which key PTT from the CM108 interface and from the serial port with DTR high and RTS low. Then:

1. **PTT through the AIOC's CM108 (HID) interface.** In Direwolf that is `PTT CM108`; pdn-soundmodem and most soundmodems support it. Keep the AIOC's serial port for control software only.
2. **Transmit level: drive the audio hot and set deviation in the radio.** Set your soundmodem's transmit level near full scale, just short of clipping, and then set the deviation with DevW and DevN. Never the other way round: the radio adds a little hiss of its own that grows with the deviation setting, so a quiet TNC with a high deviation setting makes a noisier signal.
   - The defaults (DevW 2134, shown as `0x856`, and DevN 1878, `0x756`) give about 2.8 kHz and 1.4 kHz of deviation at full scale **with the [packet AIOC firmware](https://github.com/M0LTE/aioc-packet)**, whose transmit EQ is tuned for the K5.
   - **With a stock AIOC** use DevW 1890 (`0x762`) and DevN 1634 (`0x662`). A stock AIOC gives about 1.9 times more deviation for the same setting, so these give about the same as the defaults do with the packet AIOC, and the `DEV` figure on the screen reads low (about 1.4 kHz for `0x762`).
   - The scale is logarithmic: 256 higher doubles the deviation, 16 higher is about 0.4 dB more. The `DEV` figure follows it, from about 0.1 kHz up to about 12.5 kHz at the top of the range; treat it as a guide and measure if it matters.
3. **TXDELAY.** Start at 50 ms. On the bench 30 ms was enough for AFSK 1200 and QPSK 3600, and FSK 9600 was happiest at about 50 ms. The station you are talking to may need more.
4. **Receive level.** Audio is always open (there is no squelch), and the **volume knob** sets the level into the AIOC. Turn it so the strongest packets come in well below clipping on your soundmodem's level meter. The level tone helps here: `k5ctl -p PORT tone 1000 64 10000` replaces the receive audio with a steady 1 kHz tone for 10 seconds, so you can see where the audio clips and back off from there. Then check with real packets.
5. **Do not transmit with the charger connected.** On the bench it put severe noise on the transmitted signal and every packet failed.

Two rules for anything that talks to the radio's serial port (both matter because the AIOC shares one wire between PTT and the radio's serial input):

- **38400 baud only.** At slower rates, serial data can look like a PTT press and key the transmitter.
- **Do not send serial data while transmitting.** Any byte ends the transmission.

## Software control

The radio speaks a serial control protocol over the AIOC's serial port: carrier detect with timestamps, a signal report after every received packet, transmit timing, and live settings, all pushed to the host so a TNC need not poll. It is specified in [docs/protocol-v2.md](docs/protocol-v2.md).

- **k5ctl** (in each release) flashes, backs up, and reads and changes settings from the command line. `k5ctl --help` lists everything; `k5ctl -p PORT status` and `k5ctl -p PORT set frequency=144.800MHz power=high` are good places to start.
- **M0LTE.Uvk5**, a .NET library for TNC and soundmodem authors, with a simulated radio for tests: [host/dotnet](host/dotnet/README.md).

## Safety

- **The factory calibration is never written.** The firmware refuses any write to that part of the EEPROM, whatever asks for it.
- **Transmit timeout.** Every transmission ends after the TxTOut time (30 seconds unless you change it), and the next one needs PTT released first.
- **Nothing can key the radio over the serial port.** The protocol has no transmit command, commands that could turn on the transmitter or its amplifier are refused, and at 38400 baud no serial data can be mistaken for a PTT press.
- **Transmitting is refused** when the battery is flat or the supply voltage is too high.
- **Transmit is limited to 136 to 174 MHz and 400 to 470 MHz**, the ranges the radio's amplifier and filters are designed for. This is fixed in the firmware; anything another firmware set up does not change it. A PTT press anywhere else is refused (the screen shows it, and the software control reports it). Receive works across the whole range, 350 to 400 MHz included. Transmit only where your licence allows.

## More

- [docs/packet-fw.md](docs/packet-fw.md): how the firmware works, its settings in EEPROM, building from source, and the measurements behind the defaults.
- [docs/protocol-v2.md](docs/protocol-v2.md): the serial control protocol.
- [M0LTE/aioc-packet](https://github.com/M0LTE/aioc-packet): the AIOC firmware with the transmit EQ these defaults assume.

## Credits

This firmware is a cut-down build of [mobilinkd's uv-k5-firmware-custom](https://github.com/mobilinkd/uv-k5-firmware-custom), whose DIG mode gave it the flat audio path. That in turn builds on [egzumer's firmware](https://github.com/egzumer/uv-k5-firmware-custom), [OneOfEleven's custom firmware](https://github.com/OneOfEleven/uv-k5-firmware-custom), [fagci's work](https://github.com/fagci/uv-k5-firmware-fagci-mod) and [DualTachyon's open re-implementation](https://github.com/DualTachyon/uv-k5-firmware) of the Quansheng firmware. Many thanks to all of them.

Use this firmware at your own risk. There is no guarantee that it will work on your radio.

## License

Copyright 2023 Dual Tachyon
https://github.com/DualTachyon

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

    Unless required by applicable law or agreed to in writing, software
    distributed under the License is distributed on an "AS IS" BASIS,
    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
    See the License for the specific language governing permissions and
    limitations under the License.
