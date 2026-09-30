using System.Globalization;
using M0LTE.Uvk5;
using M0LTE.Uvk5.Aioc;
using M0LTE.Uvk5.Bootloader;
using M0LTE.Uvk5.Simulation;

namespace K5Ctl;

/// <summary>k5ctl: drive a UV-K5 (or the in-process simulator) from the command line.</summary>
internal static class Program
{
    private const string Usage = """
        k5ctl - UV-K5 serial control (M0LTE.Uvk5)

        usage: k5ctl [-p PORT | --sim v2|v1|stock] [--plain] [-v] COMMAND [ARGS]

          -p, --port PORT    serial port (or $K5_PORT); always 38400 8N1, DTR/RTS low
          --sim KIND         use the in-process simulated radio instead of a port
          --plain            unobfuscated session
          -v                 trace every frame to stderr

        radio commands:
          info                       firmware, features, PTT lock
          status                     GET_STATUS (v2), else RSSI and battery
          watch [--seconds N] [--heartbeat MS] [--stream MS] [--live-tx [--aioc auto|HIDRAW]]
                                     subscribe and print events (v2)
          get [NAME ...]             settings (v2 parameters, v1 EEPROM settings block)
          get-stored [NAME ...]      stored settings (v2)
          set NAME=VALUE ... [--persist] [--dry-run] [--require-tx-ok] [--backup FILE]
          save | revert              persist live settings / reload them from EEPROM (v2)
          rssi | battery | counters [--clear] | time-sync | reboot
          reg-read REG ...           BK4819 registers
          reg-dump                   all BK4819 registers except 0x5F
          eeprom-read ADDR LEN
          backup FILE                full EEPROM backup (read twice, sha256 beside it)
          overrides                  list the RAM register override table (v2)
          tone FREQ_HZ GAIN MS       raw level tone (v2); 'tone stop' stops it

        bootloader and images (radio powered on with PTT held for flash):
          flash IMAGE [--really-flash] [--allow-bootloader V] [--version-string S] [--wait S]
                                     dry run unless --really-flash; no port: print the block plan
          image-info IMAGE
          pack RAW OUT --version V
          unpack PACKED OUT

        other:
          ports                      list serial ports

        names for get/set: frequency, power, bandwidth, dev-wide, dev-narrow, busy-sql-level, rx-gain,
          rx-dac-gain, tx-timeout, ptt-press, ptt-release, pa-enable-delay, pa-bias-delay,
          serial-lock, busy-source, busy-rssi-open, busy-rssi-close, busy-hang, busy-sql-raw, agc,
          afc, backlight, key-lock (or the enum names). Values: 145.025MHz, high, narrow, 0x856 or
          2.8kHz, 20ms, 30s, on/off, -105dBm, auto. There is no squelch: receive audio is always
          open, and busy-sql-level (1 to 9) only sets the chip's squelch detector used for busy.
          There is no mic gain: it is fixed at the maximum, and the deviation sets the level.
        """;

    private static async Task<int> Main(string[] args)
    {
        using var cts = new CancellationTokenSource();
        Console.CancelKeyPress += (_, e) =>
        {
            e.Cancel = true;
            cts.Cancel();
        };

        try
        {
            return await RunAsync(new Args(args), cts.Token);
        }
        catch (UsageException e)
        {
            Console.Error.WriteLine($"error: {e.Message}");
            Console.Error.WriteLine("run k5ctl --help for usage");
            return 2;
        }
        catch (OperationCanceledException) when (cts.IsCancellationRequested)
        {
            return 130;
        }
        catch (Exception e) when (e is K5Exception or NotSupportedException or InvalidOperationException or IOException or UnauthorizedAccessException or InvalidDataException or ArgumentException)
        {
            Console.Error.WriteLine($"error: {e.Message}");
            return 1;
        }
    }

    private static async Task<int> RunAsync(Args a, CancellationToken ct)
    {
        if (a.Flag("-h") || a.Flag("--help") || a.Rest.Count == 0)
        {
            Console.WriteLine(Usage);
            return a.Rest.Count == 0 && !a.Flag("-h") && !a.Flag("--help") ? 2 : 0;
        }

        string? port = a.Option("-p") ?? a.Option("--port") ?? Environment.GetEnvironmentVariable("K5_PORT");
        string? sim = a.Option("--sim");
        bool plain = a.Flag("--plain");
        bool verbose = a.Flag("-v");
        string cmd = a.Rest[0];
        var rest = a.Rest.Skip(1).ToList();

        switch (cmd)
        {
            case "ports":
                foreach (string p in K5SerialPort.ListPorts())
                {
                    Console.WriteLine(p);
                }

                return 0;
            case "image-info":
                return ImageInfo(await FirmwareImage.LoadAsync(Need(rest, 0, "IMAGE"), cancellationToken: ct));
            case "pack":
            {
                string version = a.Option("--version") ?? throw new UsageException("pack needs --version");
                byte[] raw = await File.ReadAllBytesAsync(Need(rest, 0, "RAW"), ct);
                new FirmwareImage(raw, version).Validate();
                await File.WriteAllBytesAsync(Need(rest, 1, "OUT"), FirmwareImage.Pack(raw, version), ct);
                return 0;
            }

            case "unpack":
            {
                var (raw, ver) = FirmwareImage.Unpack(await File.ReadAllBytesAsync(Need(rest, 0, "PACKED"), ct));
                await File.WriteAllBytesAsync(Need(rest, 1, "OUT"), raw, ct);
                Console.WriteLine($"version '{System.Text.Encoding.ASCII.GetString(ver).Split('\0')[0]}'");
                return 0;
            }

            case "flash":
                return await FlashAsync(a, rest, port, sim, ct);
        }

        var options = new K5RadioOptions
        {
            Obfuscate = !plain,
            Trace = verbose ? s => Console.Error.WriteLine(s) : null,
        };

        SimulatedRadio? simulated = null;
        K5Radio radio;
        if (sim is not null)
        {
            var kind = sim switch
            {
                "v2" => FirmwareKind.PacketV2,
                "v1" => FirmwareKind.PacketV1,
                "stock" => FirmwareKind.Stock,
                _ => throw new UsageException("--sim takes v2, v1 or stock"),
            };
            simulated = new SimulatedRadio(new SimulatedRadioOptions { Firmware = kind });
            radio = await K5Radio.ConnectAsync(simulated.HostStream, options, cancellationToken: ct);
        }
        else
        {
            if (string.IsNullOrEmpty(port))
            {
                throw new UsageException("--port (or $K5_PORT) or --sim is required");
            }

            radio = await K5Radio.OpenSerialAsync(port, options, ct);
        }

        try
        {
            return await RadioCommandAsync(radio, simulated, cmd, rest, a, ct);
        }
        finally
        {
            await radio.DisposeAsync();
            simulated?.Dispose();
        }
    }

    private static async Task<int> RadioCommandAsync(K5Radio radio, SimulatedRadio? sim, string cmd, List<string> rest, Args a, CancellationToken ct)
    {
        switch (cmd)
        {
            case "info":
                PrintInfo(radio);
                return 0;
            case "status":
                if (radio.Firmware.Kind == FirmwareKind.PacketV2)
                {
                    PrintStatus(await radio.GetStatusAsync(ct));
                }
                else
                {
                    var r = await radio.ReadRssiAsync(ct);
                    var b = await radio.ReadBatteryAsync(ct);
                    Console.WriteLine($"rssi        {r.Rssi}, noise {r.Noise}, glitch {r.Glitch}");
                    Console.WriteLine($"battery     {Volts(b)}");
                }

                PrintLock(radio);
                return 0;
            case "watch":
                return await WatchAsync(radio, sim, a, ct);
            case "get" or "get-stored":
            {
                var ids = rest.Select(ParseName).ToList();
                var s = await radio.GetSettingsAsync(cmd == "get-stored", ids.Count > 0 ? ids : null, ct);
                PrintSettings(s);
                return 0;
            }

            case "set":
                return await SetAsync(radio, rest, a, ct);
            case "save":
                Console.WriteLine("saved: " + string.Join(", ", await radio.SaveSettingsAsync(ct)));
                return 0;
            case "revert":
                Console.WriteLine("reverted: " + string.Join(", ", await radio.RevertSettingsAsync(ct)));
                return 0;
            case "rssi":
            {
                var r = await radio.ReadRssiAsync(ct);
                Console.WriteLine($"rssi {r.Rssi}, noise {r.Noise}, glitch {r.Glitch}");
                return 0;
            }

            case "battery":
                Console.WriteLine($"battery {Volts(await radio.ReadBatteryAsync(ct))}");
                return 0;
            case "counters":
            {
                var c = await radio.GetCountersAsync(a.Flag("--clear"), ct);
                string[] names = ["frames accepted", "frames bad", "frames dropped", "error replies", "events stored", "events lost", "events deferred",
                    "transmissions", "TX timeouts", "TX refused", "busy opens", "late keys", "ephemeral dropped", "EEPROM blocks written"];
                for (int i = 0; i < c.Raw.Count; i++)
                {
                    Console.WriteLine($"{(i < names.Length ? names[i] : $"counter {i}"),-22} {c.Raw[i]}");
                }

                return 0;
            }

            case "time-sync":
            {
                var r = await radio.SyncClockAsync(8, ct);
                Console.WriteLine($"radio booted at {r.RadioBootTime:yyyy-MM-dd HH:mm:ss.fff} UTC, +/-{r.Uncertainty.TotalMilliseconds:F2} ms ({r.Exchanges} exchanges)");
                return 0;
            }

            case "reboot":
                await radio.RebootAsync(ct);
                return 0;
            case "reg-read":
                foreach (string reg in rest)
                {
                    int r = ParseInt(reg);
                    Console.WriteLine($"REG_{r:X2} = 0x{await radio.ReadRegisterAsync(r, ct):X4}");
                }

                return 0;
            case "reg-dump":
            {
                ushort[] all = await radio.ReadRegistersAsync(0, 0x80, false, ct);
                for (int r = 0; r < 0x80; r++)
                {
                    Console.WriteLine(r == 0x5F ? "REG_5F = (FSK FIFO, not read)" : $"REG_{r:X2} = 0x{all[r]:X4}");
                }

                return 0;
            }

            case "eeprom-read":
            {
                int addr = ParseInt(Need(rest, 0, "ADDR"));
                byte[] data = await radio.ReadEepromAsync(addr, ParseInt(Need(rest, 1, "LEN")), ct);
                for (int i = 0; i < data.Length; i += 16)
                {
                    Console.WriteLine($"{addr + i:X4}  {Convert.ToHexString(data.AsSpan(i, Math.Min(16, data.Length - i))).ToLowerInvariant()}");
                }

                return 0;
            }

            case "backup":
            {
                string path = Need(rest, 0, "FILE");
                var b = await radio.BackupEepromAsync(new Progress<double>(p => Console.Error.Write($"\rreading {p * 100:F0}%   ")), ct);
                Console.Error.WriteLine();
                await b.SaveAsync(path, ct);
                Console.WriteLine($"backup {path} sha256 {b.Sha256} (two reads matched)");
                return 0;
            }

            case "overrides":
            {
                var t = await radio.GetOverridesAsync(ct);
                Console.WriteLine($"RAM ({t.Ram.Count}), expires in {(t.ExpiresIn is { } e ? $"{e.TotalSeconds:F0} s" : "never")}, key-ups left {(t.KeyUpsLeft?.ToString(CultureInfo.InvariantCulture) ?? "no bound")}");
                foreach (var o in t.Ram)
                {
                    Console.WriteLine($"  {o.Phase,-6} REG_{o.Register:X2} &0x{o.AndMask:X4} |0x{o.OrValue:X4}");
                }

                return 0;
            }

            case "tone":
                if (rest.Count == 1 && rest[0] == "stop")
                {
                    await radio.StopLevelToneAsync(ct);
                    return 0;
                }

                var tone = await radio.StartLevelToneRawAsync(ParseInt(Need(rest, 0, "FREQ_HZ")), ParseInt(Need(rest, 1, "GAIN")), TimeSpan.FromMilliseconds(ParseInt(Need(rest, 2, "MS"))), ct);
                Console.WriteLine($"tone: gain code {tone.GainCode}, REG_71 0x{tone.FrequencyWord:X4}");
                return 0;
            default:
                throw new UsageException($"unknown command '{cmd}'");
        }
    }

    private static void PrintInfo(K5Radio radio)
    {
        var f = radio.Firmware;
        Console.WriteLine($"firmware    {f}");
        Console.WriteLine($"kind        {f.Kind}");
        Console.WriteLine($"features    {f.Features}");
        if (f.Kind == FirmwareKind.PacketV2)
        {
            Console.WriteLine($"caps        {f.Capabilities}");
            Console.WriteLine($"serial lock {f.SerialLock.TotalMilliseconds:F0} ms, late key up to {f.LateKeyMax.TotalMilliseconds:F0} ms");
            Console.WriteLine($"events      {f.SupportedEvents}");
            Console.WriteLine($"ring        {f.EventRingCapacity} events, max request body {f.MaxRequestBody} bytes");
            Console.WriteLine($"TX band     plan {f.TxBandPlan}, flags 0x{f.TxBandFlags:X2}");
            Console.WriteLine($"EEPROM      settings layout {f.SettingsBlockLayout}, v2 layout {f.V2BlockLayout}");
        }
        else
        {
            Console.WriteLine($"serial lock up to {f.SerialLock.TotalSeconds:F1} s after each frame (this library waits {(f.Kind == FirmwareKind.PacketV1 ? "2.0" : "6.5")} s before keying)");
        }

        if (f.InLockScreen)
        {
            Console.WriteLine("lock screen is showing");
        }

        PrintLock(radio);
    }

    private static void PrintLock(K5Radio radio) =>
        Console.WriteLine($"safe to key in {radio.TimeUntilSafeToKey.TotalMilliseconds:F0} ms (writing to the port while keyed ends the transmission)");

    private static void PrintStatus(RadioStatus s)
    {
        Console.WriteLine($"frequency   {s.FrequencyHz / 1e6:F5} MHz, channel {s.Channel}");
        Console.WriteLine($"state       {s.State} ({s.Flags})");
        Console.WriteLine($"flags2      {s.Flags2}");
        Console.WriteLine($"power       {s.Power}, bandwidth {s.Bandwidth}, busy detector level {s.BusySquelchLevel}");
        Console.WriteLine($"deviation   {s.Deviation}");
        Console.WriteLine($"rssi        {s.Rssi}, noise {s.Noise}, glitch {s.Glitch}, AGC {s.Agc}");
        Console.WriteLine($"battery     {s.BatteryVolts:F2} V, level {s.BatteryLevel}");
        Console.WriteLine($"TX timeout  {s.TxTimeout.TotalSeconds:F0} s{(s.TxTimeLeft is { } left ? $", {left.TotalSeconds:F1} s left" : string.Empty)}");
        Console.WriteLine($"busy age    {(s.BusyAge == TimeSpan.MaxValue ? "over 65 s" : $"{s.BusyAge.TotalMilliseconds:F0} ms")}");
        Console.WriteLine($"uptime      {s.Uptime:g}, next event seq {s.NextEventSequence}");
    }

    private static void PrintSettings(RadioSettings s)
    {
        void P(string name, object? v)
        {
            if (v is not null)
            {
                Console.WriteLine($"{name,-16} {v}");
            }
        }

        P("frequency", s.FrequencyHz is { } f ? $"{f / 1e6:F5} MHz" : null);
        P("power", s.Power);
        P("bandwidth", s.Bandwidth);
        P("dev-wide", s.DeviationWide);
        P("dev-narrow", s.DeviationNarrow);
        P("rx-gain", s.RxGain);
        P("rx-dac-gain", s.RxDacGain);
        P("tx-timeout", Ms(s.TxTimeout, "s"));
        P("ptt-press", Ms(s.PttPressDebounce));
        P("ptt-release", Ms(s.PttReleaseDebounce));
        P("pa-enable-delay", Ms(s.PaEnableDelay));
        P("pa-bias-delay", Ms(s.PaBiasDelay));
        P("serial-lock", Ms(s.SerialLock));
        P("busy-source", s.BusySource);
        P("busy-rssi-open", s.BusyRssiOpen);
        P("busy-rssi-close", s.BusyRssiClose);
        P("busy-hang", Ms(s.BusyHang));
        P("busy-sql-level", s.BusySquelchLevel);
        P("busy-sql-raw", s.BusySquelchThresholds is { } q ? $"rssi {q.RssiOpen}/{q.RssiClose}, noise {q.NoiseOpen}/{q.NoiseClose}, glitch {q.GlitchOpen}/{q.GlitchClose}" : null);
        P("agc", s.Agc);
        P("afc", s.Afc is { } afc ? (afc ? "on" : "off") : null);
        P("backlight", s.Backlight);
        P("key-lock", s.KeyLock is { } kl ? (kl ? "on" : "off") : null);
    }

    private static string? Ms(TimeSpan? t, string unit = "ms") =>
        t is { } v ? (unit == "s" ? $"{v.TotalSeconds:F0} s" : $"{v.TotalMilliseconds:F0} ms") : null;

    private static string Volts(BatteryReading b) =>
        (b.Volts is { } v ? $"{v:F2} V" : "unknown (no calibration)") + (b.RawAdc is { } raw ? $" (ADC {raw})" : string.Empty) + (b.Level is { } l ? $", level {l}" : string.Empty);

    private static async Task<int> SetAsync(K5Radio radio, List<string> rest, Args a, CancellationToken ct)
    {
        if (rest.Count == 0)
        {
            throw new UsageException("set needs NAME=VALUE");
        }

        var changes = new RadioSettings();
        foreach (string kv in rest)
        {
            int eq = kv.IndexOf('=', StringComparison.Ordinal);
            if (eq <= 0)
            {
                throw new UsageException($"'{kv}' is not NAME=VALUE");
            }

            changes = changes.Merge(ParseSetting(ParseName(kv[..eq]), kv[(eq + 1)..], radio.DeviationLaw));
        }

        if (radio.Firmware.Kind == FirmwareKind.PacketV1 && a.Option("--backup") is null && !a.Flag("--dry-run"))
        {
            throw new UsageException("on packet firmware v1 settings are EEPROM writes: pass --backup FILE (a backup of this radio made with 'k5ctl backup FILE')");
        }

        if (a.Option("--backup") is { } backupPath)
        {
            await radio.AuthorizeEepromWritesAsync(await EepromBackup.LoadAsync(backupPath, ct), ct);
        }

        var flags = (a.Flag("--persist") ? SetSettingsFlags.Persist : 0) | (a.Flag("--dry-run") ? SetSettingsFlags.DryRun : 0)
            | (a.Flag("--require-tx-ok") ? SetSettingsFlags.RequireTxAllowed : 0);
        var r = await radio.SetSettingsAsync(changes, flags, ct);
        PrintSettings(r.Applied);
        if (r.TxAllowed is { } tx)
        {
            Console.WriteLine($"TX allowed here: {(tx ? "yes" : "no")}{(r.PersistQueued ? ", persist queued" : string.Empty)}{(r.Retuned ? ", retuned" : string.Empty)}");
        }

        if (r.AppliesAfterQuiet > TimeSpan.Zero)
        {
            Console.WriteLine($"written to the EEPROM settings block; the radio applies it about {r.AppliesAfterQuiet.TotalSeconds:F1} s after the port goes quiet");
        }

        return 0;
    }

    private static async Task<int> WatchAsync(K5Radio radio, SimulatedRadio? sim, Args a, CancellationToken ct)
    {
        if (radio.Firmware.Kind != FirmwareKind.PacketV2)
        {
            Console.Error.WriteLine("events need protocol v2 firmware; this radio has only polled commands (each costs a PTT lock)");
            return 1;
        }

        int hb = a.Option("--heartbeat") is { } h ? ParseInt(h) : 1000;
        int stream = a.Option("--stream") is { } s ? ParseInt(s) : 0;
        bool liveTx = a.Flag("--live-tx");
        IDisposable? aiocScope = null;
        AiocHid? aioc = null;
        if (liveTx && a.Option("--aioc") is { } aiocPath)
        {
            if (!OperatingSystem.IsLinux())
            {
                throw new UsageException("--aioc needs Linux hidraw");
            }

            string dev = aiocPath == "auto" ? AiocHid.FindDevices().FirstOrDefault() ?? throw new IOException("no AIOC hidraw device found") : aiocPath;
            aioc = AiocHid.Open(dev);
            aiocScope = aioc.AllowRadioOutputWhileKeyed();
            Console.Error.WriteLine($"AIOC {dev}: RXIGNPTT off in RAM for this session (restored on exit)");
        }

        try
        {
            var sub = new EventSubscription
            {
                Events = RadioEvents.Tnc | (stream > 0 ? RadioEvents.RssiStream : RadioEvents.None),
                HeartbeatPeriod = hb > 0 ? TimeSpan.FromMilliseconds(hb) : null,
                RssiStreamPeriod = stream > 0 ? TimeSpan.FromMilliseconds(stream) : null,
                LiveTx = liveTx,
            };
            var info = await radio.SubscribeAsync(sub, ct);
            Console.WriteLine($"subscribed: next seq {info.NextSequence}, radio time {info.RadioTimeMs} ms, channel {(radio.ChannelBusy is true ? "busy" : "clear")}; Ctrl+C to stop");
            using var stop = CancellationTokenSource.CreateLinkedTokenSource(ct);
            if (a.Option("--seconds") is { } secs)
            {
                stop.CancelAfter(TimeSpan.FromSeconds(double.Parse(secs, CultureInfo.InvariantCulture)));
            }

            if (sim is not null)
            {
                _ = DemoTrafficAsync(sim, stop.Token);
            }

            try
            {
                await foreach (var e in radio.ReadEventsAsync(stop.Token))
                {
                    Console.WriteLine(Describe(e));
                }
            }
            catch (OperationCanceledException) when (!ct.IsCancellationRequested)
            {
            }

            return 0;
        }
        finally
        {
            aiocScope?.Dispose();
            aioc?.Dispose();
        }
    }

    private static async Task DemoTrafficAsync(SimulatedRadio sim, CancellationToken ct)
    {
        try
        {
            var rnd = new Random(1);
            while (!ct.IsCancellationRequested)
            {
                await Task.Delay(1500, ct);
                sim.StartCarrier(Rssi.FromDbm(-110 + rnd.Next(40)), rnd.Next(5, 30), rnd.Next(0, 10));
                await Task.Delay(150 + rnd.Next(300), ct);
                sim.StopCarrier();
            }
        }
        catch (OperationCanceledException)
        {
        }
    }

    private static string Describe(RadioEvent e)
    {
        string t = (e.EstimatedTime ?? e.ReceivedAt).ToUniversalTime().ToString("HH:mm:ss.fff", CultureInfo.InvariantCulture);
        string head = $"{t} #{e.Sequence,-5} {e.RadioTimeMs,10} ms{(e.IsReplay ? " REPLAY" : string.Empty)}{(e.IsDeferred ? " DEFERRED" : string.Empty)}";
        string body = e switch
        {
            BusyEvent b => $"BUSY {(b.Busy ? "open " : "close")} {b.Rssi} noise {b.Noise} glitch {b.Glitch} cause {b.Cause}",
            RxBurstEvent r => $"BURST {r.Duration.TotalMilliseconds:F0} ms, {r.Samples} samples, RSSI mean {r.RssiMean.Dbm:F1} max {r.RssiMax.Dbm:F1} min {r.RssiMin.Dbm:F1} dBm, noise {r.NoiseMean}/{r.NoiseMin}, glitch {r.GlitchMean}/{r.GlitchMax}"
                + (r.FrequencyErrorHz is { } fe ? $", freq error {fe} Hz" : string.Empty),
            TxStartEvent s => $"TX START {s.FrequencyHz / 1e6:F5} MHz {s.Power} {s.Bandwidth} dev 0x{s.Deviation.Register:X3}, press to RF {s.KeyUpLatency.TotalMilliseconds:F0} ms"
                + (s.LateKey ? $", LATE by {s.LockDelay.TotalMilliseconds:F0} ms" : string.Empty) + (s.BusyAtPress ? ", CHANNEL WAS BUSY" : string.Empty),
            TxEndEvent x => $"TX END {x.Reason}, on air {x.OnAir.TotalMilliseconds:F0} ms, release to carrier off {x.KeyDownLatency.TotalMilliseconds:F0} ms, to RX ready {x.TurnaroundToReceive.TotalMilliseconds:F0} ms",
            TxRefusedEvent x => $"TX REFUSED {x.Reason}" + (x.Reason == TxRefusedReason.Lock ? $" ({x.LockRemaining.TotalMilliseconds:F0} ms of lock left)"
                : x.Reason == TxRefusedReason.Late ? $" ({x.LateBy.TotalMilliseconds:F0} ms after the press)" : string.Empty),
            HeartbeatEvent h => $"HEARTBEAT {h.State} {h.Rssi} battery {h.BatteryMillivolts} mV, busy {(h.BusyTime is { } bt ? $"{bt.TotalMilliseconds:F0} ms" : "unknown")}, lock {h.LockRemaining.TotalMilliseconds:F0} ms",
            RssiStreamEvent r => $"RSSI {string.Join(" ", r.Samples.Select(x => x.Rssi.Dbm.ToString("F0", CultureInfo.InvariantCulture)))} dBm every {r.Period.TotalMilliseconds:F0} ms",
            BatteryEvent b => $"BATTERY {b.Class} level {b.Level}, {b.Millivolts} mV",
            ParamsChangedEvent p => $"PARAMS CHANGED by {p.Source}: {string.Join(", ", p.Parameters)}",
            EventsLostEvent l => $"EVENTS LOST {l.Count} from #{l.FirstLost}",
            ToneEndEvent x => $"TONE END {x.Reason}",
            OverrideExpiredEvent o => $"OVERRIDES EXPIRED ({(o.ByKeyUps ? "key-ups" : "time")}), {o.EntriesReverted} reverted",
            BootEvent b => $"BOOT protocol {b.ProtocolVersion}, cause {b.ResetCause}",
            UnknownEvent u => $"UNKNOWN 0x{u.Id:X4} {Convert.ToHexString(u.Body.Span)}",
            _ => e.GetType().Name,
        };
        return $"{head}  {body}";
    }

    private static async Task<int> FlashAsync(Args a, List<string> rest, string? port, string? sim, CancellationToken ct)
    {
        var image = await FirmwareImage.LoadAsync(Need(rest, 0, "IMAGE"), a.Option("--version-string"), ct);
        image.Validate();
        if (string.IsNullOrEmpty(port) && sim is null)
        {
            Console.WriteLine($"offline dry run (no port): {rest[0]}, {image.Raw.Length} bytes, version '{image.Version}'");
            foreach (string line in image.BlockPlan())
            {
                Console.WriteLine(line);
            }

            return 0;
        }

        var allowed = new List<string> { "2.00.06" };
        allowed.AddRange(a.Options("--allow-bootloader"));
        var options = new FlashOptions
        {
            ReallyFlash = a.Flag("--really-flash"),
            AllowedBootloaders = allowed,
            BeaconTimeout = TimeSpan.FromSeconds(a.Option("--wait") is { } w ? double.Parse(w, CultureInfo.InvariantCulture) : 10),
            Audit = e => Console.Error.WriteLine($"audit: {e.Description}: {e.Outcome}"),
        };

        SimulatedBootloader? simBl = sim is not null ? new SimulatedBootloader() : null;
        await using var bl = simBl is not null ? new K5Bootloader(simBl.HostStream) : K5Bootloader.OpenSerial(port!);
        try
        {
            Console.WriteLine($"image {rest[0]}: {image.Raw.Length} bytes, {image.BlockCount} blocks, version '{image.Version}', sha256 {image.Sha256}");
            var progress = new Progress<FlashProgress>(p => Console.Error.Write($"\r{p.Stage} {p.BlocksDone}/{p.BlockCount}      "));
            var result = await bl.FlashAsync(image, options, progress, ct);
            Console.Error.WriteLine();
            Console.WriteLine($"bootloader {result.Beacon.Version}, chip {result.Beacon.ChipId}");
            Console.WriteLine(result.Flashed ? "flashed; the radio restarts into the new firmware" : "dry run: bootloader compatible, nothing sent. Use --really-flash to write.");
            return 0;
        }
        finally
        {
            simBl?.Dispose();
        }
    }

    private static int ImageInfo(FirmwareImage img)
    {
        img.Validate();
        Console.WriteLine($"kind       {(img.WasPacked ? "packed" : "raw")}");
        Console.WriteLine($"raw size   {img.Raw.Length} bytes of {FirmwareImage.FlashLimit} ({FirmwareImage.FlashLimit - img.Raw.Length} free)");
        Console.WriteLine($"blocks     {img.BlockCount} x 256");
        Console.WriteLine($"version    '{img.Version}'");
        Console.WriteLine($"raw sha256 {img.Sha256}");
        return 0;
    }

    private static RadioParameterId ParseName(string name)
    {
        string n = name.Replace("-", string.Empty, StringComparison.Ordinal).Replace("_", string.Empty, StringComparison.Ordinal).ToLowerInvariant();
        return n switch
        {
            "frequency" or "freq" or "frequencyhz" => RadioParameterId.FrequencyHz,
            "devwide" or "deviationwide" => RadioParameterId.DeviationWide,
            "devnarrow" or "deviationnarrow" => RadioParameterId.DeviationNarrow,
            "txtimeout" or "txtimeoutseconds" => RadioParameterId.TxTimeoutSeconds,
            "pttpress" or "pttpressms" => RadioParameterId.PttPressMs,
            "pttrelease" or "pttreleasems" => RadioParameterId.PttReleaseMs,
            "paenabledelay" or "paenabledelayms" => RadioParameterId.PaEnableDelayMs,
            "pabiasdelay" or "pabiasdelayms" => RadioParameterId.PaBiasDelayMs,
            "seriallock" or "seriallockms" => RadioParameterId.SerialLockMs,
            "busyhang" or "busyhangms" => RadioParameterId.BusyHangMs,
            "busysqlraw" or "busysquelchraw" or "sqlraw" => RadioParameterId.BusySquelchRaw,
            "busysqllevel" or "busysquelchlevel" or "busylevel" => RadioParameterId.BusySquelchLevel,
            "squelch" or "sql" => throw new UsageException("there is no squelch: receive audio is always open. busy-sql-level (1 to 9) sets the busy detector"),
            "micgain" or "mic" => throw new UsageException("there is no mic gain setting: the firmware fixes it at the maximum. dev-wide and dev-narrow set the transmit level"),
            "agc" or "agcfix" => RadioParameterId.AgcFix,
            _ => Enum.TryParse<RadioParameterId>(n, ignoreCase: true, out var id) && Enum.IsDefined(id) ? id : throw new UsageException($"unknown setting '{name}'"),
        };
    }

    private static RadioSettings ParseSetting(RadioParameterId id, string v, DeviationLaw law)
    {
        string s = v.Trim().ToLowerInvariant();
        TimeSpan Millis(string unit = "ms") => s.EndsWith("ms", StringComparison.Ordinal) ? TimeSpan.FromMilliseconds(ParseDouble(s[..^2]))
            : s.EndsWith('s') ? TimeSpan.FromSeconds(ParseDouble(s[..^1]))
            : unit == "s" ? TimeSpan.FromSeconds(ParseDouble(s)) : TimeSpan.FromMilliseconds(ParseDouble(s));
        bool Bool() => s switch
        {
            "on" or "true" or "1" or "yes" => true,
            "off" or "false" or "0" or "no" => false,
            _ => throw new UsageException($"'{v}' is not on/off"),
        };
        Rssi Level() => s.EndsWith("dbm", StringComparison.Ordinal) ? Rssi.FromDbm(ParseDouble(s[..^3])) : new Rssi((ushort)ParseInt(s));
        Deviation Dev() => s.EndsWith("khz", StringComparison.Ordinal) ? Deviation.FromKilohertz(ParseDouble(s[..^3]), law) : new Deviation((ushort)ParseInt(s), law);

        return id switch
        {
            RadioParameterId.FrequencyHz => new RadioSettings
            {
                FrequencyHz = s.EndsWith("mhz", StringComparison.Ordinal) ? (long)Math.Round(ParseDouble(s[..^3]) * 1e6)
                    : s.EndsWith("khz", StringComparison.Ordinal) ? (long)Math.Round(ParseDouble(s[..^3]) * 1e3)
                    : s.Contains('.', StringComparison.Ordinal) ? (long)Math.Round(ParseDouble(s) * 1e6) : ParseInt(s),
            },
            RadioParameterId.Power => new RadioSettings { Power = Enum.Parse<TxPower>(s, ignoreCase: true) },
            RadioParameterId.Bandwidth => new RadioSettings { Bandwidth = Enum.Parse<Bandwidth>(s, ignoreCase: true) },
            RadioParameterId.DeviationWide => new RadioSettings { DeviationWide = Dev() },
            RadioParameterId.DeviationNarrow => new RadioSettings { DeviationNarrow = Dev() },
            RadioParameterId.BusySquelchLevel => new RadioSettings { BusySquelchLevel = ParseInt(s) },
            RadioParameterId.RxGain => new RadioSettings { RxGain = ParseInt(s) },
            RadioParameterId.RxDacGain => new RadioSettings { RxDacGain = ParseInt(s) },
            RadioParameterId.TxTimeoutSeconds => new RadioSettings { TxTimeout = Millis("s") },
            RadioParameterId.PttPressMs => new RadioSettings { PttPressDebounce = Millis() },
            RadioParameterId.PttReleaseMs => new RadioSettings { PttReleaseDebounce = Millis() },
            RadioParameterId.PaEnableDelayMs => new RadioSettings { PaEnableDelay = Millis() },
            RadioParameterId.PaBiasDelayMs => new RadioSettings { PaBiasDelay = Millis() },
            RadioParameterId.SerialLockMs => new RadioSettings { SerialLock = Millis() },
            RadioParameterId.BusySource => new RadioSettings
            {
                BusySource = s switch
                {
                    "squelch" => BusySources.Squelch,
                    "rssi" => BusySources.Rssi,
                    "both" or "squelch,rssi" => BusySources.Squelch | BusySources.Rssi,
                    _ => (BusySources)ParseInt(s),
                },
            },
            RadioParameterId.BusyRssiOpen => new RadioSettings { BusyRssiOpen = Level() },
            RadioParameterId.BusyRssiClose => new RadioSettings { BusyRssiClose = Level() },
            RadioParameterId.BusyHangMs => new RadioSettings { BusyHang = Millis() },
            RadioParameterId.BusySquelchRaw => new RadioSettings
            {
                BusySquelchThresholds = s.Split(',') is { Length: 6 } p
                    ? new BusySquelchThresholds((byte)ParseInt(p[0]), (byte)ParseInt(p[1]), (byte)ParseInt(p[2]), (byte)ParseInt(p[3]), (byte)ParseInt(p[4]), (byte)ParseInt(p[5]))
                    : throw new UsageException("busy-sql-raw takes six numbers: rssi open,close, noise open,close, glitch open,close"),
            },
            RadioParameterId.AgcFix => new RadioSettings { Agc = s == "auto" ? AgcSetting.Auto : AgcSetting.Fixed(ParseInt(s)) },
            RadioParameterId.Afc => new RadioSettings { Afc = Bool() },
            RadioParameterId.Backlight => new RadioSettings { Backlight = ParseInt(s) },
            RadioParameterId.KeyLock => new RadioSettings { KeyLock = Bool() },
            _ => throw new UsageException($"cannot set {id}"),
        };
    }

    private static string Need(List<string> rest, int i, string what) =>
        i < rest.Count ? rest[i] : throw new UsageException($"missing {what}");

    private static int ParseInt(string s)
    {
        s = s.Trim();
        return s.StartsWith("0x", StringComparison.OrdinalIgnoreCase)
            ? int.Parse(s.AsSpan(2), NumberStyles.HexNumber, CultureInfo.InvariantCulture)
            : int.Parse(s, NumberStyles.Integer, CultureInfo.InvariantCulture);
    }

    private static double ParseDouble(string s) => double.Parse(s.Trim(), NumberStyles.Float, CultureInfo.InvariantCulture);

    private sealed class UsageException(string message) : Exception(message);

    /// <summary>Minimal argument parsing: options with values, flags, and positional arguments.</summary>
    private sealed class Args
    {
        private static readonly HashSet<string> WithValue = ["-p", "--port", "--sim", "--seconds", "--heartbeat", "--stream", "--aioc", "--backup", "--allow-bootloader", "--version-string", "--wait", "--version"];
        private readonly List<(string Name, string? Value)> _opts = [];

        public Args(string[] args)
        {
            for (int i = 0; i < args.Length; i++)
            {
                string x = args[i];
                if (x.StartsWith('-') && x.Length > 1 && !char.IsDigit(x[1]))
                {
                    string? value = null;
                    if (WithValue.Contains(x))
                    {
                        value = i + 1 < args.Length ? args[++i] : throw new UsageException($"{x} needs a value");
                    }

                    _opts.Add((x, value));
                }
                else
                {
                    Rest.Add(x);
                }
            }
        }

        public List<string> Rest { get; } = [];

        public bool Flag(string name) => _opts.Any(o => o.Name == name);

        public string? Option(string name) => _opts.LastOrDefault(o => o.Name == name).Value;

        public IEnumerable<string> Options(string name) => _opts.Where(o => o.Name == name && o.Value is not null).Select(o => o.Value!);
    }
}
