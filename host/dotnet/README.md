# M0LTE.Uvk5

A .NET library for controlling a Quansheng UV-K5 over its serial port, written for a TNC or soundmodem (pdn-soundmodem) driving the radio through an AIOC cable. It speaks the packet firmware's protocol v2 (`docs/protocol-v2.md`), and falls back sensibly on packet firmware v1 and on stock firmware.

What you get:

- **Carrier detect from the radio itself**: busy edges as events, timestamped by the radio, so CSMA does not have to guess from audio.
- **A per-burst signal report** after every received packet: RSSI mean, min and max, noise and glitch.
- **Safe keying**: the library always knows when the radio will honour a PTT press, and can hold the serial port quiet while you transmit.
- **Live settings** (frequency, power, deviation in kHz, busy detector thresholds, PTT timing) changed atomically and read back. There is no squelch: the packet firmware's receive audio is always open, and the chip's squelch result is only a carrier detector for busy (`BusySquelchLevel`, 1 to 9).
- **Safety rails** that cannot be turned off: no writes to the calibration area, no EEPROM writes without a verified backup of this radio, no writes to dangerous registers, no serial keying.
- **A simulated radio** for your own tests, a bootloader flasher, and `k5ctl`, a command-line tool.

.NET 10, no dependencies beyond System.IO.Ports, trimming and NativeAOT friendly.

## The one rule

On the AIOC's K1 plug, PTT and the host-to-radio data line are the same contact. So:

- every frame the host sends starts the radio's serial PTT lock (20 ms on v2, up to 1.5 s on v1, about 6 s on stock), and
- **any byte written to the serial port while PTT is held ends the transmission.** The AIOC releases PTT the moment the host writes, and keeps it released.

The library turns this into something you cannot get wrong by accident: `BeginTransmitAsync` waits for the lock to run out and then keeps the library silent until you dispose the window.

## Quick start

```csharp
using M0LTE.Uvk5;

await using var radio = await K5Radio.OpenSerialAsync("/dev/ttyACM0");
Console.WriteLine(radio.Firmware);            // "PKTFW 1a2b3c4 (protocol v2.0)"

var status = await radio.GetStatusAsync();
Console.WriteLine($"{status.FrequencyHz / 1e6:F4} MHz, {status.Rssi}, battery {status.BatteryVolts:F2} V");

await radio.SetSettingsAsync(new RadioSettings
{
    FrequencyHz = 144_800_000,
    Power = TxPower.High,
    DeviationWide = Deviation.FromKilohertz(3.0),   // register 0x86F with the bench AIOC law
});
```

The port is always opened at 38400 8N1 with DTR and RTS low. Any other rate is refused on purpose: slower rates let runs of zero bytes hold the line low long enough to key the radio.

## For a TNC

Subscribe once at start-up. From then on the radio pushes everything, and the TNC need not send another byte (every byte costs a lock and could collide with a key-up).

```csharp
await radio.SubscribeAsync(EventSubscription.ForTnc);   // busy, bursts, TX timing, 1 s heartbeat

// CSMA input: hardware carrier detect. Null means "unknown" (not subscribed, link lost):
// treat it as "no opinion", never as busy, or a pulled cable silences the station.
radio.BusyChanged += (_, e) => csma.OnCarrier(e.Busy, e.EstimatedTime ?? e.ReceivedAt);
bool? busy = radio.ChannelBusy;
```

Transmitting, with p-persistence staying in your code:

```csharp
if (radio.ChannelBusy is not true && Random.Shared.NextDouble() < persistence)
{
    await using var window = await radio.BeginTransmitAsync(ct);  // waits out the serial lock
    ptt.Key();                                                   // AIOC HID (CM108) PTT
    await PlayFrameAsync(audio, ct);
    ptt.Unkey();
}   // disposing the window lets commands through again, after the radio has seen the release
```

While the window is open, commands from other tasks wait; a command from inside the window throws `K5TransmitInProgressException` rather than ending your transmission. If you do not use windows, `radio.IsSafeToKey`, `radio.TimeUntilSafeToKey` and `await radio.WaitUntilSafeToKeyAsync()` give the same knowledge.

Per-burst quality, and what really went on air:

```csharp
await foreach (var e in radio.ReadEventsAsync(ct))
{
    switch (e)
    {
        case RxBurstEvent b:
            log($"burst {b.Duration.TotalMilliseconds:F0} ms, RSSI {b.RssiMean.Dbm:F1} dBm (min {b.RssiMin.Dbm:F1}), noise {b.NoiseMean}");
            break;
        case TxStartEvent s when s.LateKey:
            log($"keyed {s.LockDelay.TotalMilliseconds:F0} ms late: TXDELAY ate it");
            break;
        case TxStartEvent s when s.BusyAtPress:
            log("the channel was busy when we keyed: collision risk");
            break;
        case TxEndEvent t:
            log($"{t.OnAir.TotalMilliseconds:F0} ms on air, receiver back {t.TurnaroundToReceive.TotalMilliseconds:F0} ms after release");
            break;
        case TxRefusedEvent r:
            retry(r.Reason);
            break;
    }
}
```

Radio timestamps are milliseconds since the radio booted; `e.EstimatedTime` maps them to host time using a clock estimate (heartbeats and `SyncClockAsync`) that also tracks the radio's drift (it runs from an RC oscillator).

Things the library does for you:

- **Missed events** (a frame cut by the AIOC when PTT went down) show as a sequence gap; the library fetches them with `EVENT_REPLAY` once you are not transmitting and delivers each once, marked `IsReplay`.
- **Radio reboots** (its power-on banner, or its clock restarting) raise `Rebooted`; the library says hello again and restores your subscription.
- **Busy goes to null** if heartbeats stop, so a dead link never reads as a clear or busy channel.

### Events while transmitting (LIVE_TX)

By default the AIOC drops everything the radio sends while PTT is held, so the radio holds its events until the release (they arrive marked `IsDeferred`). To get `TxStartEvent` the moment RF is ready, turn off the AIOC's RXIGNPTT (RAM only, restored on dispose) and subscribe with `LiveTx`:

```csharp
using var aioc = Aioc.AiocHid.Open(Aioc.AiocHid.FindDevices()[0]);   // Linux hidraw
using var liveTx = aioc.AllowRadioOutputWhileKeyed();                 // register 0x60 -> 0x00000100
await radio.SubscribeAsync(EventSubscription.ForTnc with { LiveTx = true });
```

### Packet.NET's IRadioControl

The library does not depend on packet.net, but an adapter to `Packet.Radio.IRadioControl` is a few lines: `Capabilities = RssiRead | CarrierSense`, `ChannelBusy => radio.ChannelBusy`, raise `CarrierSenseChanged` from `radio.BusyChanged`, and `ReadRssiDbmAsync` from `ReadRssiAsync` (but prefer `RxBurstEvent.RssiMean`: every poll costs a lock). Keying stays with the AIOC HID PTT.

## Firmware support

| | Stock | Packet v1 | Packet v2 |
|---|---|---|---|
| Identify, EEPROM read, RSSI, battery | yes | yes | yes |
| Registers | if built in | yes | yes, batched |
| Settings | no | EEPROM block, applied about 1.5 s after the port goes quiet, needs a backup | live, atomic, read back |
| Events, status, time sync, counters, tone, overrides | no | no | yes |
| Safe to key after a command | 6.5 s | 2 s | the reply says (20 ms default) |

The kind is detected from the hello reply (the `PKT2` marker, else a version starting `PKTFW`). v2-only calls on older firmware throw `K5FirmwareNotSupportedException` and send nothing. Check `radio.Firmware.Supports(K5Features.Events)` and friends first.

## Safety

- EEPROM 0x1E00 to 0x1FFF (factory calibration) is never written. Writes must be 8-byte aligned.
- EEPROM writes need a verified backup of this radio: `await radio.BackupEepromAsync()` (reads twice, compares), or `AuthorizeEepromWritesAsync(await EepromBackup.LoadAsync(path))`, which checks the backup's calibration area against a fresh read so another radio's backup is refused. Backups use k5.py's format (`.sha256` and `.json` beside the image).
- Register writes refuse 0x00, 0x30, 0x33, 0x36 to 0x39, 0x3B, 0x3C and above 0x7F, on every firmware (including legacy 0x0602, which the firmware itself leaves open).
- Trial register overrides (`AddOverridesAsync`) are bounded by time or key-ups, after which the radio reverts them itself.
- No serial keying, ever.
- `K5RadioOptions.Audit` is told about every change sent to the radio.

## Testing with the simulated radio

`SimulatedRadio` behaves like stock, v1 or v2 firmware, including the PTT lock, the AIOC's PTT rules, deferred events, replay and the settings reload. Drive it from a test:

```csharp
var time = new FakeTimeProvider();
using var sim = new SimulatedRadio(new SimulatedRadioOptions { Firmware = FirmwareKind.PacketV2, TimeProvider = time });
await using var radio = await K5Radio.ConnectAsync(sim.HostStream, new K5RadioOptions { TimeProvider = time });

sim.StartCarrier(Rssi.FromDbm(-95));     // the channel goes busy
sim.PressPtt();                           // the AIOC keys
Assert.Empty(sim.Violations);             // nothing a real radio would object to
```

`LoopbackStream.CreatePair()` gives a pty-free duplex pipe for your own fakes, and `SimulatedBootloader` tests flashers.

## Flashing

```csharp
var image = await FirmwareImage.LoadAsync("firmware.packed.bin");
await using var bl = K5Bootloader.OpenSerial("/dev/ttyACM0");    // radio powered on with PTT held
await bl.FlashAsync(image, new FlashOptions { ReallyFlash = true }, progress);
```

A dry run (the default) listens for the beacon and checks compatibility without sending anything. Only bootloader 2.00.06 is accepted unless you allow others; images are checked for a DP32G030 vector table and must end below 0xF000.

## k5ctl

```sh
dotnet build host/dotnet/M0LTE.Uvk5.slnx
alias k5ctl=host/dotnet/tools/k5ctl/bin/Debug/net10.0/k5ctl

k5ctl -p /dev/ttyACM0 info
k5ctl -p /dev/ttyACM0 status
k5ctl -p /dev/ttyACM0 watch --seconds 60
k5ctl -p /dev/ttyACM0 get
k5ctl -p /dev/ttyACM0 set frequency=144.800MHz dev-wide=3kHz power=high
k5ctl -p /dev/ttyACM0 backup backups/k5.bin
k5ctl -p /dev/ttyACM0 set mic-gain=31 --backup backups/k5.bin   # v1: an EEPROM write
k5ctl -p /dev/ttyACM0 flash firmware.packed.bin                 # dry run
k5ctl --sim v2 watch --seconds 10                               # no radio needed
```

`K5_PORT` can stand in for `-p`. `-v` traces every frame. A NativeAOT build: `dotnet publish host/dotnet/tools/k5ctl -c Release -r linux-arm64 -p:PublishAot=true`.

## Building and testing

```sh
cd host/dotnet
dotnet build
dotnet run --project tests/M0LTE.Uvk5.Tests            # all tests
dotnet run --project tests/M0LTE.Uvk5.Tests -- -class M0LTE.Uvk5.Tests.PttLockTests
```

(xunit v3 runs in-process: `dotnet test --filter` is silently ignored, so use `-class`.)
