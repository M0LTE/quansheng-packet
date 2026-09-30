using M0LTE.Uvk5.Protocol;

namespace M0LTE.Uvk5;

/// <summary>Event kinds, as subscription bits: event 0x50C0 + n is bit n.</summary>
[Flags]
public enum RadioEvents : uint
{
    /// <summary>No events.</summary>
    None = 0,

    /// <summary>Busy (carrier detect) edges, CD 0x50C0.</summary>
    Busy = 1u << 0,

    /// <summary>Per-burst signal report after each busy interval, RX_BURST 0x50C1.</summary>
    RxBurst = 1u << 1,

    /// <summary>Transmission started (RF ready), TX_START 0x50C2.</summary>
    TxStart = 1u << 2,

    /// <summary>Transmission ended, TX_END 0x50C3.</summary>
    TxEnd = 1u << 3,

    /// <summary>A PTT press was refused, TX_REFUSED 0x50C4.</summary>
    TxRefused = 1u << 4,

    /// <summary>Batched RSSI samples, RSSI_STREAM 0x50C5 (ephemeral; needs a stream period).</summary>
    RssiStream = 1u << 5,

    /// <summary>Periodic heartbeat, HEARTBEAT 0x50C6 (ephemeral; needs a heartbeat period).</summary>
    Heartbeat = 1u << 6,

    /// <summary>Battery class change, BATTERY 0x50C7.</summary>
    Battery = 1u << 7,

    /// <summary>Parameters changed (keypad, reload, serial, boot), PARAMS_CHANGED 0x50C8.</summary>
    ParamsChanged = 1u << 8,

    /// <summary>Stored events were lost, EVENTS_LOST 0x50C9.</summary>
    EventsLost = 1u << 9,

    /// <summary>Level tone ended, TONE_END 0x50CA.</summary>
    ToneEnd = 1u << 10,

    /// <summary>Trial register overrides expired, OVERRIDE_EXPIRED 0x50CB.</summary>
    OverrideExpired = 1u << 11,

    /// <summary>Radio booted (only via a persisted mask), BOOT 0x50CC.</summary>
    Boot = 1u << 12,

    /// <summary>
    /// What a TNC wants: busy edges, burst reports, TX start, end and refusals, battery, parameter
    /// changes, lost events, tone end and override expiry. Add <see cref="Heartbeat"/> with a period
    /// for clock tracking and link supervision.
    /// </summary>
    Tnc = Busy | RxBurst | TxStart | TxEnd | TxRefused | Battery | ParamsChanged | EventsLost | ToneEnd | OverrideExpired,
}

/// <summary>Event header flags.</summary>
[Flags]
public enum RadioEventFlags : byte
{
    /// <summary>None.</summary>
    None = 0,

    /// <summary>Re-sent by EVENT_REPLAY.</summary>
    Replay = 1 << 0,

    /// <summary>Held back while PTT was asserted and sent after the release.</summary>
    Deferred = 1 << 1,

    /// <summary>Waited more than 2 ms behind other output.</summary>
    Queued = 1 << 2,

    /// <summary>Not stored, not replayable (heartbeat, RSSI stream).</summary>
    Ephemeral = 1 << 3,

    /// <summary>The output queue was empty, so the frame's first byte left within 1 ms of the event time.</summary>
    TimeExact = 1 << 4,
}

/// <summary>Why a busy edge happened (bits of the CD cause and the RX_BURST end cause).</summary>
[Flags]
public enum BusyCause : byte
{
    /// <summary>None.</summary>
    None = 0,

    /// <summary>Squelch detector edge.</summary>
    SquelchEdge = 1 << 0,

    /// <summary>RSSI edge.</summary>
    RssiEdge = 1 << 1,

    /// <summary>Retune (busy forced closed).</summary>
    Retune = 1 << 2,

    /// <summary>A transmission started (busy forced closed).</summary>
    TransmissionStarted = 1 << 3,
}

/// <summary>Why a transmission ended.</summary>
public enum TxEndReason : byte
{
    /// <summary>PTT released.</summary>
    Released = 0,

    /// <summary>TX timeout.</summary>
    Timeout = 1,

    /// <summary>A serial frame arrived (host wrote to the port while keyed).</summary>
    SerialFrame = 2,

    /// <summary>Other.</summary>
    Other = 3,
}

/// <summary>Why a PTT press was refused.</summary>
public enum TxRefusedReason : byte
{
    /// <summary>The serial lock had more than the late-key limit left.</summary>
    Lock = 1,

    /// <summary>The frequency is not TX-allowed.</summary>
    TxBand = 2,

    /// <summary>Battery empty.</summary>
    BatteryEmpty = 3,

    /// <summary>Over-voltage.</summary>
    OverVoltage = 4,

    /// <summary>Reduced service.</summary>
    ReducedService = 5,

    /// <summary>The radio could not key within the late-key limit of the press (its main loop was held up, by a long legacy EEPROM write session for example).</summary>
    Late = 6,
}

/// <summary>Battery class in BATTERY events.</summary>
public enum BatteryClass : byte
{
    /// <summary>Normal (levels 2 to 6).</summary>
    Normal = 0,

    /// <summary>Low (level 1).</summary>
    Low = 1,

    /// <summary>Empty (level 0): TX refused.</summary>
    Empty = 2,

    /// <summary>Over-voltage (level 7): TX refused.</summary>
    OverVoltage = 3,
}

/// <summary>Who changed parameters.</summary>
public enum ParamsChangeSource : byte
{
    /// <summary>The radio's keypad or menu.</summary>
    Keypad = 0,

    /// <summary>EEPROM reload after legacy writes.</summary>
    EepromReload = 1,

    /// <summary>A serial command.</summary>
    Serial = 2,

    /// <summary>Boot.</summary>
    Boot = 3,
}

/// <summary>Why a level tone ended.</summary>
public enum ToneEndReason : byte
{
    /// <summary>Duration elapsed.</summary>
    Elapsed = 0,

    /// <summary>Stopped by request.</summary>
    Stopped = 1,

    /// <summary>PTT pressed.</summary>
    Ptt = 2,

    /// <summary>Retune.</summary>
    Retune = 3,

    /// <summary>Replaced by a new tone.</summary>
    Replaced = 4,
}

/// <summary>
/// An unsolicited event from a v2 radio. Radio times are milliseconds since the radio booted;
/// <see cref="EstimatedTime"/> maps the event instant to the host clock when the library has a
/// clock estimate (see <see cref="RadioClock"/>).
/// </summary>
public abstract record RadioEvent
{
    /// <summary>The event kind as a single subscription bit.</summary>
    public abstract RadioEvents Kind { get; }

    /// <summary>Sequence number: stored events count up from boot; ephemeral events carry the next stored seq.</summary>
    public ushort Sequence { get; init; }

    /// <summary>Radio clock at the event instant (meaning defined per event), ms since boot.</summary>
    public uint RadioTimeMs { get; init; }

    /// <summary>Header flags.</summary>
    public RadioEventFlags Flags { get; init; }

    /// <summary>When the frame arrived at the host.</summary>
    public DateTimeOffset ReceivedAt { get; init; }

    /// <summary>The event instant on the host clock, if the library has a clock estimate.</summary>
    public DateTimeOffset? EstimatedTime { get; init; }

    /// <summary>Re-sent by EVENT_REPLAY.</summary>
    public bool IsReplay => (Flags & RadioEventFlags.Replay) != 0;

    /// <summary>Held back while PTT was asserted.</summary>
    public bool IsDeferred => (Flags & RadioEventFlags.Deferred) != 0;

    /// <summary>Not stored and not replayable.</summary>
    public bool IsEphemeral => (Flags & RadioEventFlags.Ephemeral) != 0;
}

/// <summary>A busy (carrier detect) edge. <see cref="RadioEvent.RadioTimeMs"/> is when the firmware saw the edge.</summary>
public sealed record BusyEvent : RadioEvent
{
    /// <inheritdoc/>
    public override RadioEvents Kind => RadioEvents.Busy;

    /// <summary>True: the channel became busy; false: it became clear.</summary>
    public bool Busy { get; init; }

    /// <summary>Which sources are busy now.</summary>
    public BusySources Sources { get; init; }

    /// <summary>What caused the edge.</summary>
    public BusyCause Cause { get; init; }

    /// <summary>RSSI at the edge.</summary>
    public Rssi Rssi { get; init; }

    /// <summary>Noise indicator at the edge.</summary>
    public int Noise { get; init; }

    /// <summary>Glitch indicator at the edge.</summary>
    public int Glitch { get; init; }
}

/// <summary>
/// Signal report for one received burst (one busy interval), sent after the closing busy edge.
/// <see cref="RadioEvent.RadioTimeMs"/> is the close time.
/// </summary>
public sealed record RxBurstEvent : RadioEvent
{
    /// <inheritdoc/>
    public override RadioEvents Kind => RadioEvents.RxBurst;

    /// <summary>Radio time the burst opened, ms.</summary>
    public uint OpenedAtMs { get; init; }

    /// <summary>Burst duration.</summary>
    public TimeSpan Duration { get; init; }

    /// <summary>Samples taken.</summary>
    public int Samples { get; init; }

    /// <summary>Mean RSSI (floor of the mean of the raw values).</summary>
    public Rssi RssiMean { get; init; }

    /// <summary>Highest RSSI.</summary>
    public Rssi RssiMax { get; init; }

    /// <summary>Lowest RSSI.</summary>
    public Rssi RssiMin { get; init; }

    /// <summary>Mean noise indicator.</summary>
    public int NoiseMean { get; init; }

    /// <summary>Lowest noise indicator.</summary>
    public int NoiseMin { get; init; }

    /// <summary>Mean glitch indicator.</summary>
    public int GlitchMean { get; init; }

    /// <summary>Highest glitch indicator.</summary>
    public int GlitchMax { get; init; }

    /// <summary>Mean AF amplitude, REG_64&lt;14:0&gt;, null if not sampled. Meaningful only with <see cref="RadioCapabilities.RxAfAmplitude"/>.</summary>
    public int? AfAmplitudeMean { get; init; }

    /// <summary>Highest AF amplitude, null if not sampled.</summary>
    public int? AfAmplitudeMax { get; init; }

    /// <summary>AGC at open.</summary>
    public AgcReading AgcAtOpen { get; init; }

    /// <summary>AGC at close.</summary>
    public AgcReading AgcAtClose { get; init; }

    /// <summary>Frequency error, Hz, null if not available.</summary>
    public int? FrequencyErrorHz { get; init; }

    /// <summary>What ended the burst.</summary>
    public BusyCause EndCause { get; init; }
}

/// <summary>
/// A transmission started. <see cref="RadioEvent.RadioTimeMs"/> is RF ready (PA bias and bias delay
/// done). Deferred until after the release unless the subscription has LIVE_TX.
/// </summary>
public sealed record TxStartEvent : RadioEvent
{
    /// <inheritdoc/>
    public override RadioEvents Kind => RadioEvents.TxStart;

    /// <summary>Radio time of the first tick counted towards the press, ms.</summary>
    public uint PressedAtMs { get; init; }

    /// <summary>Frequency, Hz.</summary>
    public long FrequencyHz { get; init; }

    /// <summary>Power.</summary>
    public TxPower Power { get; init; }

    /// <summary>Bandwidth.</summary>
    public Bandwidth Bandwidth { get; init; }

    /// <summary>Deviation register used.</summary>
    public Deviation Deviation { get; init; }

    /// <summary>How long the key-up waited for the serial lock (late key).</summary>
    public TimeSpan LockDelay { get; init; }

    /// <summary>The channel was busy at the press: collision risk.</summary>
    public bool BusyAtPress { get; init; }

    /// <summary>The key-up was late because of the serial lock.</summary>
    public bool LateKey { get; init; }

    /// <summary>PTT press to RF ready, from the radio's own clock.</summary>
    public TimeSpan KeyUpLatency => TimeSpan.FromMilliseconds(unchecked(RadioTimeMs - PressedAtMs));
}

/// <summary>A transmission ended. <see cref="RadioEvent.RadioTimeMs"/> is carrier off. Sent once the receiver is set up again.</summary>
public sealed record TxEndEvent : RadioEvent
{
    /// <inheritdoc/>
    public override RadioEvents Kind => RadioEvents.TxEnd;

    /// <summary>Radio time RF was ready, ms.</summary>
    public uint StartedAtMs { get; init; }

    /// <summary>Radio time of the first tick counted towards the release, ms (equals carrier off for other reasons).</summary>
    public uint ReleasedAtMs { get; init; }

    /// <summary>Radio time the receiver set-up completed, ms.</summary>
    public uint RxReadyAtMs { get; init; }

    /// <summary>Why it ended.</summary>
    public TxEndReason Reason { get; init; }

    /// <summary>Mean mic amplitude, REG_64&lt;14:0&gt;, null if none. Meaningful only with <see cref="RadioCapabilities.TxMicAmplitude"/>.</summary>
    public int? MicAmplitudeMean { get; init; }

    /// <summary>Highest mic amplitude, null if none.</summary>
    public int? MicAmplitudeMax { get; init; }

    /// <summary>Mic samples taken (every 10 ms).</summary>
    public int MicSamples { get; init; }

    /// <summary>RF ready to carrier off.</summary>
    public TimeSpan OnAir => TimeSpan.FromMilliseconds(unchecked(RadioTimeMs - StartedAtMs));

    /// <summary>Release to carrier off.</summary>
    public TimeSpan KeyDownLatency => TimeSpan.FromMilliseconds(unchecked(RadioTimeMs - ReleasedAtMs));

    /// <summary>Release to receiver ready: the turnaround the far end must allow for.</summary>
    public TimeSpan TurnaroundToReceive => TimeSpan.FromMilliseconds(unchecked(RxReadyAtMs - ReleasedAtMs));
}

/// <summary>A PTT press was refused and latched until PTT is released. <see cref="RadioEvent.RadioTimeMs"/> is the decision time.</summary>
public sealed record TxRefusedEvent : RadioEvent
{
    /// <inheritdoc/>
    public override RadioEvents Kind => RadioEvents.TxRefused;

    /// <summary>Radio time of the press, ms.</summary>
    public uint PressedAtMs { get; init; }

    /// <summary>Why.</summary>
    public TxRefusedReason Reason { get; init; }

    /// <summary>For <see cref="TxRefusedReason.Lock"/>, the lock remaining at the press; zero otherwise.</summary>
    public TimeSpan LockRemaining { get; init; }

    /// <summary>For <see cref="TxRefusedReason.Late"/>, how long after the press edge the radio got to it; zero otherwise.</summary>
    public TimeSpan LateBy { get; init; }
}

/// <summary>One RSSI stream sample.</summary>
/// <param name="RadioTimeMs">Radio time of the sample, ms.</param>
/// <param name="Rssi">RSSI.</param>
/// <param name="Noise">Noise indicator.</param>
/// <param name="Glitch">Glitch indicator.</param>
public readonly record struct RssiSample(uint RadioTimeMs, Rssi Rssi, int Noise, int Glitch);

/// <summary>A batch of RSSI samples (ephemeral). <see cref="RadioEvent.RadioTimeMs"/> is the last sample's time.</summary>
public sealed record RssiStreamEvent : RadioEvent
{
    /// <inheritdoc/>
    public override RadioEvents Kind => RadioEvents.RssiStream;

    /// <summary>Sample period.</summary>
    public TimeSpan Period { get; init; }

    /// <summary>The samples, oldest first.</summary>
    public IReadOnlyList<RssiSample> Samples { get; init; } = [];
}

/// <summary>A periodic heartbeat (ephemeral). <see cref="RadioEvent.RadioTimeMs"/> plus <see cref="Microseconds"/> is when it was queued.</summary>
public sealed record HeartbeatEvent : RadioEvent
{
    /// <inheritdoc/>
    public override RadioEvents Kind => RadioEvents.Heartbeat;

    /// <summary>Sub-millisecond part of the timestamp, 0 to 999 us.</summary>
    public int Microseconds { get; init; }

    /// <summary>Status flags.</summary>
    public StatusFlags Status { get; init; }

    /// <summary>State.</summary>
    public RadioState State { get; init; }

    /// <summary>RSSI.</summary>
    public Rssi Rssi { get; init; }

    /// <summary>Battery, mV.</summary>
    public int BatteryMillivolts { get; init; }

    /// <summary>Busy time since the previous heartbeat (channel occupancy), null if the radio reports 0xFFFF (unknown).</summary>
    public TimeSpan? BusyTime { get; init; }

    /// <summary>Serial lock remaining.</summary>
    public TimeSpan LockRemaining { get; init; }
}

/// <summary>The battery changed class.</summary>
public sealed record BatteryEvent : RadioEvent
{
    /// <inheritdoc/>
    public override RadioEvents Kind => RadioEvents.Battery;

    /// <summary>Class.</summary>
    public BatteryClass Class { get; init; }

    /// <summary>Level 0 to 7.</summary>
    public int Level { get; init; }

    /// <summary>Voltage, mV.</summary>
    public int Millivolts { get; init; }
}

/// <summary>Parameters changed.</summary>
public sealed record ParamsChangedEvent : RadioEvent
{
    /// <inheritdoc/>
    public override RadioEvents Kind => RadioEvents.ParamsChanged;

    /// <summary>Who changed them.</summary>
    public ParamsChangeSource Source { get; init; }

    /// <summary>Which parameters.</summary>
    public IReadOnlyList<RadioParameterId> Parameters { get; init; } = [];
}

/// <summary>Stored events were overwritten before they could be sent.</summary>
public sealed record EventsLostEvent : RadioEvent
{
    /// <inheritdoc/>
    public override RadioEvents Kind => RadioEvents.EventsLost;

    /// <summary>First lost seq.</summary>
    public ushort FirstLost { get; init; }

    /// <summary>How many.</summary>
    public int Count { get; init; }
}

/// <summary>The level tone ended.</summary>
public sealed record ToneEndEvent : RadioEvent
{
    /// <inheritdoc/>
    public override RadioEvents Kind => RadioEvents.ToneEnd;

    /// <summary>Why.</summary>
    public ToneEndReason Reason { get; init; }
}

/// <summary>Trial register overrides expired and were reverted.</summary>
public sealed record OverrideExpiredEvent : RadioEvent
{
    /// <inheritdoc/>
    public override RadioEvents Kind => RadioEvents.OverrideExpired;

    /// <summary>True if the key-up bound expired, false if the time bound did.</summary>
    public bool ByKeyUps { get; init; }

    /// <summary>Entries reverted.</summary>
    public int EntriesReverted { get; init; }
}

/// <summary>The radio booted (sent only with a persisted subscription).</summary>
public sealed record BootEvent : RadioEvent
{
    /// <inheritdoc/>
    public override RadioEvents Kind => RadioEvents.Boot;

    /// <summary>Protocol version.</summary>
    public Version ProtocolVersion { get; init; } = new(2, 0);

    /// <summary>Reset cause: 0 unknown, 1 power-on, 2 software reboot.</summary>
    public int ResetCause { get; init; }
}

/// <summary>An event id this library does not know (a newer firmware).</summary>
public sealed record UnknownEvent : RadioEvent
{
    /// <inheritdoc/>
    public override RadioEvents Kind => RadioEvents.None;

    /// <summary>The message id.</summary>
    public ushort Id { get; init; }

    /// <summary>The body after the 7-byte event header.</summary>
    public ReadOnlyMemory<byte> Body { get; init; }
}

/// <summary>Parses event frames.</summary>
internal static class EventParser
{
    public const int HeaderLength = 7;

    public static RadioEvent Parse(ushort id, ReadOnlySpan<byte> body, DeviationLaw law, DateTimeOffset receivedAt)
    {
        var r = new WireReader(body, $"event 0x{id:X4}");
        ushort seq = r.U16();
        uint t = r.U32();
        var flags = (RadioEventFlags)r.U8();
        RadioEvent e = (id - MessageIds.EventFirst) switch
        {
            0 => new BusyEvent
            {
                Busy = r.U8() != 0,
                Sources = (BusySources)r.U8(),
                Cause = (BusyCause)r.U8(),
                Rssi = new Rssi(r.U16()),
                Noise = r.U8(),
                Glitch = r.U8(),
            },
            1 => ParseBurst(ref r),
            2 => ParseTxStart(ref r, law),
            3 => new TxEndEvent
            {
                StartedAtMs = r.U32(),
                ReleasedAtMs = r.U32(),
                RxReadyAtMs = r.U32(),
                Reason = (TxEndReason)r.U8(),
                MicAmplitudeMean = OrNull(r.U16()),
                MicAmplitudeMax = OrNull(r.U16()),
                MicSamples = r.U16(),
            },
            4 => ParseRefused(ref r),
            5 => ParseStream(ref r, t),
            6 => new HeartbeatEvent
            {
                Microseconds = r.U16(),
                Status = (StatusFlags)r.U8(),
                State = (RadioState)r.U8(),
                Rssi = new Rssi(r.U16()),
                BatteryMillivolts = r.U16(),
                BusyTime = OrNull(r.U16()) is { } busyMs ? TimeSpan.FromMilliseconds(busyMs) : null,
                LockRemaining = TimeSpan.FromMilliseconds(r.U16()),
            },
            7 => new BatteryEvent { Class = (BatteryClass)r.U8(), Level = r.U8(), Millivolts = r.U16() },
            8 => new ParamsChangedEvent { Source = (ParamsChangeSource)r.U8(), Parameters = MaskToIds(r.U32()) },
            9 => new EventsLostEvent { FirstLost = r.U16(), Count = r.U16() },
            10 => new ToneEndEvent { Reason = (ToneEndReason)r.U8() },
            11 => new OverrideExpiredEvent { ByKeyUps = r.U8() == 1, EntriesReverted = r.U8() },
            12 => ParseBoot(ref r),
            _ => new UnknownEvent { Id = id, Body = r.Bytes(r.Remaining).ToArray() },
        };

        return e with { Sequence = seq, RadioTimeMs = t, Flags = flags, ReceivedAt = receivedAt };
    }

    public static IReadOnlyList<RadioParameterId> MaskToIds(uint mask)
    {
        var ids = new List<RadioParameterId>();
        for (int i = 0; i < 32; i++)
        {
            if ((mask & (1u << i)) != 0)
            {
                ids.Add((RadioParameterId)i);
            }
        }

        return ids;
    }

    private static int? OrNull(ushort v) => v == 0xFFFF ? null : v;

    private static RxBurstEvent ParseBurst(ref WireReader r)
    {
        var e = new RxBurstEvent
        {
            OpenedAtMs = r.U32(),
            Duration = TimeSpan.FromMilliseconds(r.U32()),
            Samples = r.U16(),
            RssiMean = new Rssi(r.U16()),
            RssiMax = new Rssi(r.U16()),
            RssiMin = new Rssi(r.U16()),
            NoiseMean = r.U8(),
            NoiseMin = r.U8(),
            GlitchMean = r.U8(),
            GlitchMax = r.U8(),
            AfAmplitudeMean = OrNull(r.U16()),
            AfAmplitudeMax = OrNull(r.U16()),
            AgcAtOpen = new AgcReading(r.U8()),
            AgcAtClose = new AgcReading(r.U8()),
        };
        short fe = r.S16();
        return e with { FrequencyErrorHz = fe == 0x7FFF ? null : fe, EndCause = (BusyCause)r.U8() };
    }

    private static TxRefusedEvent ParseRefused(ref WireReader r)
    {
        uint press = r.U32();
        var reason = (TxRefusedReason)r.U8();
        var detail = TimeSpan.FromMilliseconds(r.U16());
        return new TxRefusedEvent
        {
            PressedAtMs = press,
            Reason = reason,
            LockRemaining = reason == TxRefusedReason.Lock ? detail : TimeSpan.Zero,
            LateBy = reason == TxRefusedReason.Late ? detail : TimeSpan.Zero,
        };
    }

    private static TxStartEvent ParseTxStart(ref WireReader r, DeviationLaw law)
    {
        uint press = r.U32();
        uint freq = r.U32();
        var power = (TxPower)r.U8();
        var bw = (Bandwidth)r.U8();
        ushort dev = r.U16();
        ushort delay = r.U16();
        byte f = r.U8();
        return new TxStartEvent
        {
            PressedAtMs = press,
            FrequencyHz = freq,
            Power = power,
            Bandwidth = bw,
            Deviation = new Deviation((ushort)Math.Min(dev, Deviation.MaxRegister), law),
            LockDelay = TimeSpan.FromMilliseconds(delay),
            BusyAtPress = (f & 1) != 0,
            LateKey = (f & 2) != 0,
        };
    }

    private static RssiStreamEvent ParseStream(ref WireReader r, uint t)
    {
        int period = r.U8();
        int count = r.U8();
        var samples = new RssiSample[count];
        for (int i = 0; i < count; i++)
        {
            uint at = unchecked(t - (uint)((count - 1 - i) * period));
            samples[i] = new RssiSample(at, new Rssi(r.U16()), r.U8(), r.U8());
        }

        return new RssiStreamEvent { Period = TimeSpan.FromMilliseconds(period), Samples = samples };
    }

    private static BootEvent ParseBoot(ref WireReader r)
    {
        ushort v = r.U16();
        return new BootEvent { ProtocolVersion = new Version(v >> 8, v & 0xFF), ResetCause = r.U8() };
    }
}
