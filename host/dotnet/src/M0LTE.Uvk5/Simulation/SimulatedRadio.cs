using System.Buffers.Binary;
using System.Text;
using M0LTE.Uvk5.Protocol;

namespace M0LTE.Uvk5.Simulation;

/// <summary>Options for <see cref="SimulatedRadio"/>.</summary>
public sealed record SimulatedRadioOptions
{
    /// <summary>Which firmware to behave as.</summary>
    public FirmwareKind Firmware { get; init; } = FirmwareKind.PacketV2;

    /// <summary>Version string for the hello reply; a default per firmware kind if null.</summary>
    public string? Version { get; init; }

    /// <summary>Clock. A <c>FakeTimeProvider</c> makes the simulation deterministic.</summary>
    public TimeProvider TimeProvider { get; init; } = TimeProvider.System;

    /// <summary>Initial 8 KiB EEPROM image; a plausible one (valid settings block, non-blank calibration) if null.</summary>
    public byte[]? Eeprom { get; init; }

    /// <summary>Seed for the synthetic calibration area, so two simulated radios can differ.</summary>
    public int CalibrationSeed { get; init; } = 1;

    /// <summary>Initial frequency, Hz.</summary>
    public long FrequencyHz { get; init; } = 144_800_000;

    /// <summary>Which frequencies the TX band plan allows.</summary>
    public Func<long, bool> TxAllowed { get; init; } = f => f is >= 144_000_000 and < 146_000_000 or >= 430_000_000 and < 440_000_000;

    /// <summary>Stored-event ring capacity (the spec asks for at least 16).</summary>
    public int EventRingCapacity { get; init; } = 32;

    /// <summary>
    /// AIOC RXIGNPTT: radio-to-host bytes are dropped while PTT is held (the AIOC default). Set
    /// false to model an AIOC with register 0x60 = 0x00000100.
    /// </summary>
    public bool AiocDropsRadioOutputWhileKeyed { get; init; } = true;

    /// <summary>Stock firmware only: whether it has the BK4819 register commands (0x0601/0x0602).</summary>
    public bool StockHasRegisterCommands { get; init; }

    /// <summary>Delay after a PTT release before deferred events are sent (spec: 2 ms). Zero sends them at once.</summary>
    public TimeSpan DeferralResumeDelay { get; init; } = TimeSpan.FromMilliseconds(2);

    /// <summary>Battery voltage, mV.</summary>
    public int BatteryMillivolts { get; init; } = 7800;
}

/// <summary>One transmission the simulated radio made, in radio milliseconds.</summary>
/// <param name="PressedAtMs">When PTT was pressed.</param>
/// <param name="RfAtMs">When RF was ready.</param>
/// <param name="EndedAtMs">When the carrier went off, null while transmitting.</param>
/// <param name="EndReason">Why it ended.</param>
/// <param name="LateKey">It keyed late because of the serial lock (v2: reported; v1 and stock: silent, eating the start of the frame).</param>
public sealed record SimulatedTransmission(uint PressedAtMs, uint RfAtMs, uint? EndedAtMs, TxEndReason? EndReason, bool LateKey);

/// <summary>
/// An in-process UV-K5 behaving as stock firmware, packet firmware v1 or packet firmware v2
/// (docs/protocol-v2.md), for tests: the host side is <see cref="HostStream"/>; the test drives
/// the world with <see cref="PressPtt"/>, <see cref="ReleasePtt"/>, <see cref="StartCarrier"/>
/// and so on, and inspects what the radio did. Thread-safe.
/// </summary>
/// <remarks>
/// <para>Modelled: framing and both serial modes (chosen per hello from the raw id bytes), the
/// legacy commands with their quirks (session id on EEPROM commands, 0xFFFF CRC on legacy replies,
/// 0x0602 silent, packet firmware refusing unaligned or calibration EEPROM writes with no reply),
/// the PTT lock per firmware (v2: SERIAL_LOCK_MS with late key or refusal; v1: 1.0 to 1.5 s with a
/// silent late key; stock: 6 s after a hello or EEPROM command), the AIOC (a host write releases
/// PTT; radio output dropped while PTT is held), every v2 command, the stored-event ring with
/// deferral while keyed, EVENTS_LOST and replay, heartbeats, RSSI stream, busy edges and burst
/// reports, TX start/end/refused/timeout, level tone, trial register overrides with expiry, the v1
/// settings reload after EEPROM writes, and the power-on banner.</para>
/// <para>Not modelled: audio, the chip's real register semantics, frequency records in EEPROM
/// (frequency persists in RAM only), byte timing on the wire (frames arrive whole and at once).</para>
/// <para><see cref="Violations"/> collects things a real radio would suffer from: a host write
/// while PTT was held, an EEPROM write into the calibration area, frames over the 256-byte ring.
/// Tests should assert it stays empty.</para>
/// </remarks>
public sealed partial class SimulatedRadio : IDisposable
{
    private const int LateKeyMaxMs = 30;

    private readonly SimulatedRadioOptions _options;
    private readonly TimeProvider _time;
    private readonly Lock _gate = new();
    private readonly ByteQueue _toHost = new();
    private readonly List<byte> _rx = [];
    private readonly byte[] _eeprom;
    private readonly ushort[] _regs = new ushort[0x80];
    private readonly List<string> _violations = [];
    private readonly List<SimulatedTransmission> _transmissions = [];
    private readonly List<ushort> _received = [];
    private readonly List<ITimer> _timers = [];

    private long _bootTs;
    private bool _obfuscated = true;
    private uint _session;
    private long _lockUntil;
    private bool _pttPressed;
    private bool _transmitting;
    private bool _latched;
    private bool _lateKeyPending;
    private uint _pressMs;
    private uint _txStartMs;
    private ITimer? _lateKeyTimer;
    private ITimer? _txTimeoutTimer;
    private ITimer? _reloadTimer;
    private int _dropReplies;
    private int _cutNextFrame = -1;
    private bool _disposed;

    /// <summary>Creates a simulated radio, powered on.</summary>
    public SimulatedRadio(SimulatedRadioOptions? options = null)
    {
        _options = options ?? new SimulatedRadioOptions();
        _time = _options.TimeProvider;
        _eeprom = _options.Eeprom is { } e ? (byte[])e.Clone() : DefaultEeprom(_options);
        if (_eeprom.Length != K5Safety.EepromSize)
        {
            throw new ArgumentException("EEPROM image must be 8 KiB", nameof(options));
        }

        for (int i = 0; i < _regs.Length; i++)
        {
            _regs[i] = (ushort)(0x1000 + i);
        }

        HostStream = new HostEnd(this);
        Version = _options.Version ?? _options.Firmware switch
        {
            FirmwareKind.PacketV2 => "PKTFW v2sim",
            FirmwareKind.PacketV1 => "PKTFW 2951c48",
            _ => "EGZUMER v0.22",
        };
        BatteryMillivolts = _options.BatteryMillivolts;
        lock (_gate)
        {
            PowerOn(banner: false);
        }
    }

    /// <summary>The host end of the serial link: give this to <see cref="K5Radio.ConnectAsync"/>.</summary>
    public Stream HostStream { get; }

    /// <summary>The firmware being simulated.</summary>
    public FirmwareKind Firmware => _options.Firmware;

    /// <summary>The hello version string.</summary>
    public string Version { get; }

    /// <summary>Battery voltage, mV.</summary>
    public int BatteryMillivolts { get; private set; }

    /// <summary>True while transmitting.</summary>
    public bool IsTransmitting
    {
        get
        {
            lock (_gate)
            {
                return _transmitting;
            }
        }
    }

    /// <summary>True while PTT is held.</summary>
    public bool IsPttPressed
    {
        get
        {
            lock (_gate)
            {
                return _pttPressed;
            }
        }
    }

    /// <summary>Serial PTT lock remaining.</summary>
    public TimeSpan LockRemaining
    {
        get
        {
            lock (_gate)
            {
                return TimeSpan.FromMilliseconds(LockMs());
            }
        }
    }

    /// <summary>Radio clock, ms since power-on.</summary>
    public uint RadioTimeMs
    {
        get
        {
            lock (_gate)
            {
                return NowMs();
            }
        }
    }

    /// <summary>True while the host session is in obfuscated mode.</summary>
    public bool IsObfuscated
    {
        get
        {
            lock (_gate)
            {
                return _obfuscated;
            }
        }
    }

    /// <summary>Protocol violations a real radio would suffer from. Should stay empty.</summary>
    public IReadOnlyList<string> Violations
    {
        get
        {
            lock (_gate)
            {
                return [.. _violations];
            }
        }
    }

    /// <summary>Every transmission so far.</summary>
    public IReadOnlyList<SimulatedTransmission> Transmissions
    {
        get
        {
            lock (_gate)
            {
                return [.. _transmissions];
            }
        }
    }

    /// <summary>Ids of every valid frame received from the host, in order.</summary>
    public IReadOnlyList<ushort> ReceivedIds
    {
        get
        {
            lock (_gate)
            {
                return [.. _received];
            }
        }
    }

    /// <summary>A copy of the EEPROM.</summary>
    public byte[] Eeprom
    {
        get
        {
            lock (_gate)
            {
                return (byte[])_eeprom.Clone();
            }
        }
    }

    /// <summary>A copy of the BK4819 registers.</summary>
    public ushort[] Registers
    {
        get
        {
            lock (_gate)
            {
                return (ushort[])_regs.Clone();
            }
        }
    }

    /// <summary>The AIOC keys PTT (HID). Follows the firmware's PTT rules for the simulated kind.</summary>
    public void PressPtt()
    {
        lock (_gate)
        {
            if (_pttPressed)
            {
                return;
            }

            _pttPressed = true;
            _pressMs = NowMs();
            OnPress();
        }
    }

    /// <summary>The AIOC releases PTT (HID).</summary>
    public void ReleasePtt()
    {
        lock (_gate)
        {
            if (!_pttPressed)
            {
                return;
            }

            _pttPressed = false;
            OnRelease(TxEndReason.Released);
        }
    }

    /// <summary>A signal arrives on the channel.</summary>
    public void StartCarrier(Rssi rssi, int noise = 10, int glitch = 2)
    {
        lock (_gate)
        {
            _carrier = (rssi, noise, glitch);
            UpdateBusy(BusyCause.SquelchEdge | BusyCause.RssiEdge);
        }
    }

    /// <summary>The signal goes away.</summary>
    public void StopCarrier()
    {
        lock (_gate)
        {
            _carrier = null;
            UpdateBusy(BusyCause.SquelchEdge | BusyCause.RssiEdge);
        }
    }

    /// <summary>Sets the battery voltage; a class change sends a BATTERY event.</summary>
    public void SetBattery(int millivolts)
    {
        lock (_gate)
        {
            var before = BatteryClassOf(BatteryMillivolts);
            BatteryMillivolts = millivolts;
            var after = BatteryClassOf(millivolts);
            if (after != before)
            {
                StoreEvent(7, new WireWriter().U8((int)after).U8(BatteryLevel()).U16(millivolts).ToArray());
            }
        }
    }

    /// <summary>The operator changes settings on the radio's keypad (PARAMS_CHANGED, source keypad).</summary>
    public void ChangeFromKeypad(RadioSettings changes)
    {
        lock (_gate)
        {
            uint mask = 0;
            foreach (var (id, v) in ParameterCodec.ToWire(changes))
            {
                _live[id] = v;
                mask |= 1u << (int)id;
            }

            StoreEvent(8, new WireWriter().U8((int)ParamsChangeSource.Keypad).U32(mask).ToArray());
        }
    }

    /// <summary>Reboots (as a power cycle): RAM state lost, mode obfuscated, the banner line printed.</summary>
    public void Reboot()
    {
        lock (_gate)
        {
            PowerOn(banner: true);
        }
    }

    /// <summary>Sends raw bytes to the host (garbage, text), subject to the AIOC's PTT rule.</summary>
    public void InjectBytes(ReadOnlySpan<byte> bytes)
    {
        lock (_gate)
        {
            Emit(bytes.ToArray());
        }
    }

    /// <summary>The next <paramref name="count"/> replies are lost.</summary>
    public void DropReplies(int count)
    {
        lock (_gate)
        {
            _dropReplies = count;
        }
    }

    /// <summary>The next frame sent to the host is cut after <paramref name="keepBytes"/> bytes (as the AIOC does when PTT is asserted mid-frame).</summary>
    public void CutNextFrame(int keepBytes)
    {
        lock (_gate)
        {
            _cutNextFrame = keepBytes;
        }
    }

    /// <inheritdoc/>
    public void Dispose()
    {
        lock (_gate)
        {
            if (_disposed)
            {
                return;
            }

            _disposed = true;
            foreach (var t in _timers)
            {
                t.Dispose();
            }

            _timers.Clear();
        }

        _toHost.Complete();
    }

    // ------------------------------------------------------------------ plumbing

    private uint NowMs() => (uint)_time.GetElapsedTime(_bootTs).TotalMilliseconds;

    private int NowUs() => (int)(_time.GetElapsedTime(_bootTs).Ticks / 10 % 1000);

    private long Ts(double msFromNow) => _time.GetTimestamp() + (long)(msFromNow / 1000.0 * _time.TimestampFrequency);

    private int LockMs()
    {
        long now = _time.GetTimestamp();
        return _lockUntil <= now ? 0 : (int)Math.Ceiling(_time.GetElapsedTime(now, _lockUntil).TotalMilliseconds);
    }

    private ITimer After(double ms, Action action)
    {
        ITimer? timer = null;
        timer = _time.CreateTimer(_ =>
        {
            lock (_gate)
            {
                if (_disposed)
                {
                    return;
                }

                _timers.Remove(timer!);
                action();
            }
        }, null, TimeSpan.FromMilliseconds(Math.Max(0, ms)), Timeout.InfiniteTimeSpan);
        _timers.Add(timer);
        return timer;
    }

    private void Cancel(ref ITimer? timer)
    {
        if (timer is not null)
        {
            timer.Dispose();
            _timers.Remove(timer);
            timer = null;
        }
    }

    /// <summary>Radio to host, through the AIOC.</summary>
    private void Emit(byte[] bytes)
    {
        if (_disposed)
        {
            return;
        }

        if (_cutNextFrame >= 0 && bytes.Length > 0 && bytes[0] == 0xAB)
        {
            bytes = bytes[..Math.Min(_cutNextFrame, bytes.Length)];
            _cutNextFrame = -1;
        }

        if (_pttPressed && _options.AiocDropsRadioOutputWhileKeyed)
        {
            return;
        }

        _toHost.Write(bytes);
    }

    private void SendLegacyReply(ushort id, byte[] body)
    {
        if (_dropReplies > 0)
        {
            _dropReplies--;
            return;
        }

        Emit(K5FrameCodec.EncodeWithoutCrc(K5FrameCodec.BuildPayload(id, body), _obfuscated));
    }

    private void SendV2Frame(ushort id, byte[] body) =>
        Emit(K5FrameCodec.Encode(id, body, _obfuscated));

    private void OnHostBytes(ReadOnlySpan<byte> data)
    {
        lock (_gate)
        {
            if (_disposed)
            {
                return;
            }

            if (_pttPressed)
            {
                // AIOC TXFRCPTT: a serial write releases PTT at once.
                _violations.Add($"host wrote {data.Length} bytes while PTT was held (the AIOC released PTT)");
                _pttPressed = false;
                OnRelease(TxEndReason.SerialFrame);
            }

            foreach (byte b in data)
            {
                _rx.Add(b);
            }

            ParseHostFrames();
        }
    }

    /// <summary>The firmware's parser, on the raw bytes: the mode comes from the raw hello id.</summary>
    private void ParseHostFrames()
    {
        while (true)
        {
            int i = FindHeader();
            if (i < 0)
            {
                bool keepAb = _rx.Count > 0 && _rx[^1] == 0xAB;
                _rx.Clear();
                if (keepAb)
                {
                    _rx.Add(0xAB);
                }

                return;
            }

            _rx.RemoveRange(0, i);
            if (_rx.Count < 8)
            {
                return;
            }

            int size = _rx[2] | (_rx[3] << 8);
            if (size + 8 > 256)
            {
                _violations.Add($"frame of {size + 8} bytes exceeds the 256-byte receive ring");
                _rx.RemoveRange(0, 2);
                Counter(2);
                continue;
            }

            if (_rx.Count < size + 8)
            {
                return;
            }

            if (_rx[size + 6] != 0xDC || _rx[size + 7] != 0xBA)
            {
                _rx.RemoveRange(0, 2);
                Counter(1);
                continue;
            }

            byte[] body = [.. _rx.GetRange(4, size + 2)];
            _rx.RemoveRange(0, size + 8);
            ushort rawId = (ushort)(body[0] | (body[1] << 8));
            if (_options.Firmware != FirmwareKind.Unknown)
            {
                if (rawId == 0x0514)
                {
                    _obfuscated = false;
                }
                else if (rawId == 0x6902)
                {
                    _obfuscated = true;
                }
            }

            if (_obfuscated)
            {
                K5FrameCodec.Obfuscate(body);
            }

            ushort crc = BinaryPrimitives.ReadUInt16LittleEndian(body.AsSpan(size));
            if (crc != K5FrameCodec.Crc16(body.AsSpan(0, size)))
            {
                Counter(1);
                continue;
            }

            byte[] payload = body[..size];
            if (payload.Length < 4)
            {
                continue;
            }

            ushort id = BinaryPrimitives.ReadUInt16LittleEndian(payload);
            _received.Add(id);
            Counter(0);
            OnValidFrame();
            HandleFrame(id, payload);
        }
    }

    private int FindHeader()
    {
        for (int i = 0; i + 1 < _rx.Count; i++)
        {
            if (_rx[i] == 0xAB && _rx[i + 1] == 0xCD)
            {
                return i;
            }
        }

        return -1;
    }

    /// <summary>Every valid frame starts the lock and ends any transmission (5.3 rule 1).</summary>
    private void OnValidFrame()
    {
        if (_reloadTimer is not null)
        {
            // The settings reload waits for the session to go quiet: every frame restarts it.
            Cancel(ref _reloadTimer);
            _reloadTimer = After(_options.Firmware == FirmwareKind.PacketV2 ? 1000 : 1500 - NowMs() % 500, ReloadFromEeprom);
        }

        switch (_options.Firmware)
        {
            case FirmwareKind.PacketV2:
                _lockUntil = Ts(Param(RadioParameterId.SerialLockMs));
                if (_transmitting)
                {
                    EndTx(TxEndReason.SerialFrame);
                }

                break;
            case FirmwareKind.PacketV1:
                // SERIAL_PTT_LOCK_500ms = 3, counted down in the 500 ms slice: 1.0 to 1.5 s.
                _lockUntil = Ts(1500 - NowMs() % 500);
                if (_transmitting)
                {
                    EndTx(TxEndReason.SerialFrame);
                }

                break;
        }
    }

    private void HandleFrame(ushort id, byte[] payload)
    {
        ReadOnlySpan<byte> b = payload.AsSpan(4);
        switch (id)
        {
            case MessageIds.Hello or MessageIds.HelloAlt:
                if (b.Length >= 4)
                {
                    _session = BinaryPrimitives.ReadUInt32LittleEndian(b);
                }

                StockLock();
                if (_options.Firmware == FirmwareKind.PacketV2)
                {
                    ResetSubscriptionToStored();
                }

                SendHelloReply();
                break;
            case MessageIds.EepromRead:
                HandleEepromRead(b);
                break;
            case MessageIds.EepromWrite:
                HandleEepromWrite(b);
                break;
            case MessageIds.Rssi:
            {
                var (rssi, noise, glitch) = Signal();
                SendLegacyReply(MessageIds.RssiReply, new WireWriter().U16(rssi).U8(noise).U8(glitch).ToArray());
                break;
            }

            case MessageIds.Battery:
            {
                ushort cal = BinaryPrimitives.ReadUInt16LittleEndian(_eeprom.AsSpan(0x1F46));
                int raw = (int)Math.Round(BatteryMillivolts / 1000.0 * cal / 7.6);
                SendLegacyReply(MessageIds.BatteryReply, new WireWriter().U16(raw).U16(0).ToArray());
                break;
            }

            case MessageIds.Reboot:
                if (_transmitting)
                {
                    EndTx(TxEndReason.Other);
                }

                PowerOn(banner: true);
                break;
            case MessageIds.RegRead when HasRegisterCommands() && b.Length >= 1:
                SendLegacyReply(MessageIds.RegRead, new WireWriter().U8(b[0]).U16(ReadReg(b[0], true)).ToArray());
                break;
            case MessageIds.RegWrite when HasRegisterCommands() && b.Length >= 3:
                _regs[b[0] & 0x7F] = BinaryPrimitives.ReadUInt16LittleEndian(b[1..]);
                break;
            default:
                if (_options.Firmware == FirmwareKind.PacketV2 && id is >= MessageIds.V2First and <= MessageIds.V2RequestLast)
                {
                    HandleV2(id, payload);
                }

                // Every firmware ignores unknown ids silently.
                break;
        }
    }

    private bool HasRegisterCommands() => _options.Firmware != FirmwareKind.Stock || _options.StockHasRegisterCommands;

    private void StockLock()
    {
        if (_options.Firmware is FirmwareKind.Stock or FirmwareKind.Unknown)
        {
            // gSerialConfigCountDown_500ms = 12 on hello and EEPROM commands: 5.5 to 6 s + slice.
            _lockUntil = Ts(6500 - NowMs() % 500);
            if (_transmitting)
            {
                EndTx(TxEndReason.SerialFrame);
            }
        }
    }

    private void SendHelloReply()
    {
        var w = new WireWriter().Ascii(Version, 16).U8(0).U8(0).U8(0).U8(0);
        if (_options.Firmware == FirmwareKind.PacketV2)
        {
            w.U32(MessageIds.Pkt2Magic).U16(0x0200).U16(0).U32(0).U32(0);
        }
        else if (_options.Firmware == FirmwareKind.PacketV1)
        {
            w.U32(0).U32(0).U32(0).U32(0);
        }
        else
        {
            for (int i = 0; i < 16; i++)
            {
                w.U8(i);
            }
        }

        SendLegacyReply(MessageIds.HelloReply, w.ToArray());
    }

    private void HandleEepromRead(ReadOnlySpan<byte> b)
    {
        if (b.Length < 8)
        {
            return;
        }

        int off = BinaryPrimitives.ReadUInt16LittleEndian(b);
        int size = b[2];
        uint ts = BinaryPrimitives.ReadUInt32LittleEndian(b[4..]);
        StockLock();
        if (ts != _session || size > 128)
        {
            if (size > 128)
            {
                _violations.Add($"EEPROM read of {size} bytes overflows the firmware's reply buffer");
            }

            return;
        }

        var w = new WireWriter().U16(off).U8(size).U8(0);
        for (int i = 0; i < size; i++)
        {
            w.U8(_eeprom[(off + i) % K5Safety.EepromSize]);
        }

        SendLegacyReply(MessageIds.EepromReadReply, w.ToArray());
    }

    private void HandleEepromWrite(ReadOnlySpan<byte> b)
    {
        if (b.Length < 8)
        {
            return;
        }

        int off = BinaryPrimitives.ReadUInt16LittleEndian(b);
        int size = b[2];
        uint ts = BinaryPrimitives.ReadUInt32LittleEndian(b[4..]);
        StockLock();
        if (ts != _session || size % 8 != 0 || 8 + size > b.Length)
        {
            return;
        }

        bool packet = _options.Firmware is FirmwareKind.PacketV1 or FirmwareKind.PacketV2;
        for (int i = 0; i < size / 8; i++)
        {
            int a = off + i * 8;
            if (a + 8 > K5Safety.CalibrationStart || a % 8 != 0)
            {
                if (packet)
                {
                    return;         // refused whole, with no reply
                }

                _violations.Add($"calibration area written at 0x{a:X4}");
            }
        }

        for (int i = 0; i < size / 8; i++)
        {
            int a = off + i * 8;
            if (a < K5Safety.EepromSize)
            {
                b.Slice(8 + i * 8, 8).CopyTo(_eeprom.AsSpan(a));
            }
        }

        if (packet)
        {
            // Settings reload once the host has been quiet for a while (v1: 500 ms slices, v2: 1.0 s).
            Cancel(ref _reloadTimer);
            _reloadTimer = After(_options.Firmware == FirmwareKind.PacketV2 ? 1000 : 1500 - NowMs() % 500, ReloadFromEeprom);
        }

        SendLegacyReply(MessageIds.EepromWriteReply, new WireWriter().U16(off).ToArray());
    }

    private ushort ReadReg(int reg, bool includeFifo)
    {
        reg &= 0x7F;
        return reg switch
        {
            0x5F when !includeFifo => 0,
            0x67 => Signal().Rssi,
            0x65 => (ushort)Signal().Noise,
            0x63 => (ushort)Signal().Glitch,
            _ => _regs[reg],
        };
    }

    private (ushort Rssi, int Noise, int Glitch) Signal() =>
        _carrier is { } c && !_transmitting ? (c.Rssi.Raw, c.Noise, c.Glitch) : ((ushort)60, 90, 40);

    // ------------------------------------------------------------------ power

    private void PowerOn(bool banner)
    {
        foreach (var t in _timers)
        {
            t.Dispose();
        }

        _timers.Clear();
        _lateKeyTimer = _txTimeoutTimer = _reloadTimer = _heartbeatTimer = _streamTimer = _toneTimer = _overrideTimer = null;
        _bootTs = _time.GetTimestamp();
        _obfuscated = true;
        _lockUntil = _bootTs;
        _transmitting = false;
        _latched = false;
        _lateKeyPending = false;
        _rx.Clear();
        _session = 0;
        PowerOnV2();
        if (banner && _options.Firmware is FirmwareKind.PacketV1 or FirmwareKind.PacketV2)
        {
            Emit(Encoding.ASCII.GetBytes($"UV-K5 packet firmware, {Version}\r\n"));
        }

        if (_options.Firmware == FirmwareKind.PacketV2 && (_subMask & (1u << 12)) != 0)
        {
            StoreEvent(12, new WireWriter().U16(0x0200).U8(banner ? 2 : 1).ToArray());
        }
    }

    private static byte[] DefaultEeprom(SimulatedRadioOptions o)
    {
        var e = new byte[K5Safety.EepromSize];
        Array.Fill(e, (byte)0xFF);
        var rnd = new Random(o.CalibrationSeed);
        for (int i = 0; i < 0x0D00; i++)
        {
            e[i] = (byte)rnd.Next(256);
        }

        byte[] settings = [0x01, 0x01, 0x04, 0x1F, 0x56, 0x08, 0x56, 0x07, 0xFF, 0x0F, 0x03, 0x00, 0x00, 0xFF, 0xFF, 0xFF];
        settings.CopyTo(e, 0x1D00);
        byte[] timing = [0x05, 0x05, 0x01, 0x02, 0xFF, 0xFF, 0xFF, 0xFF];
        timing.CopyTo(e, 0x1D50);
        if (o.Firmware == FirmwareKind.PacketV2)
        {
            byte[] v2 = [0x01, 0x02, 0x01, 0x14, 0x6E, 0x00, 0x68, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0xFF];
            v2.CopyTo(e, 0x1D60);
        }

        for (int i = K5Safety.CalibrationStart; i < K5Safety.EepromSize; i++)
        {
            e[i] = (byte)rnd.Next(256);
        }

        ushort[] battery = [1900, 1990, 2040, 2090, 2150, 2300];
        for (int i = 0; i < battery.Length; i++)
        {
            BinaryPrimitives.WriteUInt16LittleEndian(e.AsSpan(0x1F40 + 2 * i), battery[i]);
        }

        return e;
    }

    // ------------------------------------------------------------------ PTT (5.3)

    private void OnPress()
    {
        if (_options.Firmware == FirmwareKind.PacketV2)
        {
            StopTone(ToneEndReason.Ptt);
            if (_latched)
            {
                return;
            }

            TxRefusedReason? bar = BatteryClassOf(BatteryMillivolts) switch
            {
                BatteryClass.Empty => TxRefusedReason.BatteryEmpty,
                BatteryClass.OverVoltage => TxRefusedReason.OverVoltage,
                _ => _options.TxAllowed((long)Param(RadioParameterId.FrequencyHz)) ? null : TxRefusedReason.TxBand,
            };
            if (bar is { } reason)
            {
                Refuse(reason, 0);
                return;
            }

            int lockMs = LockMs();
            if (lockMs == 0)
            {
                KeyUp(0, late: false);
            }
            else if (lockMs <= LateKeyMaxMs)
            {
                _lateKeyPending = true;
                _lateKeyTimer = After(lockMs, () =>
                {
                    _lateKeyTimer = null;
                    if (_pttPressed && _lateKeyPending && LockMs() == 0)
                    {
                        _lateKeyPending = false;
                        KeyUp((int)(NowMs() - _pressMs), late: true);
                    }
                });
            }
            else
            {
                Refuse(TxRefusedReason.Lock, lockMs);
            }

            return;
        }

        // v1 and stock: a press during the lock is not refused; the radio keys when it runs out.
        if (!_options.TxAllowed((long)Param(RadioParameterId.FrequencyHz)) || _latched)
        {
            return;
        }

        int wait = LockMs();
        if (wait == 0)
        {
            KeyUp(0, late: false);
        }
        else
        {
            _lateKeyPending = true;
            _lateKeyTimer = After(wait, () =>
            {
                _lateKeyTimer = null;
                if (_pttPressed && _lateKeyPending)
                {
                    _lateKeyPending = false;
                    KeyUp((int)(NowMs() - _pressMs), late: true);
                }
            });
        }
    }

    private void Refuse(TxRefusedReason reason, int lockMs)
    {
        _latched = true;
        Counter(9);
        StoreEvent(4, new WireWriter().U32(_pressMs).U8((int)reason).U16(lockMs).ToArray());
    }

    private void KeyUp(int lockDelayMs, bool late)
    {
        bool busyAtPress = _busy;
        if (_busy)
        {
            SetBusy(false, BusyCause.TransmissionStarted);
        }

        _transmitting = true;
        Counter(7);
        if (late)
        {
            Counter(11);
        }

        _txStartMs = NowMs() + (uint)(Param(RadioParameterId.PttPressMs) + Param(RadioParameterId.PaEnableDelayMs) + Param(RadioParameterId.PaBiasDelayMs));
        _transmissions.Add(new SimulatedTransmission(_pressMs, _txStartMs, null, null, late));
        OnKeyUpOverrides();
        if (_options.Firmware == FirmwareKind.PacketV2)
        {
            int bw = (int)Param(RadioParameterId.Bandwidth);
            ushort dev = (ushort)Param(bw == 0 ? RadioParameterId.DeviationWide : RadioParameterId.DeviationNarrow);
            StoreEvent(2, new WireWriter().U32(_pressMs).U32((uint)Param(RadioParameterId.FrequencyHz)).U8((int)Param(RadioParameterId.Power))
                .U8(bw).U16(dev).U16(lockDelayMs).U8((busyAtPress ? 1 : 0) | (late ? 2 : 0)).ToArray(), atMs: _txStartMs);
        }

        _txTimeoutTimer = After(TxTimeoutSeconds() * 1000.0, () =>
        {
            _txTimeoutTimer = null;
            if (_transmitting)
            {
                Counter(8);
                EndTx(TxEndReason.Timeout);
                _latched = true;
            }
        });
    }

    private void OnRelease(TxEndReason reasonIfTransmitting)
    {
        _lateKeyPending = false;
        Cancel(ref _lateKeyTimer);
        _latched = false;
        if (_transmitting)
        {
            EndTx(reasonIfTransmitting);
        }

        // Deferred events go out shortly after the release.
        if (_options.DeferralResumeDelay <= TimeSpan.Zero)
        {
            FlushEvents();
        }
        else
        {
            After(_options.DeferralResumeDelay.TotalMilliseconds, FlushEvents);
        }
    }

    private void EndTx(TxEndReason reason)
    {
        _transmitting = false;
        Cancel(ref _txTimeoutTimer);
        uint now = NowMs();
        uint releaseMs = now;
        uint off = reason == TxEndReason.Released ? now + (uint)Param(RadioParameterId.PttReleaseMs) : now;
        uint rxReady = off + 3;
        var last = _transmissions[^1];
        _transmissions[^1] = last with { EndedAtMs = off, EndReason = reason };
        if (_options.Firmware == FirmwareKind.PacketV2)
        {
            StoreEvent(3, new WireWriter().U32(_txStartMs).U32(releaseMs).U32(rxReady).U8((int)reason).U16(0xFFFF).U16(0xFFFF).U16(0).ToArray(), atMs: off);
        }

        UpdateBusy(BusyCause.None);
    }

    private int TxTimeoutSeconds() => (int)Param(RadioParameterId.TxTimeoutSeconds);

    private static BatteryClass BatteryClassOf(int mv) => mv switch
    {
        < 6300 => BatteryClass.Empty,
        > 8900 => BatteryClass.OverVoltage,
        < 6800 => BatteryClass.Low,
        _ => BatteryClass.Normal,
    };

    private int BatteryLevel() => BatteryMillivolts switch
    {
        < 6300 => 0,
        > 8900 => 7,
        < 6800 => 1,
        _ => Math.Clamp(2 + (BatteryMillivolts - 6800) / 400, 2, 6),
    };

    private sealed class HostEnd(SimulatedRadio radio) : Stream
    {
        public override bool CanRead => true;

        public override bool CanSeek => false;

        public override bool CanWrite => true;

        public override long Length => throw new NotSupportedException();

        public override long Position { get => throw new NotSupportedException(); set => throw new NotSupportedException(); }

        public override void Flush()
        {
        }

        public override Task FlushAsync(CancellationToken cancellationToken) => Task.CompletedTask;

        public override int Read(byte[] buffer, int offset, int count) =>
            radio._toHost.ReadAsync(buffer.AsMemory(offset, count), CancellationToken.None).AsTask().GetAwaiter().GetResult();

        public override ValueTask<int> ReadAsync(Memory<byte> buffer, CancellationToken cancellationToken = default) =>
            radio._toHost.ReadAsync(buffer, cancellationToken);

        public override Task<int> ReadAsync(byte[] buffer, int offset, int count, CancellationToken cancellationToken) =>
            ReadAsync(buffer.AsMemory(offset, count), cancellationToken).AsTask();

        public override void Write(byte[] buffer, int offset, int count) => radio.OnHostBytes(buffer.AsSpan(offset, count));

        public override ValueTask WriteAsync(ReadOnlyMemory<byte> buffer, CancellationToken cancellationToken = default)
        {
            radio.OnHostBytes(buffer.Span);
            return ValueTask.CompletedTask;
        }

        public override Task WriteAsync(byte[] buffer, int offset, int count, CancellationToken cancellationToken)
        {
            radio.OnHostBytes(buffer.AsSpan(offset, count));
            return Task.CompletedTask;
        }

        public override long Seek(long offset, SeekOrigin origin) => throw new NotSupportedException();

        public override void SetLength(long value) => throw new NotSupportedException();

        protected override void Dispose(bool disposing)
        {
            if (disposing)
            {
                radio._toHost.Complete();
            }

            base.Dispose(disposing);
        }
    }
}
