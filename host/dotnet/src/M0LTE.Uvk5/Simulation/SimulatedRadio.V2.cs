using System.Buffers.Binary;
using M0LTE.Uvk5.Protocol;

namespace M0LTE.Uvk5.Simulation;

public sealed partial class SimulatedRadio
{
    private readonly Dictionary<RadioParameterId, ulong> _live = [];
    private readonly Dictionary<RadioParameterId, ulong> _storedChannel = [];
    private readonly List<StoredEvent> _ring = [];
    private readonly uint[] _counters = new uint[14];
    private readonly List<RegisterOverride> _ramOverrides = [];
    private readonly List<(ushort Rssi, byte Noise, byte Glitch)> _streamBatch = [];

    private (Rssi Rssi, int Noise, int Glitch)? _carrier;
    private bool _busy;
    private bool _rssiBusy;
    private uint _busyEdgeMs;
    private uint _burstOpenMs;
    private (ushort Rssi, int Noise, int Glitch) _burstSignal;
    private uint _busyAccumMs;
    private uint _lastHeartbeatMs;
    private ushort _nextSeq;
    private uint _subMask;
    private bool _liveTx;
    private int _hbPeriod;
    private int _streamPeriod;
    private int _streamBatchSize = 10;
    private int _burstPeriod = 5;
    private int _lostPendingFirst = -1;
    private int _lostPendingCount;
    private bool _toneRunning;
    private int _overrideKeyUps;
    private ITimer? _heartbeatTimer;
    private ITimer? _streamTimer;
    private ITimer? _toneTimer;
    private ITimer? _overrideTimer;
    private long _overrideExpiresAt;

    private sealed class StoredEvent
    {
        public required ushort Seq { get; init; }

        public required byte[] Payload { get; set; }

        public bool Sent { get; set; }

        public bool Deferred { get; set; }
    }

    /// <summary>The live settings.</summary>
    public RadioSettings LiveSettings
    {
        get
        {
            lock (_gate)
            {
                var s = new RadioSettings();
                foreach (var (id, v) in _live)
                {
                    s = ParameterCodec.Apply(s, id, v, DeviationLaw.AiocWithBenchEq);
                }

                return s;
            }
        }
    }

    /// <summary>The live event subscription mask.</summary>
    public RadioEvents SubscribedEvents
    {
        get
        {
            lock (_gate)
            {
                return (RadioEvents)_subMask;
            }
        }
    }

    /// <summary>The RAM register override entries.</summary>
    public IReadOnlyList<RegisterOverride> RamOverrides
    {
        get
        {
            lock (_gate)
            {
                return [.. _ramOverrides];
            }
        }
    }

    /// <summary>True while a level tone plays.</summary>
    public bool IsToneRunning
    {
        get
        {
            lock (_gate)
            {
                return _toneRunning;
            }
        }
    }

    private ulong Param(RadioParameterId id) => _live.TryGetValue(id, out var v) ? v : 0;

    private void Counter(int i)
    {
        if (i < _counters.Length)
        {
            _counters[i]++;
        }
    }

    // ------------------------------------------------------------------ power-on state

    private void PowerOnV2()
    {
        _live.Clear();
        foreach (RadioParameterId id in Enum.GetValues<RadioParameterId>())
        {
            _live[id] = StoredOrDefault(id);
        }

        Array.Clear(_counters);
        _ring.Clear();
        _nextSeq = 0;
        _busy = false;
        _rssiBusy = false;
        _busyEdgeMs = 0;
        _busyAccumMs = 0;
        _lastHeartbeatMs = 0;
        _toneRunning = false;
        _ramOverrides.Clear();
        _streamBatch.Clear();
        _lostPendingFirst = -1;
        _lostPendingCount = 0;
        ResetSubscriptionToStored();
        UpdateBusy(BusyCause.None);
    }

    private bool SettingsBlockValid() => _eeprom[0x1D00] == 1;

    private bool V2BlockValid() => SettingsBlockValid() && _eeprom[0x1D60] == 1;

    private ulong StoredOrDefault(RadioParameterId id)
    {
        bool v1 = SettingsBlockValid();
        bool v2 = V2BlockValid();
        int U8(int addr, int min, int max, int def, bool valid) => valid && _eeprom[addr] >= min && _eeprom[addr] <= max ? _eeprom[addr] : def;
        int U16(int addr, int max, int def, bool valid)
        {
            int v = BinaryPrimitives.ReadUInt16LittleEndian(_eeprom.AsSpan(addr));
            return valid && v <= max ? v : def;
        }

        return id switch
        {
            RadioParameterId.FrequencyHz => _storedChannel.TryGetValue(id, out var f) ? f : (ulong)_options.FrequencyHz,
            RadioParameterId.Power => _storedChannel.TryGetValue(id, out var p) ? p : 0,
            RadioParameterId.Bandwidth => _storedChannel.TryGetValue(id, out var b) ? b : 0,
            RadioParameterId.DeviationWide => (ulong)U16(0x1D04, Deviation.MaxRegister, 0x856, v1),
            RadioParameterId.DeviationNarrow => (ulong)U16(0x1D06, Deviation.MaxRegister, 0x756, v1),
            RadioParameterId.MicGain => (ulong)U8(0x1D03, 0, 31, 31, v1),
            RadioParameterId.Squelch => (ulong)U8(0x1D01, 0, 9, 1, v1),
            RadioParameterId.RxGain => (ulong)U8(0x1D08, 0, 63, 50, v1),
            RadioParameterId.RxDacGain => (ulong)U8(0x1D09, 0, 15, 15, v1),
            RadioParameterId.TxTimeoutSeconds => (ulong)ParameterCodec.TxTimeoutSeconds[U8(0x1D02, 0, 6, 4, v1)],
            RadioParameterId.PttPressMs => (ulong)U8(0x1D50, 1, 40, 5, v1),
            RadioParameterId.PttReleaseMs => (ulong)U8(0x1D51, 2, 40, 5, v1),
            RadioParameterId.PaEnableDelayMs => (ulong)U8(0x1D52, 1, 20, 1, v1),
            RadioParameterId.PaBiasDelayMs => (ulong)U8(0x1D53, 0, 20, 2, v1),
            RadioParameterId.SerialLockMs => (ulong)(U8(0x1D61, 0, 150, 2, v2) * 10),
            RadioParameterId.BusySource => (ulong)U8(0x1D62, 1, 3, 1, v2),
            RadioParameterId.BusyHangMs => (ulong)U8(0x1D63, 0, 250, 20, v2),
            RadioParameterId.BusyRssiOpen => (ulong)U16(0x1D64, 511, 110, v2),
            RadioParameterId.BusyRssiClose => (ulong)U16(0x1D66, 511, 104, v2),
            RadioParameterId.SquelchRaw => 0x28_30_40_48_50_5AUL,
            RadioParameterId.AgcFix => 0xFF,
            RadioParameterId.Afc => 1,
            RadioParameterId.Backlight => (ulong)U8(0x1D0A, 0, 7, 3, v1),
            RadioParameterId.KeyLock => (ulong)U8(0x1D0C, 0, 1, 0, v1),
            _ => 0,
        };
    }

    private void ResetSubscriptionToStored()
    {
        bool v2 = V2BlockValid();
        uint mask = v2 ? BinaryPrimitives.ReadUInt32LittleEndian(_eeprom.AsSpan(0x1D68)) : 0;
        if (mask == 0xFFFFFFFF)
        {
            mask = 0;
        }

        int hb = v2 ? BinaryPrimitives.ReadUInt16LittleEndian(_eeprom.AsSpan(0x1D6C)) : 0;
        if (hb is < 100 or > 60000)
        {
            hb = 0;
        }

        _subMask = mask;
        _liveTx = v2 && _eeprom[0x1D6E] != 0xFF && (_eeprom[0x1D6E] & 1) != 0;
        _hbPeriod = hb;
        _streamPeriod = 0;
        _burstPeriod = 5;
        RestartTimers();
    }

    private void ReloadFromEeprom()
    {
        _reloadTimer = null;
        if (_transmitting)
        {
            _reloadTimer = After(100, ReloadFromEeprom);
            return;
        }

        uint mask = 0;
        foreach (RadioParameterId id in Enum.GetValues<RadioParameterId>())
        {
            if (ParameterCodec.IsRamOnly(id) || id is RadioParameterId.FrequencyHz or RadioParameterId.Power or RadioParameterId.Bandwidth)
            {
                continue;
            }

            ulong v = StoredOrDefault(id);
            if (Param(id) != v)
            {
                _live[id] = v;
                mask |= 1u << (int)id;
            }
        }

        if (mask != 0 && _options.Firmware == FirmwareKind.PacketV2)
        {
            StoreEvent(8, new WireWriter().U8((int)ParamsChangeSource.EepromReload).U32(mask).ToArray());
        }
    }

    // ------------------------------------------------------------------ busy and bursts

    private void UpdateBusy(BusyCause cause)
    {
        if (_transmitting)
        {
            return;
        }

        int src = (int)Param(RadioParameterId.BusySource);
        bool squelchOpen = Param(RadioParameterId.Squelch) == 0 || _carrier is not null;
        if (_carrier is { } c)
        {
            if (c.Rssi.Raw >= Param(RadioParameterId.BusyRssiOpen))
            {
                _rssiBusy = true;
            }
            else if (c.Rssi.Raw < Param(RadioParameterId.BusyRssiClose))
            {
                _rssiBusy = false;
            }
        }
        else
        {
            _rssiBusy = false;
        }

        bool busy = ((src & 1) != 0 && squelchOpen) || ((src & 2) != 0 && _rssiBusy);
        if (busy != _busy)
        {
            SetBusy(busy, cause);
        }
    }

    private void SetBusy(bool busy, BusyCause cause)
    {
        uint now = NowMs();
        var (rssi, noise, glitch) = _carrier is { } c ? (c.Rssi.Raw, c.Noise, c.Glitch) : Signal();
        if (!busy && _busy)
        {
            _busyAccumMs += now - _burstOpenMs;
        }

        _busy = busy;
        _busyEdgeMs = now;
        int sources = (_carrier is not null || Param(RadioParameterId.Squelch) == 0 ? 1 : 0) | (_rssiBusy ? 2 : 0);
        if (!busy)
        {
            sources = 0;
        }

        StoreEvent(0, new WireWriter().U8(busy ? 1 : 0).U8(sources).U8((int)cause).U16(rssi).U8(noise).U8(glitch).ToArray());
        if (busy)
        {
            Counter(10);
            _burstOpenMs = now;
            _burstSignal = (rssi, noise, glitch);
            return;
        }

        uint dur = now - _burstOpenMs;
        int samples = Math.Max(1, (int)(dur / (uint)Math.Max(1, _burstPeriod)));
        var (br, bn, bg) = _burstSignal;
        StoreEvent(1, new WireWriter().U32(_burstOpenMs).U32(dur).U16(samples).U16(br).U16(br).U16(br).U8(bn).U8(bn).U8(bg).U8(bg)
            .U16(0xFFFF).U16(0xFFFF).U8(0).U8(0).S16(0x7FFF).U8((int)cause).ToArray());
    }

    // ------------------------------------------------------------------ events

    private bool Deferring() => (_pttPressed || _transmitting) && !_liveTx;

    private void StoreEvent(int n, byte[] body, uint? atMs = null)
    {
        if (_options.Firmware != FirmwareKind.PacketV2 || (_subMask & (1u << n)) == 0)
        {
            return;
        }

        ushort seq = _nextSeq++;
        byte[] payload = EventPayload((ushort)(MessageIds.EventFirst + n), seq, atMs ?? NowMs(), 0, body);
        if (_ring.Count >= _options.EventRingCapacity)
        {
            var old = _ring[0];
            _ring.RemoveAt(0);
            if (!old.Sent)
            {
                Counter(5);
                if (_lostPendingFirst < 0)
                {
                    _lostPendingFirst = old.Seq;
                }

                _lostPendingCount++;
            }
        }

        _ring.Add(new StoredEvent { Seq = seq, Payload = payload });
        Counter(4);
        FlushEvents();
    }

    private static byte[] EventPayload(ushort id, ushort seq, uint t, byte flags, byte[] body)
    {
        var w = new WireWriter().U16(seq).U32(t).U8(flags).Bytes(body);
        return K5FrameCodec.BuildPayload(id, w.ToArray());
    }

    private void FlushEvents()
    {
        if (_disposed || _options.Firmware != FirmwareKind.PacketV2)
        {
            return;
        }

        if (Deferring())
        {
            foreach (var e in _ring)
            {
                if (!e.Sent && !e.Deferred)
                {
                    e.Deferred = true;
                    Counter(6);
                }
            }

            return;
        }

        foreach (var e in _ring)
        {
            if (e.Sent)
            {
                continue;
            }

            byte flags = e.Deferred ? (byte)RadioEventFlags.Deferred : (byte)RadioEventFlags.TimeExact;
            e.Payload[10] = flags;
            e.Sent = true;
            SendV2Frame(BinaryPrimitives.ReadUInt16LittleEndian(e.Payload), e.Payload.AsSpan(4).ToArray());
        }

        // Everything in the ring has gone, so storing EVENTS_LOST overwrites nothing unsent.
        if (_lostPendingCount > 0 && (_subMask & 0x1FFFu & ~((1u << 5) | (1u << 6))) != 0)
        {
            int first = _lostPendingFirst;
            int count = _lostPendingCount;
            _lostPendingFirst = -1;
            _lostPendingCount = 0;
            uint saved = _subMask;
            _subMask |= 1u << 9;
            StoreEvent(9, new WireWriter().U16(first).U16(count).ToArray());
            _subMask = saved;
        }
    }

    private void EmitEphemeral(int n, byte[] body)
    {
        if ((_subMask & (1u << n)) == 0)
        {
            return;
        }

        if (Deferring())
        {
            Counter(12);
            return;
        }

        byte[] payload = EventPayload((ushort)(MessageIds.EventFirst + n), _nextSeq, NowMs(), (byte)(RadioEventFlags.Ephemeral | RadioEventFlags.TimeExact), body);
        SendV2Frame(BinaryPrimitives.ReadUInt16LittleEndian(payload), payload.AsSpan(4).ToArray());
    }

    private void RestartTimers()
    {
        Cancel(ref _heartbeatTimer);
        Cancel(ref _streamTimer);
        if (_hbPeriod > 0 && (_subMask & (1u << 6)) != 0)
        {
            _lastHeartbeatMs = NowMs();
            _busyAccumMs = 0;
            ScheduleHeartbeat();
        }

        if (_streamPeriod > 0 && (_subMask & (1u << 5)) != 0)
        {
            ScheduleStream();
        }
    }

    private void ScheduleHeartbeat()
    {
        _heartbeatTimer = After(_hbPeriod, () =>
        {
            uint now = NowMs();
            uint busyMs = _busyAccumMs + (_busy ? now - _burstOpenMs : 0);
            _busyAccumMs = 0;
            if (_busy)
            {
                _burstOpenMs = now;     // occupancy counted per interval
            }

            _lastHeartbeatMs = now;
            EmitEphemeral(6, new WireWriter().U16(NowUs()).U8((int)Flags1()).U8((int)State()).U16(Signal().Rssi)
                .U16(BatteryMillivolts / 10 * 10).U16((int)Math.Min(busyMs, 65535)).U16(LockMs()).ToArray());
            ScheduleHeartbeat();
        });
    }

    private void ScheduleStream()
    {
        _streamTimer = After(_streamPeriod, () =>
        {
            if (!_transmitting)
            {
                var (r, n, g) = Signal();
                _streamBatch.Add((r, (byte)n, (byte)g));
                if (_streamBatch.Count >= _streamBatchSize)
                {
                    var w = new WireWriter().U8(_streamPeriod).U8(_streamBatch.Count);
                    foreach (var s in _streamBatch)
                    {
                        w.U16(s.Rssi).U8(s.Noise).U8(s.Glitch);
                    }

                    _streamBatch.Clear();
                    EmitEphemeral(5, w.ToArray());
                }
            }

            ScheduleStream();
        });
    }

    private StatusFlags Flags1()
    {
        var f = StatusFlags.None;
        if (_carrier is not null || Param(RadioParameterId.Squelch) == 0)
        {
            f |= StatusFlags.SquelchOpen;
        }

        if (_busy)
        {
            f |= StatusFlags.Busy;
        }

        if (_pttPressed)
        {
            f |= StatusFlags.PttPressed;
        }

        if (LockMs() > 0)
        {
            f |= StatusFlags.LockActive;
        }

        if (_options.TxAllowed((long)Param(RadioParameterId.FrequencyHz)))
        {
            f |= StatusFlags.TxAllowed;
        }

        if (_latched)
        {
            f |= StatusFlags.TxLatched;
        }

        if (_toneRunning)
        {
            f |= StatusFlags.LevelToneRunning;
        }

        if (_lateKeyPending)
        {
            f |= StatusFlags.LateKeyPending;
        }

        return f;
    }

    private RadioState State() =>
        _transmitting ? RadioState.Transmitting : _carrier is not null || _busy ? RadioState.Receiving : RadioState.Idle;

    // ------------------------------------------------------------------ v2 commands

    private void Reply(ushort id, byte tag, K5Status status, byte[] body)
    {
        if (status != K5Status.Ok)
        {
            Counter(3);
        }

        if (_dropReplies > 0)
        {
            _dropReplies--;
            return;
        }

        var w = new WireWriter().U8(tag).U8((int)status).U16(LockMs()).Bytes(body);
        SendV2Frame((ushort)(id + MessageIds.ReplyOffset), w.ToArray());
    }

    private void Error(ushort id, byte tag, K5Status status, int detail = 0) => Reply(id, tag, status, [(byte)detail]);

    private void HandleV2(ushort id, byte[] payload)
    {
        int declared = BinaryPrimitives.ReadUInt16LittleEndian(payload.AsSpan(2));
        byte[] body = payload[4..];
        byte tag = body.Length > 0 ? body[0] : (byte)0;
        if (declared != body.Length || body.Length < 1 || body.Length > 120)
        {
            Error(id, tag, K5Status.BadLength);
            return;
        }

        byte[] a = body[1..];
        switch (id)
        {
            case MessageIds.GetInfo: GetInfo(id, tag, a); break;
            case MessageIds.GetStatus: GetStatus(id, tag, a); break;
            case MessageIds.Subscribe: Subscribe(id, tag, a); break;
            case MessageIds.TimeSync: TimeSync(id, tag, a); break;
            case MessageIds.GetParams: GetParams(id, tag, a); break;
            case MessageIds.SetParams: SetParams(id, tag, a); break;
            case MessageIds.SaveParams: SaveParams(id, tag, a); break;
            case MessageIds.LevelTone: LevelTone(id, tag, a); break;
            case MessageIds.RegReadV2: RegRead(id, tag, a); break;
            case MessageIds.RegWriteV2: RegWrite(id, tag, a); break;
            case MessageIds.RegOverride: Override(id, tag, a); break;
            case MessageIds.EventReplay: Replay(id, tag, a); break;
            case MessageIds.GetCounters: Counters(id, tag, a); break;
            default: Error(id, tag, K5Status.UnknownCommand); break;
        }
    }

    private void GetInfo(ushort id, byte tag, byte[] a)
    {
        if (a.Length != 0)
        {
            Error(id, tag, K5Status.BadLength);
            return;
        }

        var caps = RadioCapabilities.LiveTx | RadioCapabilities.RssiBusyDetector | RadioCapabilities.LevelToneRaw
            | RadioCapabilities.RamRegisterOverrides | RadioCapabilities.Persistence | RadioCapabilities.ExactTimeSync;
        if (_eeprom[0x1D6F] != 0xFF && V2BlockValid())
        {
            caps |= RadioCapabilities.LevelToneCalibrated;
        }

        uint pmask = 0;
        foreach (RadioParameterId p in Enum.GetValues<RadioParameterId>())
        {
            pmask |= 1u << (int)p;
        }

        var w = new WireWriter().U16(0x0200).Ascii(Version, 16).U32((uint)caps).U32(pmask).U32(0x1FFF).U8(120).U8(_options.EventRingCapacity)
            .U16((int)Param(RadioParameterId.SerialLockMs)).U8(LateKeyMaxMs).U8(0).U8(0).U8(SettingsBlockValid() ? 1 : 0).U8(V2BlockValid() ? 1 : 0).U8(5);
        Reply(id, tag, K5Status.Ok, w.ToArray());
    }

    private void GetStatus(ushort id, byte tag, byte[] a)
    {
        if (a.Length != 0)
        {
            Error(id, tag, K5Status.BadLength);
            return;
        }

        var (rssi, noise, glitch) = Signal();
        int bw = (int)Param(RadioParameterId.Bandwidth);
        var flags2 = StatusFlags2.None;
        if (_ramOverrides.Count > 0)
        {
            flags2 |= StatusFlags2.RamOverridesActive;
        }

        if (_liveTx)
        {
            flags2 |= StatusFlags2.LiveTx;
        }

        uint busyAge = NowMs() - _busyEdgeMs;
        int txLeft = _transmitting ? Math.Max(0, (int)((TxTimeoutSeconds() * 1000 - (NowMs() - _txStartMs)) / 100)) : 0xFFFF;
        var w = new WireWriter().U32(NowMs()).U32((uint)Param(RadioParameterId.FrequencyHz)).U8((int)State()).U8((int)Flags1()).U8((int)flags2)
            .U8((int)Param(RadioParameterId.Power)).U8(bw).U8((int)Param(RadioParameterId.Squelch))
            .U16((int)Param(bw == 0 ? RadioParameterId.DeviationWide : RadioParameterId.DeviationNarrow))
            .U16(rssi).U8(noise).U8(glitch).U8(0).U8(BatteryLevel()).U16(BatteryMillivolts / 10 * 10).U16(LockMs()).U16(txLeft)
            .U16((int)Math.Min(busyAge, 65535)).U16(_nextSeq).U8(200).U8(TxTimeoutSeconds());
        Reply(id, tag, K5Status.Ok, w.ToArray());
    }

    private void Subscribe(ushort id, byte tag, byte[] a)
    {
        if (a.Length != 10)
        {
            Error(id, tag, K5Status.BadLength);
            return;
        }

        uint mask = BinaryPrimitives.ReadUInt32LittleEndian(a);
        byte options = a[4];
        int hb = BinaryPrimitives.ReadUInt16LittleEndian(a.AsSpan(5));
        int stream = a[7];
        int batch = a[8];
        int burst = a[9];
        if ((hb != 0 && hb is < 100 or > 60000))
        {
            Error(id, tag, K5Status.Range, 5);
            return;
        }

        if (stream != 0 && stream is < 5 or > 250)
        {
            Error(id, tag, K5Status.Range, 7);
            return;
        }

        if (batch is < 1 or > 20)
        {
            Error(id, tag, K5Status.Range, 8);
            return;
        }

        if (burst != 0 && burst is < 2 or > 50)
        {
            Error(id, tag, K5Status.Range, 9);
            return;
        }

        if ((options & 2) != 0)
        {
            if (!SettingsBlockValid())
            {
                Error(id, tag, K5Status.Eeprom);
                return;
            }

            EnsureV2Block();
            BinaryPrimitives.WriteUInt32LittleEndian(_eeprom.AsSpan(0x1D68), mask);
            BinaryPrimitives.WriteUInt16LittleEndian(_eeprom.AsSpan(0x1D6C), (ushort)hb);
            _eeprom[0x1D6E] = (byte)(options & 1);
            Counter(13);
            Counter(13);
        }

        _subMask = mask;
        _liveTx = (options & 1) != 0;
        _hbPeriod = hb;
        _streamPeriod = stream;
        _streamBatchSize = batch;
        _burstPeriod = burst == 0 ? 5 : burst;
        _streamBatch.Clear();
        RestartTimers();
        ushort oldest = _ring.Count > 0 ? _ring[0].Seq : _nextSeq;
        Reply(id, tag, K5Status.Ok, new WireWriter().U16(_nextSeq).U16(oldest).U32(NowMs()).ToArray());
    }

    private void TimeSync(ushort id, byte tag, byte[] a)
    {
        if (a.Length != 8)
        {
            Error(id, tag, K5Status.BadLength);
            return;
        }

        uint ms = NowMs();
        int us = NowUs();
        Reply(id, tag, K5Status.Ok, new WireWriter().Bytes(a).U32(ms).U16(us).U32(ms).U16(us).U8(1).ToArray());
    }

    private void GetParams(ushort id, byte tag, byte[] a)
    {
        if (a.Length < 1)
        {
            Error(id, tag, K5Status.BadLength);
            return;
        }

        bool stored = (a[0] & 1) != 0;
        var ids = a.Length > 1 ? a[1..].Select(x => (RadioParameterId)x).ToList() : [.. Enum.GetValues<RadioParameterId>()];
        var seen = new HashSet<RadioParameterId>();
        foreach (var p in ids)
        {
            if (!ParameterCodec.IsKnown(p) || !seen.Add(p))
            {
                Error(id, tag, K5Status.BadParameter, (byte)p);
                return;
            }
        }

        var w = new WireWriter().U8(a[0]);
        foreach (var p in ids)
        {
            if (stored && ParameterCodec.IsRamOnly(p))
            {
                continue;
            }

            ParameterCodec.WriteRecord(w, p, stored ? StoredOrDefault(p) : Param(p));
        }

        Reply(id, tag, K5Status.Ok, w.ToArray());
    }

    private void SetParams(ushort id, byte tag, byte[] a)
    {
        if (a.Length < 1)
        {
            Error(id, tag, K5Status.BadLength);
            return;
        }

        byte flags = a[0];
        bool persist = (flags & 1) != 0;
        bool requireTx = (flags & 2) != 0;
        bool dry = (flags & 4) != 0;
        var records = new List<(RadioParameterId Id, ulong Value)>();
        int pos = 1;
        while (pos < a.Length)
        {
            var p = (RadioParameterId)a[pos];
            int size = ParameterCodec.SizeOf(p);
            if (size < 0 || records.Any(r => r.Id == p))
            {
                Error(id, tag, K5Status.BadParameter, (byte)p);
                return;
            }

            if (pos + 1 + size > a.Length)
            {
                Error(id, tag, K5Status.BadLength);
                return;
            }

            ulong v = 0;
            for (int i = 0; i < size; i++)
            {
                v |= (ulong)a[pos + 1 + i] << (8 * i);
            }

            records.Add((p, v));
            pos += 1 + size;
        }

        ulong open = records.FirstOrDefault(r => r.Id == RadioParameterId.BusyRssiOpen) is { Id: RadioParameterId.BusyRssiOpen } o ? o.Value : Param(RadioParameterId.BusyRssiOpen);
        foreach (var (p, v) in records)
        {
            if (!InRange(p, v, open))
            {
                Error(id, tag, K5Status.Range, (byte)p);
                return;
            }

            if (persist && ParameterCodec.IsRamOnly(p))
            {
                Error(id, tag, K5Status.NotPersistable, (byte)p);
                return;
            }
        }

        ulong freq = records.FirstOrDefault(r => r.Id == RadioParameterId.FrequencyHz) is { Id: RadioParameterId.FrequencyHz } fr ? fr.Value : Param(RadioParameterId.FrequencyHz);
        bool txOk = _options.TxAllowed((long)freq);
        if (requireTx && !txOk)
        {
            Error(id, tag, K5Status.TxBand, (byte)RadioParameterId.FrequencyHz);
            return;
        }

        if (persist && !SettingsBlockValid())
        {
            Error(id, tag, K5Status.Eeprom);
            return;
        }

        bool retuned = false;
        if (!dry)
        {
            uint mask = 0;
            foreach (var (p, v) in records)
            {
                if (p == RadioParameterId.FrequencyHz && Param(p) != v)
                {
                    retuned = true;
                }

                _live[p] = v;
                mask |= 1u << (int)p;
                if (p == RadioParameterId.Squelch)
                {
                    _live[RadioParameterId.SquelchRaw] = StoredOrDefault(RadioParameterId.SquelchRaw);
                }
            }

            if (retuned)
            {
                StopTone(ToneEndReason.Retune);
                if (_busy)
                {
                    SetBusy(false, BusyCause.Retune);
                }
            }

            UpdateBusy(BusyCause.SquelchEdge | BusyCause.RssiEdge);
            if (persist)
            {
                foreach (var (p, v) in records)
                {
                    PersistParam(p, v);
                }
            }

            StoreEvent(8, new WireWriter().U8((int)ParamsChangeSource.Serial).U32(mask).ToArray());
        }

        var w = new WireWriter().U8((txOk ? 1 : 0) | (persist && !dry ? 2 : 0) | (retuned ? 4 : 0));
        foreach (var (p, v) in records)
        {
            ParameterCodec.WriteRecord(w, p, dry ? v : Param(p));
        }

        Reply(id, tag, K5Status.Ok, w.ToArray());
    }

    private static bool InRange(RadioParameterId p, ulong v, ulong rssiOpen) => p switch
    {
        RadioParameterId.FrequencyHz => v is >= 50_000_000 and <= 600_000_000 && v % 10 == 0,
        RadioParameterId.Power => v <= 2,
        RadioParameterId.Bandwidth => v <= 1,
        RadioParameterId.DeviationWide or RadioParameterId.DeviationNarrow => v <= Deviation.MaxRegister,
        RadioParameterId.MicGain => v <= 31,
        RadioParameterId.Squelch => v <= 9,
        RadioParameterId.RxGain => v <= 63,
        RadioParameterId.RxDacGain => v <= 15,
        RadioParameterId.TxTimeoutSeconds => Array.IndexOf(ParameterCodec.TxTimeoutSeconds, (int)v) >= 0,
        RadioParameterId.PttPressMs => v is >= 1 and <= 40,
        RadioParameterId.PttReleaseMs => v is >= 2 and <= 40,
        RadioParameterId.PaEnableDelayMs => v is >= 1 and <= 20,
        RadioParameterId.PaBiasDelayMs => v <= 20,
        RadioParameterId.SerialLockMs => v <= 1500 && v % 10 == 0,
        RadioParameterId.BusySource => v is >= 1 and <= 3,
        RadioParameterId.BusyRssiOpen => v <= 511,
        RadioParameterId.BusyRssiClose => v <= 511 && v <= rssiOpen,
        RadioParameterId.BusyHangMs => v <= 250,
        RadioParameterId.SquelchRaw => ((v >> 16) & 0xFF) <= 127 && ((v >> 24) & 0xFF) <= 127,
        RadioParameterId.AgcFix => v == 0xFF || v <= 7,
        RadioParameterId.Afc or RadioParameterId.KeyLock => v <= 1,
        RadioParameterId.Backlight => v <= 7,
        _ => false,
    };

    private void EnsureV2Block()
    {
        if (_eeprom[0x1D60] != 1)
        {
            Array.Fill(_eeprom, (byte)0xFF, 0x1D60, 16);
            _eeprom[0x1D60] = 1;
        }
    }

    private void PersistParam(RadioParameterId p, ulong v)
    {
        void W8(int addr, ulong x) => _eeprom[addr] = (byte)x;
        void W16(int addr, ulong x) => BinaryPrimitives.WriteUInt16LittleEndian(_eeprom.AsSpan(addr), (ushort)x);
        Counter(13);
        switch (p)
        {
            case RadioParameterId.FrequencyHz or RadioParameterId.Power or RadioParameterId.Bandwidth: _storedChannel[p] = v; break;
            case RadioParameterId.DeviationWide: W16(0x1D04, v); break;
            case RadioParameterId.DeviationNarrow: W16(0x1D06, v); break;
            case RadioParameterId.MicGain: W8(0x1D03, v); break;
            case RadioParameterId.Squelch: W8(0x1D01, v); break;
            case RadioParameterId.RxGain: W8(0x1D08, v); break;
            case RadioParameterId.RxDacGain: W8(0x1D09, v); break;
            case RadioParameterId.TxTimeoutSeconds: W8(0x1D02, (ulong)Array.IndexOf(ParameterCodec.TxTimeoutSeconds, (int)v)); break;
            case RadioParameterId.PttPressMs: W8(0x1D50, v); break;
            case RadioParameterId.PttReleaseMs: W8(0x1D51, v); break;
            case RadioParameterId.PaEnableDelayMs: W8(0x1D52, v); break;
            case RadioParameterId.PaBiasDelayMs: W8(0x1D53, v); break;
            case RadioParameterId.SerialLockMs: EnsureV2Block(); W8(0x1D61, v / 10); break;
            case RadioParameterId.BusySource: EnsureV2Block(); W8(0x1D62, v); break;
            case RadioParameterId.BusyHangMs: EnsureV2Block(); W8(0x1D63, v); break;
            case RadioParameterId.BusyRssiOpen: EnsureV2Block(); W16(0x1D64, v); break;
            case RadioParameterId.BusyRssiClose: EnsureV2Block(); W16(0x1D66, v); break;
            case RadioParameterId.Backlight: W8(0x1D0A, v); break;
            case RadioParameterId.KeyLock: W8(0x1D0C, v); break;
        }
    }

    private void SaveParams(ushort id, byte tag, byte[] a)
    {
        if (a.Length != 1 || a[0] > 1)
        {
            Error(id, tag, a.Length != 1 ? K5Status.BadLength : K5Status.Range, 0);
            return;
        }

        uint mask = 0;
        if (a[0] == 0)
        {
            if (!SettingsBlockValid())
            {
                Error(id, tag, K5Status.Eeprom);
                return;
            }

            foreach (RadioParameterId p in Enum.GetValues<RadioParameterId>())
            {
                if (!ParameterCodec.IsRamOnly(p) && Param(p) != StoredOrDefault(p))
                {
                    PersistParam(p, Param(p));
                    mask |= 1u << (int)p;
                }
            }
        }
        else
        {
            foreach (RadioParameterId p in Enum.GetValues<RadioParameterId>())
            {
                ulong v = StoredOrDefault(p);
                if (Param(p) != v)
                {
                    _live[p] = v;
                    mask |= 1u << (int)p;
                }
            }

            UpdateBusy(BusyCause.SquelchEdge | BusyCause.RssiEdge);
        }

        Reply(id, tag, K5Status.Ok, new WireWriter().U32(mask).ToArray());
    }

    private void LevelTone(ushort id, byte tag, byte[] a)
    {
        if (a.Length != 7)
        {
            Error(id, tag, K5Status.BadLength);
            return;
        }

        int f = BinaryPrimitives.ReadUInt16LittleEndian(a);
        int mode = a[2];
        int level = BinaryPrimitives.ReadUInt16LittleEndian(a.AsSpan(3));
        int dur = BinaryPrimitives.ReadUInt16LittleEndian(a.AsSpan(5));
        if (dur == 0)
        {
            StopTone(ToneEndReason.Stopped);
            Reply(id, tag, K5Status.Ok, new WireWriter().U8(0).U16(0).ToArray());
            return;
        }

        if (f is < 100 or > 5000)
        {
            Error(id, tag, K5Status.Range, 0);
            return;
        }

        if (mode > 1 || (mode == 0 && level > 8000) || (mode == 1 && level > 127))
        {
            Error(id, tag, K5Status.Range, mode > 1 ? 2 : 3);
            return;
        }

        if (dur > 60000)
        {
            Error(id, tag, K5Status.Range, 5);
            return;
        }

        int gain;
        if (mode == 0)
        {
            if (_eeprom[0x1D6F] == 0xFF || !V2BlockValid())
            {
                Error(id, tag, K5Status.Unsupported);
                return;
            }

            gain = Math.Clamp((int)Math.Round(_eeprom[0x1D6F] * level / 3000.0), 0, 127);
        }
        else
        {
            gain = level;
        }

        StopTone(ToneEndReason.Replaced);
        _toneRunning = true;
        _toneTimer = After(dur, () =>
        {
            _toneTimer = null;
            StopTone(ToneEndReason.Elapsed);
        });
        Reply(id, tag, K5Status.Ok, new WireWriter().U8(gain).U16((int)Math.Round(f * 10.32444)).ToArray());
    }

    private void StopTone(ToneEndReason reason)
    {
        if (!_toneRunning)
        {
            return;
        }

        _toneRunning = false;
        Cancel(ref _toneTimer);
        StoreEvent(10, [(byte)reason]);
    }

    private void RegRead(ushort id, byte tag, byte[] a)
    {
        if (a.Length != 3)
        {
            Error(id, tag, K5Status.BadLength);
            return;
        }

        int first = a[0];
        int count = a[1];
        if (count is < 1 or > 64 || first + count > 0x80)
        {
            Error(id, tag, K5Status.Range, count is < 1 or > 64 ? 1 : 0);
            return;
        }

        var w = new WireWriter().U8(first).U8(count);
        for (int i = 0; i < count; i++)
        {
            w.U16(ReadReg(first + i, (a[2] & 1) != 0));
        }

        Reply(id, tag, K5Status.Ok, w.ToArray());
    }

    private void RegWrite(ushort id, byte tag, byte[] a)
    {
        if (a.Length < 1 || a[0] is < 1 or > 16 || a.Length != 1 + 3 * a[0])
        {
            Error(id, tag, a.Length >= 1 && a[0] is < 1 or > 16 ? K5Status.Range : K5Status.BadLength);
            return;
        }

        int n = a[0];
        for (int i = 0; i < n; i++)
        {
            int reg = a[1 + 3 * i];
            if (K5Safety.IsRegisterRefused(reg))
            {
                Error(id, tag, K5Status.Refused, reg);
                return;
            }
        }

        var w = new WireWriter().U8(n);
        for (int i = 0; i < n; i++)
        {
            int reg = a[1 + 3 * i];
            _regs[reg] = BinaryPrimitives.ReadUInt16LittleEndian(a.AsSpan(2 + 3 * i));
            w.U8(reg).U16(_regs[reg]);
        }

        Reply(id, tag, K5Status.Ok, w.ToArray());
    }

    private List<RegisterOverride> EepromOverrides()
    {
        var list = new List<RegisterOverride>();
        if (!SettingsBlockValid())
        {
            return list;
        }

        for (int i = 0; i < 8; i++)
        {
            int at = 0x1D10 + 8 * i;
            byte phase = _eeprom[at];
            byte reg = _eeprom[at + 1];
            if (phase is 0 or 0xFF || reg == 0xFF)
            {
                break;
            }

            list.Add(new RegisterOverride((OverridePhase)phase, reg, BinaryPrimitives.ReadUInt16LittleEndian(_eeprom.AsSpan(at + 2)), BinaryPrimitives.ReadUInt16LittleEndian(_eeprom.AsSpan(at + 4))));
        }

        return list;
    }

    private void Override(ushort id, byte tag, byte[] a)
    {
        if (a.Length < 5 || a.Length != 5 + 6 * a[4])
        {
            Error(id, tag, K5Status.BadLength);
            return;
        }

        int op = a[0];
        int expiryS = BinaryPrimitives.ReadUInt16LittleEndian(a.AsSpan(1));
        int keyUps = a[3];
        int n = a[4];
        switch (op)
        {
            case 0:
                break;
            case 1:
                if (n == 0 || _ramOverrides.Count + n > 8)
                {
                    Error(id, tag, K5Status.Range, 4);
                    return;
                }

                var add = new List<RegisterOverride>();
                for (int i = 0; i < n; i++)
                {
                    int at = 5 + 6 * i;
                    var e = new RegisterOverride((OverridePhase)a[at], a[at + 1], BinaryPrimitives.ReadUInt16LittleEndian(a.AsSpan(at + 2)), BinaryPrimitives.ReadUInt16LittleEndian(a.AsSpan(at + 4)));
                    if (K5Safety.IsRegisterRefused(e.Register))
                    {
                        Error(id, tag, K5Status.Refused, e.Register);
                        return;
                    }

                    if (e.Phase is OverridePhase.None || (byte)e.Phase > 3)
                    {
                        Error(id, tag, K5Status.Range, at);
                        return;
                    }

                    add.Add(e);
                }

                _ramOverrides.AddRange(add);
                foreach (var e in add.Where(x => (x.Phase & OverridePhase.Rx) != 0))
                {
                    ApplyOverride(e);
                }

                Cancel(ref _overrideTimer);
                _overrideExpiresAt = 0;
                if (expiryS > 0)
                {
                    _overrideExpiresAt = Ts(expiryS * 1000.0);
                    _overrideTimer = After(expiryS * 1000.0, () =>
                    {
                        _overrideTimer = null;
                        ExpireOverrides(0);
                    });
                }

                _overrideKeyUps = keyUps;
                break;
            case 2:
                _ramOverrides.Clear();
                Cancel(ref _overrideTimer);
                _overrideKeyUps = 0;
                break;
            case 3 or 4:
                if (!SettingsBlockValid())
                {
                    Error(id, tag, K5Status.Eeprom);
                    return;
                }

                Array.Fill(_eeprom, (byte)0xFF, 0x1D10, 0x40);
                if (op == 3)
                {
                    for (int i = 0; i < _ramOverrides.Count; i++)
                    {
                        var e = _ramOverrides[i];
                        int at = 0x1D10 + 8 * i;
                        _eeprom[at] = (byte)e.Phase;
                        _eeprom[at + 1] = e.Register;
                        BinaryPrimitives.WriteUInt16LittleEndian(_eeprom.AsSpan(at + 2), e.AndMask);
                        BinaryPrimitives.WriteUInt16LittleEndian(_eeprom.AsSpan(at + 4), e.OrValue);
                    }

                    _ramOverrides.Clear();
                    Cancel(ref _overrideTimer);
                    _overrideKeyUps = 0;
                }

                Counter(13);
                break;
            default:
                Error(id, tag, K5Status.Range, 0);
                return;
        }

        int sLeft = _overrideTimer is null ? 0xFFFF : (int)Math.Ceiling(_time.GetElapsedTime(_time.GetTimestamp(), _overrideExpiresAt).TotalSeconds);
        var w = new WireWriter().U8(_ramOverrides.Count).U16(sLeft).U8(_overrideKeyUps == 0 ? 0xFF : _overrideKeyUps);
        foreach (var e in _ramOverrides)
        {
            w.U8((int)e.Phase).U8(e.Register).U16(e.AndMask).U16(e.OrValue);
        }

        var ee = EepromOverrides();
        w.U8(ee.Count);
        foreach (var e in ee)
        {
            w.U8((int)e.Phase).U8(e.Register).U16(e.AndMask).U16(e.OrValue);
        }

        Reply(id, tag, K5Status.Ok, w.ToArray());
    }

    private void ApplyOverride(RegisterOverride e)
    {
        int v = (_regs[e.Register] & e.AndMask) | e.OrValue;
        if (e.Register == 0x40 && (v & 0xFFF) > Deviation.MaxRegister)
        {
            v = (v & 0xF000) | Deviation.MaxRegister;
        }

        _regs[e.Register] = (ushort)v;
    }

    private void OnKeyUpOverrides()
    {
        foreach (var e in _ramOverrides.Where(x => (x.Phase & OverridePhase.Tx) != 0))
        {
            ApplyOverride(e);
        }

        if (_overrideKeyUps > 0 && _ramOverrides.Count > 0 && --_overrideKeyUps == 0)
        {
            ExpireOverrides(1);
        }
    }

    private void ExpireOverrides(int reason)
    {
        int n = _ramOverrides.Count;
        _ramOverrides.Clear();
        Cancel(ref _overrideTimer);
        _overrideKeyUps = 0;
        StoreEvent(11, [(byte)reason, (byte)n]);
    }

    private void Replay(ushort id, byte tag, byte[] a)
    {
        if (a.Length != 2)
        {
            Error(id, tag, K5Status.BadLength);
            return;
        }

        ushort from = BinaryPrimitives.ReadUInt16LittleEndian(a);
        int span = (ushort)(_nextSeq - from);
        int first = -1;
        int count = 0;
        foreach (var e in _ring)
        {
            if ((ushort)(e.Seq - from) >= span || span == 0)
            {
                continue;
            }

            if (first < 0)
            {
                first = e.Seq;
            }

            var copy = (byte[])e.Payload.Clone();
            copy[10] |= (byte)RadioEventFlags.Replay;
            e.Sent = true;
            SendV2Frame(BinaryPrimitives.ReadUInt16LittleEndian(copy), copy.AsSpan(4).ToArray());
            count++;
        }

        ushort oldest = _ring.Count > 0 ? _ring[0].Seq : _nextSeq;
        Reply(id, tag, K5Status.Ok, new WireWriter().U16(first < 0 ? from : first).U8(count).U16(oldest).U16(_nextSeq).ToArray());
    }

    private void Counters(ushort id, byte tag, byte[] a)
    {
        if (a.Length != 1)
        {
            Error(id, tag, K5Status.BadLength);
            return;
        }

        var w = new WireWriter().U8(_counters.Length);
        foreach (uint c in _counters)
        {
            w.U32(c);
        }

        if ((a[0] & 1) != 0)
        {
            Array.Clear(_counters);
        }

        Reply(id, tag, K5Status.Ok, w.ToArray());
    }
}
