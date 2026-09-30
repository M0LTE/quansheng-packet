# Protocol v2 golden vectors

`protocol-v2.json` holds request and response bytes produced by the real firmware code (the protocol modules compiled on the host by `tests/host/test_v2.c`), for checking an independent client or simulator against the firmware. `tests/host/run.sh` regenerates the file on every run and fails if the result differs from the committed copy; `tests/host/run.sh --vectors` rewrites it after an intended change.

Each vector:

| Field | Meaning |
|---|---|
| `name` | short name |
| `state` | the radio state the bytes depend on (clock, registers, EEPROM, earlier commands) |
| `request_mode`, `request` | the complete request frame as hex, `AB CD` to `DC BA`, built in that mode (`plain` or `obfuscated`); empty for unsolicited events |
| `response_mode`, `response` | everything the radio sent in reply, as hex, possibly several frames (events come before an `EVENT_REPLAY` reply) |
| `frames` | the response decoded: `id`, `crc` (`real` on every 0x50xx frame, `FFFF` on legacy replies) and `body` (after the inner id and length) |

Common state unless the vector says otherwise: a fresh boot with a blank EEPROM except the factory calibration area (so the first power-on writes and signs the default settings family, as on a newly flashed radio), firmware version string `PKTFW test`, clock 1000 ms at boot, then a plain-mode hello (`0x0514` with raw id bytes `14 05`), so replies carry `lock_ms` 20 (the default SERIAL_LOCK_MS) because every frame restarts the lock. Tags are arbitrary per vector.

Reply bodies start with the 4-byte reply header (`tag`, `status`, `lock_ms`); event bodies with the 7-byte event header (`seq`, `t_ms`, `flags`). Layouts are in `docs/protocol-v2.md` sections 4, 6 and 8.
