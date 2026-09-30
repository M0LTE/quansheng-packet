using System.Buffers.Binary;
using M0LTE.Uvk5.Protocol;

namespace M0LTE.Uvk5;

public sealed partial class K5Radio
{
    /// <summary>
    /// Says hello, identifies the firmware from the reply (PKT2 marker: v2; version starting
    /// <c>PKTFW</c>: v1; anything else: stock) and on v2 reads GET_INFO. Resets the radio's live
    /// event subscription to its stored default (a hello always does).
    /// </summary>
    public async Task<FirmwareInfo> IdentifyAsync(CancellationToken cancellationToken = default)
    {
        byte[] body = new byte[4];
        BinaryPrimitives.WriteUInt32LittleEndian(body, _sessionId);

        // The radio takes its mode from how the hello arrives, so set the decoder first.
        _decoder.Obfuscated = _options.Obfuscate;
        K5Frame reply = await LegacyAsync("hello", MessageIds.Hello, body, f => f.Id == MessageIds.HelloReply, cancellationToken).ConfigureAwait(false);
        var r = new WireReader(reply.Body.Span, "hello reply");
        string version = r.Ascii(16);
        bool aes = r.U8() != 0;
        bool lockScreen = r.U8() != 0;
        r.Bytes(2);
        uint magic = r.Remaining >= 4 ? r.U32() : 0;
        ushort proto = r.Remaining >= 2 ? r.U16() : (ushort)0;

        FirmwareKind kind = magic == MessageIds.Pkt2Magic ? FirmwareKind.PacketV2
            : version.StartsWith("PKTFW", StringComparison.Ordinal) ? FirmwareKind.PacketV1
            : FirmwareKind.Stock;

        var info = new FirmwareInfo
        {
            Kind = kind,
            Version = version,
            InLockScreen = lockScreen,
            HasCustomAesKey = aes,
            ProtocolVersion = kind == FirmwareKind.PacketV2 ? new Version(proto >> 8, proto & 0xFF) : null,
        };

        _lock.SetFirmware(kind, null);
        _lock.OnLegacyReply();
        _sequencer.Reset();
        lock (_gate)
        {
            _channelBusy = null;
            _busySubscribed = false;
        }

        if (kind == FirmwareKind.PacketV2)
        {
            Firmware = info;
            info = await GetInfoAsync(info, cancellationToken).ConfigureAwait(false);
        }
        else
        {
            info = info with
            {
                Features = K5Features.Eeprom | K5Features.LegacyRssi | K5Features.Battery
                    | (kind == FirmwareKind.PacketV1 ? K5Features.Registers | K5Features.Settings : K5Features.None),
                SupportedParameters = kind == FirmwareKind.PacketV1 ? new HashSet<RadioParameterId>(SettingsBlock.V1Parameters) : new HashSet<RadioParameterId>(),
                SerialLock = kind == FirmwareKind.PacketV1 ? TimeSpan.FromSeconds(1.5) : TimeSpan.FromSeconds(6.5),
            };
        }

        Firmware = info;
        return info;
    }

    private async Task<FirmwareInfo> GetInfoAsync(FirmwareInfo hello, CancellationToken ct)
    {
        byte[] b = await V2Async("GET_INFO", MessageIds.GetInfo, [], true, ct).ConfigureAwait(false);
        var r = new WireReader(b, "GET_INFO reply");
        ushort proto = r.U16();
        string version = r.Ascii(16);
        var caps = (RadioCapabilities)r.U32();
        uint pmask = r.U32();
        var events = (RadioEvents)r.U32();
        int maxBody = r.U8();
        int ring = r.U8();
        int lockMs = r.U16();
        int lateMax = r.U8();
        int bandPlan = r.U8();
        int bandFlags = r.U8();
        int layout = r.U8();
        int v2layout = r.U8();
        int burstPeriod = r.U8();

        var ids = new HashSet<RadioParameterId>(EventParser.MaskToIds(pmask));
        var features = K5Features.Eeprom | K5Features.LegacyRssi | K5Features.Battery | K5Features.Registers | K5Features.Settings
            | K5Features.LiveSettings | K5Features.Status | K5Features.Events | K5Features.TimeSync | K5Features.Counters | K5Features.ExactPttLock;
        if ((caps & (RadioCapabilities.LevelToneRaw | RadioCapabilities.LevelToneCalibrated)) != 0)
        {
            features |= K5Features.LevelTone;
        }

        if ((caps & RadioCapabilities.RamRegisterOverrides) != 0)
        {
            features |= K5Features.RegisterOverrides;
        }

        _lock.SetFirmware(FirmwareKind.PacketV2, TimeSpan.FromMilliseconds(lockMs));
        return hello with
        {
            ProtocolVersion = new Version(proto >> 8, proto & 0xFF),
            Version = string.IsNullOrEmpty(version) ? hello.Version : version,
            Capabilities = caps,
            SupportedParameters = ids,
            SupportedEvents = events,
            Features = features,
            MaxRequestBody = maxBody,
            EventRingCapacity = ring,
            SerialLock = TimeSpan.FromMilliseconds(lockMs),
            LateKeyMax = TimeSpan.FromMilliseconds(lateMax),
            TxBandPlan = bandPlan,
            TxBandFlags = bandFlags,
            SettingsBlockLayout = layout,
            V2BlockLayout = v2layout,
            DefaultBurstSamplePeriod = TimeSpan.FromMilliseconds(burstPeriod),
        };
    }

    /// <summary>GET_STATUS (v2).</summary>
    public async Task<RadioStatus> GetStatusAsync(CancellationToken cancellationToken = default)
    {
        byte[] b = await V2Async("GET_STATUS", MessageIds.GetStatus, [], true, cancellationToken).ConfigureAwait(false);
        var r = new WireReader(b, "GET_STATUS reply");
        var s = new RadioStatus
        {
            Uptime = TimeSpan.FromMilliseconds(r.U32()),
            FrequencyHz = r.U32(),
            State = (RadioState)r.U8(),
            Flags = (StatusFlags)r.U8(),
            Flags2 = (StatusFlags2)r.U8(),
            Power = (TxPower)r.U8(),
            Bandwidth = (Bandwidth)r.U8(),
            BusySquelchLevel = r.U8(),
            Deviation = new Deviation((ushort)Math.Min(r.U16(), Deviation.MaxRegister), _options.DeviationLaw),
            Rssi = new Rssi(r.U16()),
            Noise = r.U8(),
            Glitch = r.U8(),
            Agc = new AgcReading(r.U8()),
            BatteryLevel = r.U8(),
            BatteryMillivolts = r.U16(),
            LockRemaining = TimeSpan.FromMilliseconds(r.U16()),
        };
        ushort txLeft = r.U16();
        ushort busyAge = r.U16();
        s = s with
        {
            TxTimeLeft = txLeft == 0xFFFF ? null : TimeSpan.FromMilliseconds(txLeft * 100.0),
            BusyAge = busyAge == 0xFFFF ? TimeSpan.MaxValue : TimeSpan.FromMilliseconds(busyAge),
            NextEventSequence = r.U16(),
            Channel = r.U8(),
            TxTimeout = TimeSpan.FromSeconds(r.U8()),
        };

        lock (_gate)
        {
            if (_busySubscribed)
            {
                _channelBusy = s.IsBusy;
            }
        }

        return s;
    }

    /// <summary>
    /// SUBSCRIBE (v2): chooses which events the radio sends. Remembered, and restored after a
    /// reboot if <see cref="K5RadioOptions.ResubscribeAfterReboot"/>. With busy events, also reads
    /// the current busy state so <see cref="ChannelBusy"/> is known at once.
    /// </summary>
    public async Task<SubscriptionInfo> SubscribeAsync(EventSubscription subscription, CancellationToken cancellationToken = default)
    {
        ArgumentNullException.ThrowIfNull(subscription);
        RadioEvents mask = subscription.Events;
        int hb = 0;
        if (subscription.HeartbeatPeriod is { } hp)
        {
            hb = (int)hp.TotalMilliseconds;
            if (hb is < 100 or > 60000)
            {
                throw new ArgumentOutOfRangeException(nameof(subscription), hp, "HeartbeatPeriod must be 100 ms to 60 s");
            }

            mask |= RadioEvents.Heartbeat;
        }

        int stream = 0;
        if (subscription.RssiStreamPeriod is { } sp)
        {
            stream = (int)sp.TotalMilliseconds;
            if (stream is < 5 or > 250)
            {
                throw new ArgumentOutOfRangeException(nameof(subscription), sp, "RssiStreamPeriod must be 5 to 250 ms");
            }

            mask |= RadioEvents.RssiStream;
        }

        if (subscription.RssiStreamBatch is < 1 or > 20)
        {
            throw new ArgumentOutOfRangeException(nameof(subscription), subscription.RssiStreamBatch, "RssiStreamBatch must be 1 to 20");
        }

        int burst = 0;
        if (subscription.BurstSamplePeriod is { } bp)
        {
            burst = (int)bp.TotalMilliseconds;
            if (burst is < 2 or > 50)
            {
                throw new ArgumentOutOfRangeException(nameof(subscription), bp, "BurstSamplePeriod must be 2 to 50 ms");
            }
        }

        if (subscription.LiveTx && Firmware.Kind == FirmwareKind.PacketV2 && (Firmware.Capabilities & RadioCapabilities.LiveTx) == 0)
        {
            throw new K5FirmwareNotSupportedException("LiveTx", Firmware.Kind, "firmware with the LIVE_TX capability");
        }

        var w = new WireWriter()
            .U32((uint)mask)
            .U8((subscription.LiveTx ? 1 : 0) | (subscription.Persist ? 2 : 0))
            .U16(hb)
            .U8(stream)
            .U8(stream > 0 ? subscription.RssiStreamBatch : 0)
            .U8(burst);
        byte[] b = await V2Async("SUBSCRIBE", MessageIds.Subscribe, w.ToArray(), true, cancellationToken).ConfigureAwait(false);
        var r = new WireReader(b, "SUBSCRIBE reply");
        var info = new SubscriptionInfo(r.U16(), r.U16(), r.U32());
        _sequencer.Prime(info.NextSequence);
        lock (_gate)
        {
            _lastSubscription = subscription;
            _busySubscribed = (mask & RadioEvents.Busy) != 0;
            _channelBusy = null;
            _lastHeartbeat = hb > 0 ? _time.GetTimestamp() : 0;
        }

        if ((mask & RadioEvents.Busy) != 0)
        {
            await GetStatusAsync(cancellationToken).ConfigureAwait(false);
        }

        return info;
    }

    /// <summary>
    /// Aligns <see cref="Clock"/> with a few TIME_SYNC exchanges (v2) and returns the best. The
    /// radio clock drifts; heartbeats keep the estimate (and its rate) current afterwards.
    /// </summary>
    public async Task<TimeSyncResult> SyncClockAsync(int exchanges = 8, CancellationToken cancellationToken = default)
    {
        RequireV2("TIME_SYNC");
        ArgumentOutOfRangeException.ThrowIfLessThan(exchanges, 1);
        double bestHalf = double.MaxValue;
        for (int i = 0; i < exchanges; i++)
        {
            byte[] hostRef = new byte[8];
            BinaryPrimitives.WriteInt64LittleEndian(hostRef, _time.GetTimestamp());
            long t0 = _time.GetTimestamp();
            byte[] b = await V2Async("TIME_SYNC", MessageIds.TimeSync, hostRef, true, cancellationToken).ConfigureAwait(false);
            long t3 = _time.GetTimestamp();
            var r = new WireReader(b, "TIME_SYNC reply");
            r.Bytes(8);
            double rx = r.U32() + r.U16() / 1000.0;
            double tx = r.U32() + r.U16() / 1000.0;

            // Request frame: 8 framing + 4 header + 1 tag + 8 ref = 21 bytes; reply: 8 + 4 + 4 + 21 = 37.
            const int nq = 21;
            const int nr = 37;
            double up = Clock.HostMs(t0) + nq * 0.2604;
            double down = Clock.HostMs(t3) - nr * 0.256;
            double offset = ((up - rx) + (down - tx)) / 2.0;
            double half = Math.Max(0, ((down - up) - (tx - rx)) / 2.0);
            Clock.AddSymmetric((rx + tx) / 2.0, offset, half);
            if (half < bestHalf)
            {
                bestHalf = half;
            }
        }

        return new TimeSyncResult(Clock.ToHostTime(0) ?? default, TimeSpan.FromMilliseconds(bestHalf), exchanges);
    }

    /// <summary>
    /// Reads settings. On v2, GET_PARAMS: the live values, or with <paramref name="stored"/> the
    /// EEPROM values (RAM-only settings omitted); <paramref name="ids"/> empty means every
    /// supported one. On packet firmware v1, the EEPROM settings and timing blocks (the values the
    /// firmware is using, with out-of-range bytes reported as the defaults it falls back to).
    /// </summary>
    public async Task<RadioSettings> GetSettingsAsync(bool stored = false, IReadOnlyCollection<RadioParameterId>? ids = null, CancellationToken cancellationToken = default)
    {
        if (Firmware.Kind == FirmwareKind.PacketV1)
        {
            return await ReadV1SettingsAsync(cancellationToken).ConfigureAwait(false);
        }

        RequireV2("GET_PARAMS");
        var w = new WireWriter().U8(stored ? 1 : 0);
        foreach (var id in ids ?? [])
        {
            w.U8((byte)id);
        }

        byte[] b = await V2Async("GET_PARAMS", MessageIds.GetParams, w.ToArray(), true, cancellationToken).ConfigureAwait(false);
        var r = new WireReader(b, "GET_PARAMS reply");
        r.U8();
        var s = ParameterCodec.ReadRecords(ref r, _options.DeviationLaw, out _);
        if (!stored && s.PttReleaseDebounce is { } rel)
        {
            _pttRelease = rel;
        }

        return s;
    }

    /// <summary>
    /// Changes settings: every non-null property of <paramref name="changes"/>, validated here
    /// first. On v2, SET_PARAMS: atomic (all or nothing), live at once, read back. On packet
    /// firmware v1, a read-modify-write of the EEPROM settings blocks, which needs
    /// <see cref="AuthorizeEepromWritesAsync"/> first; the firmware applies it about 1.0 to 1.5 s
    /// after the session goes quiet (see <see cref="SetSettingsResult.AppliesAfterQuiet"/>).
    /// </summary>
    public async Task<SetSettingsResult> SetSettingsAsync(RadioSettings changes, SetSettingsFlags flags = SetSettingsFlags.None, CancellationToken cancellationToken = default)
    {
        ArgumentNullException.ThrowIfNull(changes);
        var records = ParameterCodec.ToWire(changes);
        if (records.Count == 0)
        {
            throw new ArgumentException("no settings to change", nameof(changes));
        }

        if (Firmware.Kind == FirmwareKind.PacketV1)
        {
            if ((flags & (SetSettingsFlags.DryRun | SetSettingsFlags.RequireTxAllowed)) != 0)
            {
                throw new K5FirmwareNotSupportedException("SetSettings with DryRun or RequireTxAllowed", Firmware.Kind, "protocol v2 firmware");
            }

            return await WriteV1SettingsAsync(changes, cancellationToken).ConfigureAwait(false);
        }

        RequireV2("SET_PARAMS");
        foreach (var (id, _) in records)
        {
            if (!Firmware.Supports(id))
            {
                throw new K5FirmwareNotSupportedException($"setting {id}", Firmware.Kind, "a firmware that reports this parameter in GET_INFO");
            }

            if ((flags & SetSettingsFlags.Persist) != 0 && ParameterCodec.IsRamOnly(id))
            {
                throw new ArgumentException($"{id} is RAM-only and cannot be persisted", nameof(changes));
            }
        }

        var w = new WireWriter().U8((int)flags);
        foreach (var (id, v) in records)
        {
            ParameterCodec.WriteRecord(w, id, v);
        }

        byte[] args = w.ToArray();
        if (args.Length + 1 > Math.Max(Firmware.MaxRequestBody, 120))
        {
            throw new ArgumentException("too many settings for one request", nameof(changes));
        }

        string outcome = "timeout";
        try
        {
            byte[] b = await V2Async("SET_PARAMS", MessageIds.SetParams, args, true, cancellationToken).ConfigureAwait(false);
            var r = new WireReader(b, "SET_PARAMS reply");
            byte result = r.U8();
            var applied = ParameterCodec.ReadRecords(ref r, _options.DeviationLaw, out _);
            if (applied.PttReleaseDebounce is { } rel && (flags & SetSettingsFlags.DryRun) == 0)
            {
                _pttRelease = rel;
            }

            if (applied.SerialLock is { } sl && (flags & SetSettingsFlags.DryRun) == 0)
            {
                _lock.SetFirmware(FirmwareKind.PacketV2, sl);
                Firmware = Firmware with { SerialLock = sl };
            }

            outcome = "ok";
            return new SetSettingsResult
            {
                Applied = applied,
                TxAllowed = (result & 1) != 0,
                PersistQueued = (result & 2) != 0,
                Retuned = (result & 4) != 0,
            };
        }
        catch (Exception e) when (e is not OperationCanceledException)
        {
            outcome = e.Message;
            throw;
        }
        finally
        {
            Audit(K5AuditKind.SettingsChange, $"SET_PARAMS {string.Join(", ", records.Select(x => $"{x.Id}={x.Value}"))} flags {flags}", outcome, NoDetails);
        }
    }

    /// <summary>SAVE_PARAMS SAVE (v2): persists every live persistable setting that differs from EEPROM. Returns what was written.</summary>
    public async Task<IReadOnlyList<RadioParameterId>> SaveSettingsAsync(CancellationToken cancellationToken = default)
    {
        byte[] b = await V2Async("SAVE_PARAMS", MessageIds.SaveParams, [0], true, cancellationToken).ConfigureAwait(false);
        return EventParser.MaskToIds(new WireReader(b, "SAVE_PARAMS reply").U32());
    }

    /// <summary>SAVE_PARAMS REVERT (v2): reloads every live setting from EEPROM and drops RAM-only ones. Returns what was reverted.</summary>
    public async Task<IReadOnlyList<RadioParameterId>> RevertSettingsAsync(CancellationToken cancellationToken = default)
    {
        byte[] b = await V2Async("SAVE_PARAMS", MessageIds.SaveParams, [1], true, cancellationToken).ConfigureAwait(false);
        return EventParser.MaskToIds(new WireReader(b, "SAVE_PARAMS reply").U32());
    }

    /// <summary>
    /// LEVEL_TONE (v2), deviation-equivalent mode: plays a tone on the radio's own audio output at
    /// the level a signal of <paramref name="deviationHz"/> would give, so the operator can set the
    /// volume knob against the TNC's level meter. Needs the tone calibration
    /// (<see cref="RadioCapabilities.LevelToneCalibrated"/>).
    /// </summary>
    public Task<LevelToneResult> StartLevelToneAsync(int frequencyHz, int deviationHz, TimeSpan duration, CancellationToken cancellationToken = default)
    {
        ArgumentOutOfRangeException.ThrowIfLessThan(deviationHz, 0);
        ArgumentOutOfRangeException.ThrowIfGreaterThan(deviationHz, 8000);
        return LevelToneAsync(frequencyHz, 0, deviationHz, duration, cancellationToken);
    }

    /// <summary>LEVEL_TONE (v2) with a raw REG_70&lt;14:8&gt; gain code, 0 to 127.</summary>
    public Task<LevelToneResult> StartLevelToneRawAsync(int frequencyHz, int gainCode, TimeSpan duration, CancellationToken cancellationToken = default)
    {
        ArgumentOutOfRangeException.ThrowIfLessThan(gainCode, 0);
        ArgumentOutOfRangeException.ThrowIfGreaterThan(gainCode, 127);
        return LevelToneAsync(frequencyHz, 1, gainCode, duration, cancellationToken);
    }

    /// <summary>Stops a running level tone.</summary>
    public async Task StopLevelToneAsync(CancellationToken cancellationToken = default)
    {
        var w = new WireWriter().U16(1000).U8(1).U16(0).U16(0);
        await V2Async("LEVEL_TONE", MessageIds.LevelTone, w.ToArray(), true, cancellationToken).ConfigureAwait(false);
    }

    private async Task<LevelToneResult> LevelToneAsync(int frequencyHz, int mode, int level, TimeSpan duration, CancellationToken ct)
    {
        ArgumentOutOfRangeException.ThrowIfLessThan(frequencyHz, 100);
        ArgumentOutOfRangeException.ThrowIfGreaterThan(frequencyHz, 5000);
        int ms = (int)duration.TotalMilliseconds;
        if (ms is < 1 or > 60000)
        {
            throw new ArgumentOutOfRangeException(nameof(duration), duration, "duration must be 1 ms to 60 s");
        }

        var w = new WireWriter().U16(frequencyHz).U8(mode).U16(level).U16(ms);
        byte[] b = await V2Async("LEVEL_TONE", MessageIds.LevelTone, w.ToArray(), true, ct).ConfigureAwait(false);
        var r = new WireReader(b, "LEVEL_TONE reply");
        return new LevelToneResult(r.U8(), r.U16());
    }

    /// <summary>OVERRIDE LIST (v2): the RAM and EEPROM register override tables.</summary>
    public Task<OverrideTables> GetOverridesAsync(CancellationToken cancellationToken = default) =>
        OverrideAsync(0, [], null, null, true, cancellationToken);

    /// <summary>
    /// OVERRIDE ADD (v2): adds trial overrides to the RAM table (at most 8 entries in all), bounded
    /// by time and/or key-ups, after which the radio reverts them by itself. RX-phase entries apply
    /// at once, TX-phase entries at the next key-up. Every register is checked against the refusal
    /// list first. Not retried on timeout (re-read with <see cref="GetOverridesAsync"/>).
    /// </summary>
    public Task<OverrideTables> AddOverridesAsync(IReadOnlyList<RegisterOverride> entries, TimeSpan? expiry, int? keyUps, CancellationToken cancellationToken = default)
    {
        ArgumentNullException.ThrowIfNull(entries);
        if (entries.Count is 0 or > 8)
        {
            throw new ArgumentOutOfRangeException(nameof(entries), entries.Count, "1 to 8 entries");
        }

        foreach (var e in entries)
        {
            K5Safety.CheckRegisterWrite(e.Register);
            if (e.Phase is OverridePhase.None || (byte)e.Phase > 3)
            {
                throw new ArgumentException($"override for 0x{e.Register:X2} has no valid phase", nameof(entries));
            }
        }

        if (expiry is { } x && (x.TotalSeconds is < 1 or > 65535 || x.TotalSeconds % 1 != 0))
        {
            throw new ArgumentOutOfRangeException(nameof(expiry), expiry, "whole seconds, 1 to 65535");
        }

        if (keyUps is < 1 or > 255)
        {
            throw new ArgumentOutOfRangeException(nameof(keyUps), keyUps, "1 to 255");
        }

        return OverrideAsync(1, entries, expiry, keyUps, false, cancellationToken);
    }

    /// <summary>OVERRIDE CLEAR (v2): empties the RAM table.</summary>
    public Task<OverrideTables> ClearOverridesAsync(CancellationToken cancellationToken = default) =>
        OverrideAsync(2, [], null, null, true, cancellationToken);

    /// <summary>OVERRIDE COMMIT (v2): the RAM table replaces the EEPROM table (0x1D10 to 0x1D4F) and the RAM table is cleared.</summary>
    public Task<OverrideTables> CommitOverridesAsync(CancellationToken cancellationToken = default) =>
        OverrideAsync(3, [], null, null, true, cancellationToken);

    /// <summary>OVERRIDE CLEAR_EEPROM (v2): empties the EEPROM table.</summary>
    public Task<OverrideTables> ClearEepromOverridesAsync(CancellationToken cancellationToken = default) =>
        OverrideAsync(4, [], null, null, true, cancellationToken);

    private async Task<OverrideTables> OverrideAsync(int op, IReadOnlyList<RegisterOverride> entries, TimeSpan? expiry, int? keyUps, bool retryable, CancellationToken ct)
    {
        var w = new WireWriter().U8(op).U16(expiry is { } e ? (int)e.TotalSeconds : 0).U8(keyUps ?? 0).U8(entries.Count);
        foreach (var x in entries)
        {
            w.U8((byte)x.Phase).U8(x.Register).U16(x.AndMask).U16(x.OrValue);
        }

        string outcome = "timeout";
        try
        {
            byte[] b = await V2Async("REG_OVERRIDE", MessageIds.RegOverride, w.ToArray(), retryable, ct).ConfigureAwait(false);
            var r = new WireReader(b, "REG_OVERRIDE reply");
            int nRam = r.U8();
            ushort sLeft = r.U16();
            byte kLeft = r.U8();
            var ram = ReadEntries(ref r, nRam);
            int nEe = r.U8();
            var ee = ReadEntries(ref r, nEe);
            outcome = "ok";
            return new OverrideTables
            {
                Ram = ram,
                Eeprom = ee,
                ExpiresIn = sLeft == 0xFFFF ? null : TimeSpan.FromSeconds(sLeft),
                KeyUpsLeft = kLeft == 0xFF ? null : kLeft,
            };
        }
        catch (Exception ex) when (ex is not OperationCanceledException)
        {
            outcome = ex.Message;
            throw;
        }
        finally
        {
            if (op != 0)
            {
                Audit(K5AuditKind.RegisterOverride, $"REG_OVERRIDE op {op} with {entries.Count} entries", outcome,
                    new Dictionary<string, string> { ["entries"] = string.Join(" ", entries.Select(x => $"{x.Phase}:{x.Register:X2}&{x.AndMask:X4}|{x.OrValue:X4}")) });
            }
        }

        static RegisterOverride[] ReadEntries(ref WireReader r, int n)
        {
            var list = new RegisterOverride[n];
            for (int i = 0; i < n; i++)
            {
                list[i] = new RegisterOverride((OverridePhase)r.U8(), r.U8(), r.U16(), r.U16());
            }

            return list;
        }
    }

    /// <summary>
    /// EVENT_REPLAY (v2): the radio re-sends every stored event still in its ring from
    /// <paramref name="fromSequence"/>, before replying. Events already delivered are not
    /// delivered again. The library does this by itself after a gap unless
    /// <see cref="K5RadioOptions.RecoverMissedEvents"/> is off.
    /// </summary>
    public async Task<ReplayResult> ReplayEventsAsync(ushort fromSequence, CancellationToken cancellationToken = default)
    {
        // One request re-sends at most 256 bytes of events (so the reply stays inside the
        // timeout); ask again from where it stopped until caught up.
        ReplayResult? total = null;
        ushort from = fromSequence;
        for (int round = 0; round < 64; round++)
        {
            byte[] b = await V2Async("EVENT_REPLAY", MessageIds.EventReplay, new WireWriter().U16(from).ToArray(), true, cancellationToken,
                TimeSpan.FromMilliseconds(Math.Max(_options.V2ReplyTimeout.TotalMilliseconds, 400))).ConfigureAwait(false);
            var r = new WireReader(b, "EVENT_REPLAY reply");
            var part = new ReplayResult(r.U16(), r.U8(), r.U16(), r.U16());
            total = total is { } t
                ? t with { CountSent = t.CountSent + part.CountSent, OldestAvailable = part.OldestAvailable, NextSequence = part.NextSequence }
                : part;
            ushort after = (ushort)(part.FirstSent + part.CountSent);
            if (part.CountSent == 0 || after == part.NextSequence || (ushort)(after - from) == 0)
            {
                break;
            }

            from = after;
        }

        _sequencer.ReplayDone();
        return total!.Value;
    }

    /// <summary>GET_COUNTERS (v2), optionally clearing them after reading.</summary>
    public async Task<RadioCounters> GetCountersAsync(bool clear = false, CancellationToken cancellationToken = default)
    {
        byte[] b = await V2Async("GET_COUNTERS", MessageIds.GetCounters, [clear ? (byte)1 : (byte)0], !clear, cancellationToken).ConfigureAwait(false);
        var r = new WireReader(b, "GET_COUNTERS reply");
        int n = r.U8();
        var list = new uint[n];
        for (int i = 0; i < n; i++)
        {
            list[i] = r.U32();
        }

        return new RadioCounters { Raw = list };
    }

    private static readonly IReadOnlyDictionary<string, string> NoDetails = new Dictionary<string, string>();

    private void Audit(K5AuditKind kind, string description, string outcome, IReadOnlyDictionary<string, string> details)
    {
        try
        {
            _options.Audit?.Invoke(new K5AuditEntry(kind, description, outcome, details));
        }
        catch (Exception e)
        {
            _options.Trace?.Invoke($"audit callback threw: {e.Message}");
        }
    }
}
