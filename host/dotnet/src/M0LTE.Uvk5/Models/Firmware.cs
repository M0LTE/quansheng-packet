namespace M0LTE.Uvk5;

/// <summary>Which kind of firmware the radio runs, as identified from its hello reply.</summary>
public enum FirmwareKind
{
    /// <summary>Not identified yet (no hello reply seen).</summary>
    Unknown,

    /// <summary>Stock Quansheng, egzumer or another firmware: legacy commands only.</summary>
    Stock,

    /// <summary>Packet firmware v1 (version starts <c>PKTFW</c>, no PKT2 marker): legacy commands plus the EEPROM settings block.</summary>
    PacketV1,

    /// <summary>Packet firmware speaking protocol v2 (PKT2 marker in the hello reply).</summary>
    PacketV2,
}

/// <summary>Result status of a protocol v2 command (reply header byte 1).</summary>
public enum K5Status : byte
{
    /// <summary>Done.</summary>
    Ok = 0x00,

    /// <summary>Undefined request id.</summary>
    UnknownCommand = 0x01,

    /// <summary>Body too short or too long, or body length mismatch.</summary>
    BadLength = 0x02,

    /// <summary>Unknown or repeated parameter id.</summary>
    BadParameter = 0x03,

    /// <summary>Value out of range.</summary>
    Range = 0x04,

    /// <summary>Frequency not TX-allowed and REQUIRE_TX_OK was set.</summary>
    TxBand = 0x05,

    /// <summary>Not possible in the current state (reduced service).</summary>
    State = 0x06,

    /// <summary>Register not allowed.</summary>
    Refused = 0x07,

    /// <summary>Capability absent or uncalibrated.</summary>
    Unsupported = 0x08,

    /// <summary>Settings block not valid, cannot persist.</summary>
    Eeprom = 0x09,

    /// <summary>RAM-only parameter with PERSIST.</summary>
    NotPersistable = 0x0A,
}

/// <summary>Capability bits a v2 radio reports in GET_INFO.</summary>
[Flags]
public enum RadioCapabilities : uint
{
    /// <summary>None.</summary>
    None = 0,

    /// <summary>The LIVE_TX subscription option (events during transmissions).</summary>
    LiveTx = 1 << 0,

    /// <summary>The RSSI busy detector.</summary>
    RssiBusyDetector = 1 << 1,

    /// <summary>LEVEL_TONE with a raw gain code.</summary>
    LevelToneRaw = 1 << 2,

    /// <summary>LEVEL_TONE in deviation-equivalent mode (needs the tone calibration byte).</summary>
    LevelToneCalibrated = 1 << 3,

    /// <summary>RX AF amplitude in burst reports is validated.</summary>
    RxAfAmplitude = 1 << 4,

    /// <summary>AGC readback is validated.</summary>
    AgcReadback = 1 << 5,

    /// <summary>Frequency error in burst reports is available.</summary>
    FrequencyError = 1 << 6,

    /// <summary>TX mic amplitude in TX_END is validated.</summary>
    TxMicAmplitude = 1 << 7,

    /// <summary>RAM register overrides (REG_OVERRIDE).</summary>
    RamRegisterOverrides = 1 << 8,

    /// <summary>Persisting parameters to EEPROM.</summary>
    Persistence = 1 << 9,

    /// <summary>TIME_SYNC timestamps are exact.</summary>
    ExactTimeSync = 1 << 10,
}

/// <summary>What this library can do with the connected radio, derived from its firmware kind and capabilities.</summary>
[Flags]
public enum K5Features
{
    /// <summary>Nothing (not connected).</summary>
    None = 0,

    /// <summary>Legacy EEPROM read and (with a verified backup) write.</summary>
    Eeprom = 1 << 0,

    /// <summary>Legacy RSSI, noise and glitch read (0x0527).</summary>
    LegacyRssi = 1 << 1,

    /// <summary>Battery reading.</summary>
    Battery = 1 << 2,

    /// <summary>BK4819 register read and write (packet firmware always has them; stock builds may not).</summary>
    Registers = 1 << 3,

    /// <summary>Reading and changing settings: v2 parameters, or on packet firmware v1 the EEPROM settings block (applied about 1.5 s after the session goes quiet).</summary>
    Settings = 1 << 4,

    /// <summary>Live, atomic settings changes with read-back (v2 SET_PARAMS).</summary>
    LiveSettings = 1 << 5,

    /// <summary>GET_STATUS.</summary>
    Status = 1 << 6,

    /// <summary>Unsolicited events: busy edges, burst reports, TX timing, heartbeats.</summary>
    Events = 1 << 7,

    /// <summary>TIME_SYNC.</summary>
    TimeSync = 1 << 8,

    /// <summary>LEVEL_TONE.</summary>
    LevelTone = 1 << 9,

    /// <summary>REG_OVERRIDE trial overrides.</summary>
    RegisterOverrides = 1 << 10,

    /// <summary>GET_COUNTERS.</summary>
    Counters = 1 << 11,

    /// <summary>Exact PTT-lock knowledge: every reply carries the lock remaining.</summary>
    ExactPttLock = 1 << 12,
}

/// <summary>What the radio said about itself: the hello reply and, on v2, GET_INFO.</summary>
public sealed record FirmwareInfo
{
    /// <summary>The firmware kind.</summary>
    public required FirmwareKind Kind { get; init; }

    /// <summary>The version string from the hello reply, for example <c>PKTFW 2951c48</c>.</summary>
    public required string Version { get; init; }

    /// <summary>Protocol version from the PKT2 marker (2.0 for v2), or null before v2.</summary>
    public Version? ProtocolVersion { get; init; }

    /// <summary>The radio reports its lock screen (stock firmware with a power-on password).</summary>
    public bool InLockScreen { get; init; }

    /// <summary>The radio reports a custom AES key (stock firmware only).</summary>
    public bool HasCustomAesKey { get; init; }

    /// <summary>Raw v2 capability bits, <see cref="RadioCapabilities.None"/> before v2.</summary>
    public RadioCapabilities Capabilities { get; init; }

    /// <summary>What the library can do with this radio.</summary>
    public K5Features Features { get; init; }

    /// <summary>Parameters the radio supports (v2 GET_INFO), or the settings-block subset on v1.</summary>
    public IReadOnlySet<RadioParameterId> SupportedParameters { get; init; } = new HashSet<RadioParameterId>();

    /// <summary>Events the radio can send (v2), <see cref="RadioEvents.None"/> before v2.</summary>
    public RadioEvents SupportedEvents { get; init; }

    /// <summary>Largest v2 request body, bytes.</summary>
    public int MaxRequestBody { get; init; }

    /// <summary>Stored-event ring capacity, events.</summary>
    public int EventRingCapacity { get; init; }

    /// <summary>
    /// The PTT lock after each host frame: SERIAL_LOCK_MS on v2, 1.0 to 1.5 s on v1 (reported as
    /// 1.5 s), about 6 s after a hello or EEPROM command on stock (reported as 6.5 s).
    /// </summary>
    public TimeSpan SerialLock { get; init; }

    /// <summary>Longest late key the radio does instead of refusing a press during the lock (v2), else zero.</summary>
    public TimeSpan LateKeyMax { get; init; }

    /// <summary>TX band plan (F_LOCK, 0 to 7) on v2.</summary>
    public int TxBandPlan { get; init; }

    /// <summary>TX band flags on v2: bit 0 200TX, bit 1 350TX, bit 2 500TX, bit 3 350EN.</summary>
    public int TxBandFlags { get; init; }

    /// <summary>Settings block layout at 0x1D00 (1), 0 if the block is not valid. Known on v2; on v1 after a settings read.</summary>
    public int SettingsBlockLayout { get; init; }

    /// <summary>v2 EEPROM block layout at 0x1D60 (1), 0 if not valid.</summary>
    public int V2BlockLayout { get; init; }

    /// <summary>Default burst sample period on v2.</summary>
    public TimeSpan DefaultBurstSamplePeriod { get; init; }

    /// <summary>True if the library can do <paramref name="feature"/> with this radio.</summary>
    public bool Supports(K5Features feature) => (Features & feature) == feature;

    /// <summary>True if the radio supports parameter <paramref name="id"/>.</summary>
    public bool Supports(RadioParameterId id) => SupportedParameters.Contains(id);

    /// <inheritdoc/>
    public override string ToString() => Kind switch
    {
        FirmwareKind.PacketV2 => $"{Version} (protocol v{ProtocolVersion})",
        FirmwareKind.PacketV1 => $"{Version} (packet firmware v1)",
        FirmwareKind.Stock => $"{Version} (legacy commands only)",
        _ => Version,
    };
}
