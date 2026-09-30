namespace M0LTE.Uvk5.Client;

/// <summary>
/// Knows when the radio will next honour a PTT press, from what the host has sent and what the
/// radio replied (protocol v2 section 5.2). Thread-safe.
/// </summary>
/// <remarks>
/// <para>Every valid host frame starts the radio's serial PTT lock. The rules:</para>
/// <list type="bullet">
/// <item>v2, after a 0x50xx reply: <c>t_reply + lock_ms</c> (the reply says how long is left).</item>
/// <item>v2, after a legacy reply: <c>t_reply + SERIAL_LOCK_MS + 2 ms</c>.</item>
/// <item>v2, no reply (legacy 0x0602, which only a bench build acts on) or a lost reply: <c>t_write_done + 0.27 ms x frame_bytes + 12 ms + SERIAL_LOCK_MS</c>.</item>
/// <item>v1: the lock is 1.0 to 1.5 s after the last frame and a press inside it is not refused but
/// keyed late, eating the start of the frame; so the allowance (default 2 s) runs from the moment
/// the last frame has left the wire.</item>
/// <item>Stock and unidentified firmware: about 6 s after a hello or EEPROM command (upstream 12
/// ticks of 500 ms, so up to 6.5 s); applied after every frame to be safe.</item>
/// </list>
/// <para>"Left the wire" is the time the write completed plus 0.27 ms per byte: a USB serial
/// adapter accepts the bytes long before the UART has sent them, and keying while a frame is
/// still going out cuts it and can leave the radio's parser deaf for a while.</para>
/// </remarks>
internal sealed class PttLockTracker
{
    /// <summary>Time on the wire per byte from the host at 38400 8N1 (10 bits), with margin.</summary>
    public static readonly TimeSpan PerByte = TimeSpan.FromMicroseconds(270);

    private readonly TimeProvider _time;
    private readonly Lock _gate = new();
    private long _readyAt;
    private long _lastFrameOffWire;
    private bool _anyFrame;

    public PttLockTracker(TimeProvider time, TimeSpan v1Allowance, TimeSpan stockAllowance)
    {
        _time = time;
        V1Allowance = v1Allowance;
        StockAllowance = stockAllowance;
        _readyAt = time.GetTimestamp();
    }

    public FirmwareKind Kind { get; private set; } = FirmwareKind.Unknown;

    /// <summary>SERIAL_LOCK_MS in force on v2; the protocol maximum until GET_INFO says otherwise.</summary>
    public TimeSpan SerialLock { get; private set; } = TimeSpan.FromMilliseconds(1500);

    public TimeSpan V1Allowance { get; }

    public TimeSpan StockAllowance { get; }

    /// <summary>Timestamp (TimeProvider ticks) from which a press is honoured.</summary>
    public long ReadyAt
    {
        get
        {
            lock (_gate)
            {
                return _readyAt;
            }
        }
    }

    public TimeSpan Remaining
    {
        get
        {
            long now = _time.GetTimestamp();
            long ready = ReadyAt;
            return ready <= now ? TimeSpan.Zero : _time.GetElapsedTime(now, ready);
        }
    }

    /// <summary>The firmware was identified (or re-identified): re-derive the lock from the last frame.</summary>
    public void SetFirmware(FirmwareKind kind, TimeSpan? serialLock)
    {
        lock (_gate)
        {
            Kind = kind;
            if (serialLock is { } sl)
            {
                SerialLock = sl;
            }

            if (_anyFrame && kind is FirmwareKind.PacketV1 or FirmwareKind.Stock)
            {
                _readyAt = Add(_lastFrameOffWire, kind == FirmwareKind.PacketV1 ? V1Allowance : StockAllowance);
            }
        }
    }

    /// <summary>A frame of <paramref name="frameBytes"/> finished being written (handed to the OS).</summary>
    public void OnFrameWritten(int frameBytes)
    {
        long now = _time.GetTimestamp();
        lock (_gate)
        {
            long offWire = Add(now, PerByte * frameBytes);
            _lastFrameOffWire = offWire;
            _anyFrame = true;
            long ready = Kind switch
            {
                FirmwareKind.PacketV2 => Add(offWire, TimeSpan.FromMilliseconds(12) + SerialLock),
                FirmwareKind.PacketV1 => Add(offWire, V1Allowance),
                _ => Add(offWire, StockAllowance),
            };

            _readyAt = Math.Max(_readyAt, ready);
        }
    }

    /// <summary>A v2 reply arrived carrying <paramref name="lockMs"/>: authoritative for the latest frame.</summary>
    public void OnV2Reply(ushort lockMs)
    {
        long now = _time.GetTimestamp();
        lock (_gate)
        {
            _readyAt = Math.Max(Add(now, TimeSpan.FromMilliseconds(lockMs)), _lastFrameOffWire);
        }
    }

    /// <summary>A legacy reply arrived.</summary>
    public void OnLegacyReply()
    {
        if (Kind != FirmwareKind.PacketV2)
        {
            return;
        }

        long now = _time.GetTimestamp();
        lock (_gate)
        {
            _readyAt = Math.Max(Add(now, SerialLock + TimeSpan.FromMilliseconds(2)), _lastFrameOffWire);
        }
    }

    private long Add(long timestamp, TimeSpan span) =>
        timestamp + (long)(span.TotalSeconds * _time.TimestampFrequency);
}
