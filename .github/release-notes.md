Packet radio firmware for the Quansheng UV-K5, for use with an AIOC cable and a soundmodem or TNC. See the [README](https://github.com/M0LTE/quansheng-packet/blob/{{TAG}}/README.md) for what it does, how to flash it and how to set it up.

**Which file do I want?**

| File | What it is |
|---|---|
| `quansheng-packet-{{TAG}}.bin` | the firmware, packed for the radio's bootloader: flash this one |
| `quansheng-packet-{{TAG}}-raw.bin` | the same firmware unpacked, for developers |
| `k5ctl-{{TAG}}-<platform>` | the command-line flasher and control tool, for Linux (x64, arm64), Windows (x64) and macOS (Apple silicon) |
| `SHA256SUMS` | checksums of all of the above |

**Flashing, in short:** switch the radio off, hold PTT while switching it on (the torch LED lights and the screen stays blank), then run `k5ctl -p PORT flash quansheng-packet-{{TAG}}.bin --really-flash`. Without `--really-flash` it only checks the radio and the file.

**Reproducible:** the firmware was built in the pinned toolchain container. `./compile-with-docker.sh VERSION_STRING={{TAG}}` at this tag gives the same bytes.

Checksums:
