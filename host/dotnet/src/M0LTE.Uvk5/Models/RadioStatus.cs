namespace M0LTE.Uvk5;

/// <summary>Radio operating state.</summary>
public enum RadioState : byte
{
    /// <summary>Idle, squelch closed.</summary>
    Idle = 0,

    /// <summary>Receiving (squelch open).</summary>
    Receiving = 1,

    /// <summary>Transmitting.</summary>
    Transmitting = 2,

    /// <summary>Monitor (squelch forced open).</summary>
    Monitor = 3,

    /// <summary>Reduced service (critical battery): PTT is ignored.</summary>
    ReducedService = 4,
}

/// <summary>Status flags, first byte (GET_STATUS flags1; also in heartbeats).</summary>
[Flags]
public enum StatusFlags : byte
{
    /// <summary>None.</summary>
    None = 0,

    /// <summary>Squelch open.</summary>
    SquelchOpen = 1 << 0,

    /// <summary>Busy (the firmware's carrier detect).</summary>
    Busy = 1 << 1,

    /// <summary>PTT pressed.</summary>
    PttPressed = 1 << 2,

    /// <summary>Serial PTT lock active.</summary>
    LockActive = 1 << 3,

    /// <summary>TX allowed at this frequency.</summary>
    TxAllowed = 1 << 4,

    /// <summary>TX latched: PTT must be released before the next transmission.</summary>
    TxLatched = 1 << 5,

    /// <summary>Level tone running.</summary>
    LevelToneRunning = 1 << 6,

    /// <summary>Late key pending.</summary>
    LateKeyPending = 1 << 7,
}

/// <summary>Status flags, second byte (GET_STATUS flags2).</summary>
[Flags]
public enum StatusFlags2 : byte
{
    /// <summary>None.</summary>
    None = 0,

    /// <summary>Live parameters differ from the stored ones.</summary>
    UnsavedChanges = 1 << 0,

    /// <summary>RAM register overrides active.</summary>
    RamOverridesActive = 1 << 1,

    /// <summary>EEPROM register overrides active.</summary>
    EepromOverridesActive = 1 << 2,

    /// <summary>LIVE_TX subscription option on.</summary>
    LiveTx = 1 << 3,

    /// <summary>A persist to EEPROM is in progress.</summary>
    PersistInProgress = 1 << 4,

    /// <summary>Memory-channel mode (else a band slot).</summary>
    MemoryChannelMode = 1 << 5,
}

/// <summary>A v2 GET_STATUS snapshot.</summary>
public sealed record RadioStatus
{
    /// <summary>Radio uptime (the event clock).</summary>
    public TimeSpan Uptime { get; init; }

    /// <summary>Frequency, Hz.</summary>
    public long FrequencyHz { get; init; }

    /// <summary>Operating state.</summary>
    public RadioState State { get; init; }

    /// <summary>Flags, first byte.</summary>
    public StatusFlags Flags { get; init; }

    /// <summary>Flags, second byte.</summary>
    public StatusFlags2 Flags2 { get; init; }

    /// <summary>TX power.</summary>
    public TxPower Power { get; init; }

    /// <summary>Bandwidth.</summary>
    public Bandwidth Bandwidth { get; init; }

    /// <summary>Squelch level, 0 to 9.</summary>
    public int Squelch { get; init; }

    /// <summary>Deviation register in use for the current bandwidth.</summary>
    public Deviation Deviation { get; init; }

    /// <summary>RSSI now.</summary>
    public Rssi Rssi { get; init; }

    /// <summary>Noise indicator, REG_65&lt;6:0&gt;.</summary>
    public int Noise { get; init; }

    /// <summary>Glitch indicator, REG_63&lt;7:0&gt;.</summary>
    public int Glitch { get; init; }

    /// <summary>AGC readback.</summary>
    public AgcReading Agc { get; init; }

    /// <summary>Battery level: 0 empty (TX refused), 1 to 6, 7 over-voltage.</summary>
    public int BatteryLevel { get; init; }

    /// <summary>Battery voltage, mV (10 mV resolution).</summary>
    public int BatteryMillivolts { get; init; }

    /// <summary>Serial PTT lock remaining when the status was taken.</summary>
    public TimeSpan LockRemaining { get; init; }

    /// <summary>Time until the TX timeout, null when not transmitting.</summary>
    public TimeSpan? TxTimeLeft { get; init; }

    /// <summary>Time since the last busy edge; <see cref="TimeSpan.MaxValue"/> when the counter saturated (over 65.5 s).</summary>
    public TimeSpan BusyAge { get; init; }

    /// <summary>The seq the next stored event will get.</summary>
    public ushort NextEventSequence { get; init; }

    /// <summary>Always 0xFF on the current packet firmware, which has one operating channel and no memory channels or band slots.</summary>
    public int Channel { get; init; }

    /// <summary>TX timeout.</summary>
    public TimeSpan TxTimeout { get; init; }

    /// <summary>The firmware's busy state (carrier detect).</summary>
    public bool IsBusy => (Flags & StatusFlags.Busy) != 0;

    /// <summary>TX allowed at this frequency.</summary>
    public bool IsTxAllowed => (Flags & StatusFlags.TxAllowed) != 0;

    /// <summary>Battery voltage, volts.</summary>
    public double BatteryVolts => BatteryMillivolts / 1000.0;
}

/// <summary>A legacy RSSI reading (0x0527): RSSI, noise and glitch indicators.</summary>
/// <param name="Rssi">RSSI.</param>
/// <param name="Noise">Noise indicator, REG_65&lt;6:0&gt;.</param>
/// <param name="Glitch">Glitch indicator, REG_63&lt;7:0&gt;.</param>
public readonly record struct RssiReading(Rssi Rssi, int Noise, int Glitch);

/// <summary>A battery reading.</summary>
public sealed record BatteryReading
{
    /// <summary>Battery voltage, volts, null if it cannot be worked out (legacy firmware with no battery calibration).</summary>
    public double? Volts { get; init; }

    /// <summary>The raw ADC value (legacy 0x0529), null on v2 where the firmware reports mV.</summary>
    public int? RawAdc { get; init; }

    /// <summary>Battery level 0 (empty, TX refused) to 6, 7 over-voltage; v2 only.</summary>
    public int? Level { get; init; }
}

/// <summary>Firmware counters (GET_COUNTERS). Later firmware may append counters; see <see cref="Raw"/>.</summary>
public sealed record RadioCounters
{
    /// <summary>Every counter, in wire order.</summary>
    public required IReadOnlyList<uint> Raw { get; init; }

    private uint At(int i) => i < Raw.Count ? Raw[i] : 0;

    /// <summary>Frames accepted.</summary>
    public uint FramesAccepted => At(0);

    /// <summary>Frames with a bad CRC or footer.</summary>
    public uint FramesBad => At(1);

    /// <summary>Frames dropped (oversize, ring overrun).</summary>
    public uint FramesDropped => At(2);

    /// <summary>Non-OK replies.</summary>
    public uint ErrorReplies => At(3);

    /// <summary>Events stored.</summary>
    public uint EventsStored => At(4);

    /// <summary>Events lost (overwritten before sending).</summary>
    public uint EventsLost => At(5);

    /// <summary>Events deferred while PTT was asserted.</summary>
    public uint EventsDeferred => At(6);

    /// <summary>Transmissions.</summary>
    public uint Transmissions => At(7);

    /// <summary>TX timeouts.</summary>
    public uint TxTimeouts => At(8);

    /// <summary>TX refused.</summary>
    public uint TxRefused => At(9);

    /// <summary>Busy opens.</summary>
    public uint BusyOpens => At(10);

    /// <summary>Late keys.</summary>
    public uint LateKeys => At(11);

    /// <summary>Ephemeral frames dropped (output queue full).</summary>
    public uint EphemeralDropped => At(12);

    /// <summary>EEPROM blocks written.</summary>
    public uint EepromBlocksWritten => At(13);
}

/// <summary>Which phases a register override applies in.</summary>
[Flags]
public enum OverridePhase : byte
{
    /// <summary>None.</summary>
    None = 0,

    /// <summary>After the TX set-up (every key-up).</summary>
    Tx = 1,

    /// <summary>After the RX set-up (every return to receive and every squelch open).</summary>
    Rx = 2,
}

/// <summary>One register override entry: <c>register = (value &amp; AndMask) | OrValue</c>, written after the firmware's own writes for the phase.</summary>
/// <param name="Phase">When it applies.</param>
/// <param name="Register">BK4819 register.</param>
/// <param name="AndMask">AND mask.</param>
/// <param name="OrValue">OR value.</param>
public readonly record struct RegisterOverride(OverridePhase Phase, byte Register, ushort AndMask, ushort OrValue);

/// <summary>The override tables (REG_OVERRIDE reply).</summary>
public sealed record OverrideTables
{
    /// <summary>RAM (trial) entries.</summary>
    public required IReadOnlyList<RegisterOverride> Ram { get; init; }

    /// <summary>EEPROM entries (0x1D10 to 0x1D4F).</summary>
    public required IReadOnlyList<RegisterOverride> Eeprom { get; init; }

    /// <summary>Time until the RAM table expires, null for no time bound.</summary>
    public TimeSpan? ExpiresIn { get; init; }

    /// <summary>Key-ups until the RAM table expires, null for no key-up bound.</summary>
    public int? KeyUpsLeft { get; init; }
}

/// <summary>Result of starting a level tone.</summary>
/// <param name="GainCode">REG_70&lt;14:8&gt; gain code used.</param>
/// <param name="FrequencyWord">REG_71 frequency word used.</param>
public readonly record struct LevelToneResult(int GainCode, int FrequencyWord);

/// <summary>Result of an EVENT_REPLAY.</summary>
/// <param name="FirstSent">Seq of the first event re-sent.</param>
/// <param name="CountSent">Events re-sent.</param>
/// <param name="OldestAvailable">Oldest seq still in the ring.</param>
/// <param name="NextSequence">Seq the next stored event will get.</param>
public readonly record struct ReplayResult(ushort FirstSent, int CountSent, ushort OldestAvailable, ushort NextSequence);
