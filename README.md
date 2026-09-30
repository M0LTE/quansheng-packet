# Firmware images for browser flashing

This branch only holds the packed firmware images from the [releases](https://github.com/M0LTE/quansheng-packet/releases), so a browser-based flasher can load them directly (raw.githubusercontent.com allows cross-origin reads; release downloads do not).

- `vX.Y.Z/quansheng-packet-vX.Y.Z.bin`: the image from that release, byte for byte.
- `latest/quansheng-packet.bin`: the latest release.

Flash the latest release from your browser (Chrome or Edge, with Web Serial):
https://armel.github.io/uvtools/?firmwareURL=https://raw.githubusercontent.com/M0LTE/quansheng-packet/flash/latest/quansheng-packet.bin

The release workflow updates this branch; do not edit it by hand. Checksums are in each release's SHA256SUMS.
