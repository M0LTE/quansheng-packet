using M0LTE.Uvk5.Protocol;

namespace M0LTE.Uvk5;

/// <summary>Parameter ids of protocol v2 (GET_PARAMS, SET_PARAMS).</summary>
public enum RadioParameterId : byte
{
    /// <summary>Frequency, Hz (u32).</summary>
    FrequencyHz = 0x01,

    /// <summary>TX power (u8).</summary>
    Power = 0x02,

    /// <summary>Bandwidth (u8).</summary>
    Bandwidth = 0x03,

    /// <summary>Wide deviation register (u16).</summary>
    DeviationWide = 0x04,

    /// <summary>Narrow deviation register (u16).</summary>
    DeviationNarrow = 0x05,

    /// <summary>RX AF gain 2, REG_48&lt;9:4&gt; (u8).</summary>
    RxGain = 0x08,

    /// <summary>RX DAC gain, REG_48&lt;3:0&gt; (u8).</summary>
    RxDacGain = 0x09,

    /// <summary>TX timeout, s (u8).</summary>
    TxTimeoutSeconds = 0x0A,

    /// <summary>PTT press debounce, ms (u8).</summary>
    PttPressMs = 0x0B,

    /// <summary>PTT release debounce, ms (u8).</summary>
    PttReleaseMs = 0x0C,

    /// <summary>Delay after PA enable, ms (u8).</summary>
    PaEnableDelayMs = 0x0D,

    /// <summary>Delay after PA bias, ms (u8).</summary>
    PaBiasDelayMs = 0x0E,

    /// <summary>Serial PTT lock, ms (u16).</summary>
    SerialLockMs = 0x0F,

    /// <summary>Busy sources (u8).</summary>
    BusySource = 0x10,

    /// <summary>RSSI busy open threshold (u16 raw).</summary>
    BusyRssiOpen = 0x11,

    /// <summary>RSSI busy close threshold (u16 raw).</summary>
    BusyRssiClose = 0x12,

    /// <summary>RSSI busy hang, ms (u8).</summary>
    BusyHangMs = 0x13,

    /// <summary>Thresholds of the chip's squelch detector used for busy (6 bytes, RAM only; was SQL_RAW).</summary>
    BusySquelchRaw = 0x14,

    /// <summary>AGC fix (u8, RAM only).</summary>
    AgcFix = 0x15,

    /// <summary>AFC on or off (u8, RAM only).</summary>
    Afc = 0x16,

    /// <summary>Backlight 0 to 7 (u8).</summary>
    Backlight = 0x17,

    /// <summary>Key lock (u8).</summary>
    KeyLock = 0x18,

    /// <summary>Busy detector level 1 to 9 (u8): the factory squelch-table row the chip's squelch detector uses. Not a squelch: receive audio is always open.</summary>
    BusySquelchLevel = 0x19,
}

/// <summary>
/// Radio settings. Every property is optional: when reading, null means "not reported" (not
/// supported by this firmware or not asked for); when writing, only non-null properties are sent,
/// all together, so a change is atomic on v2.
/// </summary>
/// <remarks>
/// There is no squelch setting: the packet firmware has no squelch and its receive audio is always
/// open. Parameter 0x07 (SQUELCH) is retired, and the radio answers UNSUPPORTED if asked for it.
/// The chip's squelch result survives only as a carrier detector for busy, set by
/// <see cref="BusySquelchLevel"/> and <see cref="BusySquelchThresholds"/>.
/// <para>There is no mic gain setting either: the firmware fixes it at the maximum (31), because the
/// whole range moved the deviation by only about 0.5 dB. Parameter 0x06 (MIC_GAIN) is retired the
/// same way; the deviation settings set the transmit level.</para>
/// <para>Values are validated against the protocol's ranges when a change is sent, before
/// anything goes to the radio.</para>
/// </remarks>
public sealed record RadioSettings
{
    /// <summary>Frequency, Hz: 50 to 600 MHz, a multiple of 10 Hz. Moves the radio out of memory-channel mode to the band slot for that frequency.</summary>
    public long? FrequencyHz { get; init; }

    /// <summary>TX power.</summary>
    public TxPower? Power { get; init; }

    /// <summary>Bandwidth.</summary>
    public Bandwidth? Bandwidth { get; init; }

    /// <summary>Deviation used on wide channels (default 0x856).</summary>
    public Deviation? DeviationWide { get; init; }

    /// <summary>Deviation used on narrow channels (default 0x756).</summary>
    public Deviation? DeviationNarrow { get; init; }

    /// <summary>RX AF gain 2, REG_48&lt;9:4&gt;, 0 to 63 in 0.5 dB steps.</summary>
    public int? RxGain { get; init; }

    /// <summary>RX DAC gain, REG_48&lt;3:0&gt;, 0 to 15 in about 2 dB steps.</summary>
    public int? RxDacGain { get; init; }

    /// <summary>TX timeout: 5, 10, 15, 20, 30, 60 or 120 s.</summary>
    public TimeSpan? TxTimeout { get; init; }

    /// <summary>PTT press debounce, 1 to 40 ms.</summary>
    public TimeSpan? PttPressDebounce { get; init; }

    /// <summary>PTT release debounce, 2 to 40 ms.</summary>
    public TimeSpan? PttReleaseDebounce { get; init; }

    /// <summary>Delay after PA enable before PA bias, 1 to 20 ms.</summary>
    public TimeSpan? PaEnableDelay { get; init; }

    /// <summary>Delay after PA bias, 0 to 20 ms.</summary>
    public TimeSpan? PaBiasDelay { get; init; }

    /// <summary>Serial PTT lock after each host frame, 0 to 1500 ms in 10 ms steps (v2 default 20 ms).</summary>
    public TimeSpan? SerialLock { get; init; }

    /// <summary>What the firmware's busy state is built from.</summary>
    public BusySources? BusySource { get; init; }

    /// <summary>RSSI busy opens at or above this level.</summary>
    public Rssi? BusyRssiOpen { get; init; }

    /// <summary>RSSI busy closes after <see cref="BusyHang"/> continuously below this level (at most <see cref="BusyRssiOpen"/>).</summary>
    public Rssi? BusyRssiClose { get; init; }

    /// <summary>RSSI busy hang time, 0 to 250 ms.</summary>
    public TimeSpan? BusyHang { get; init; }

    /// <summary>
    /// Busy detector level, 1 to 9 (default 1): which row of the factory squelch tables the chip's
    /// squelch detector uses for the busy state. It never mutes anything. Setting it drops any
    /// <see cref="BusySquelchThresholds"/>. On packet firmware v1, where the same EEPROM byte
    /// (0x1D01) is a real squelch, the value read is that squelch level, 0 to 9.
    /// </summary>
    public int? BusySquelchLevel { get; init; }

    /// <summary>Raw thresholds for the chip's squelch detector (busy only, RAM only).</summary>
    public BusySquelchThresholds? BusySquelchThresholds { get; init; }

    /// <summary>AGC mode (RAM only, diagnostic).</summary>
    public AgcSetting? Agc { get; init; }

    /// <summary>AFC on or off (RAM only, diagnostic).</summary>
    public bool? Afc { get; init; }

    /// <summary>Backlight, 0 (off) to 7 (on).</summary>
    public int? Backlight { get; init; }

    /// <summary>Keypad lock.</summary>
    public bool? KeyLock { get; init; }

    /// <summary>The ids of the properties that are set, in id order.</summary>
    public IReadOnlyList<RadioParameterId> SetIds => ParameterCodec.IdsPresent(this);

    /// <summary>
    /// A copy with every property of <paramref name="other"/> that is set replacing this one's.
    /// </summary>
    public RadioSettings Merge(RadioSettings other) => ParameterCodec.Merge(this, other);
}

/// <summary>Flags for <c>SetSettingsAsync</c>.</summary>
[Flags]
public enum SetSettingsFlags
{
    /// <summary>RAM only: the change is lost at power-off. The right default for a TNC that sets up the radio at start-up.</summary>
    None = 0,

    /// <summary>
    /// Also write the changed values to EEPROM, after the reply, one 8-byte block per main-loop
    /// pass, never during a transmission. Costs about 8 ms per block and can delay a key-up by up
    /// to 10 ms: persist only when idle.
    /// </summary>
    Persist = 1,

    /// <summary>Reject the change if the resulting frequency is not TX-allowed by the band plan.</summary>
    RequireTxAllowed = 2,

    /// <summary>Validate only; nothing changes.</summary>
    DryRun = 4,
}

/// <summary>Result of a settings change.</summary>
public sealed record SetSettingsResult
{
    /// <summary>The values the radio reports it applied (read back), for every setting sent.</summary>
    public required RadioSettings Applied { get; init; }

    /// <summary>TX is allowed at the resulting frequency (v2), null if unknown.</summary>
    public bool? TxAllowed { get; init; }

    /// <summary>A persist to EEPROM was queued.</summary>
    public bool PersistQueued { get; init; }

    /// <summary>The receiver was retuned (an open squelch closed).</summary>
    public bool Retuned { get; init; }

    /// <summary>
    /// On packet firmware v1 the new values are written to the EEPROM settings block and the
    /// firmware applies them about 1.0 to 1.5 s after the serial session goes quiet. Zero on v2,
    /// where changes are live at once.
    /// </summary>
    public TimeSpan AppliesAfterQuiet { get; init; }
}
