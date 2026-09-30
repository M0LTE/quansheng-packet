using System.Globalization;

namespace M0LTE.Uvk5;

/// <summary>
/// A BK4819 RSSI reading: REG_67&lt;8:0&gt;, 0.5 dB steps, dBm = raw / 2 - 160 as the chip reports it.
/// The radio's screen adds a per-band correction on top (<see cref="RssiScale"/>); use
/// <see cref="DbmAt"/> to show the level the way the radio does. Thresholds (BUSY_RSSI_OPEN and
/// CLOSE) are compared with the raw value, so they stay in raw units.
/// </summary>
/// <param name="Raw">The raw 9-bit value, 0 to 511.</param>
public readonly record struct Rssi(ushort Raw)
{
    /// <summary>The chip's own reading in dBm, uncorrected (0.5 dB steps).</summary>
    public double Dbm => Raw / 2.0 - 160.0;

    /// <summary>
    /// The level the radio's screen shows at <paramref name="frequencyHz"/>, in whole dBm: raw / 2
    /// rounded down, minus 160, plus the band's correction (<see cref="RssiScale.CorrectionDb"/>).
    /// </summary>
    public int DbmAt(long frequencyHz) => Raw / 2 - 160 + RssiScale.CorrectionDb(frequencyHz);

    /// <summary>The radio's S-meter reading at <paramref name="frequencyHz"/>, for example "S3" or "S9+12".</summary>
    public string SMeterAt(long frequencyHz) => RssiScale.SMeter(DbmAt(frequencyHz));

    /// <summary>The raw value nearest to <paramref name="dbm"/> on the chip's uncorrected scale.</summary>
    public static Rssi FromDbm(double dbm) =>
        new((ushort)Math.Clamp(Math.Round((dbm + 160.0) * 2.0), 0, 511));

    /// <summary>The level as the radio shows it at <paramref name="frequencyHz"/>, with the raw value: "-127 dBm S0 (raw 67)".</summary>
    public string Describe(long frequencyHz) =>
        string.Create(CultureInfo.InvariantCulture, $"{DbmAt(frequencyHz)} dBm {SMeterAt(frequencyHz)} (raw {Raw})");

    /// <summary>The chip's uncorrected reading; see <see cref="Describe"/> for the level as the radio shows it.</summary>
    public override string ToString() => string.Create(CultureInfo.InvariantCulture, $"{Dbm:F1} dBm uncorrected (raw {Raw})");
}

/// <summary>
/// The radio's signal scale (ui/main.c): the chip's dBm plus a per-band correction, and an S-meter
/// with S0 at -130 dBm and S9 at -76 dBm. The corrections are upstream's empirical table, not yet
/// checked against a calibrated signal.
/// </summary>
public static class RssiScale
{
    /// <summary>S0 on the radio's S-meter, dBm.</summary>
    public const int S0Dbm = -130;

    /// <summary>S9 on the radio's S-meter, dBm.</summary>
    public const int S9Dbm = -76;

    // frequencies.c: a frequency belongs to the highest band whose lower edge it reaches; below
    // 108 MHz is band 1. ui/main.c dBmCorrTable, bands 1 to 7.
    private static readonly (long FromHz, int CorrectionDb)[] Bands =
    [
        (50_000_000, -15),
        (108_000_000, -25),
        (137_000_000, -20),
        (174_000_000, -4),
        (350_000_000, -7),
        (400_000_000, -6),
        (470_000_000, -1),
    ];

    /// <summary>The screen's correction in dB for a receive frequency: -15 (below 108 MHz), -25 (108 to 137), -20 (137 to 174), -4 (174 to 350), -7 (350 to 400), -6 (400 to 470), -1 (470 up).</summary>
    public static int CorrectionDb(long frequencyHz)
    {
        for (int i = Bands.Length - 1; i > 0; i--)
        {
            if (frequencyHz >= Bands[i].FromHz)
            {
                return Bands[i].CorrectionDb;
            }
        }

        return Bands[0].CorrectionDb;
    }

    /// <summary>The radio's S-meter for a corrected level: S0 to S9 at 6 dB per unit, then "S9+N" (dB over S9, up to 99) from 10 dB over.</summary>
    public static string SMeter(int dbm)
    {
        int over = Math.Clamp(dbm - S9Dbm, 0, 99);
        if (over >= 10)
        {
            return string.Create(CultureInfo.InvariantCulture, $"S9+{over}");
        }

        int s = Math.Clamp((int)Math.Floor((dbm - S0Dbm) / ((S9Dbm - S0Dbm) / 9.0)), 0, 9);
        return string.Create(CultureInfo.InvariantCulture, $"S{s}");
    }
}

/// <summary>
/// How the deviation register maps to deviation, measured on the bench: REG_40&lt;11:0&gt; is
/// logarithmic and +0x100 doubles the deviation (about 0.0235 dB per step). The absolute level
/// depends on the TNC's drive level and the AIOC's TX EQ, so a law is anchored at one measured
/// point: a register value and the deviation it gives for a 1 kHz tone at 0 dBFS from the TNC.
/// </summary>
/// <param name="ReferenceRegister">The register value of the anchor point.</param>
/// <param name="ReferenceKilohertz">Deviation at the anchor, kHz, for a full-scale (0 dBFS) tone.</param>
/// <param name="Description">What the anchor was measured with.</param>
public sealed record DeviationLaw(ushort ReferenceRegister, double ReferenceKilohertz, string Description)
{
    /// <summary>Register steps per doubling of deviation.</summary>
    public const double StepsPerOctave = 256.0;

    /// <summary>
    /// The bench AIOC (rev 1.0) with its stored TX EQ (-5.74 dB at 1 kHz): 0x856 gives about
    /// 2.8 kHz at 0 dBFS. The firmware's default wide deviation is 0x856 for this reason.
    /// </summary>
    public static DeviationLaw AiocWithBenchEq { get; } =
        new(0x856, 2.8, "AIOC with the bench TX EQ, 1 kHz at 0 dBFS");

    /// <summary>A stock AIOC with no TX EQ: 0x762 gives the same 2.8 kHz at 0 dBFS (5.74 dB less register).</summary>
    public static DeviationLaw StockAioc { get; } =
        new(0x762, 2.8, "stock AIOC without TX EQ, 1 kHz at 0 dBFS");

    /// <summary>Deviation in kHz that <paramref name="register"/> gives at 0 dBFS under this law.</summary>
    public double KilohertzAt(ushort register) =>
        ReferenceKilohertz * Math.Pow(2.0, (register - ReferenceRegister) / StepsPerOctave);

    /// <summary>The register value nearest to <paramref name="kilohertz"/> at 0 dBFS, not clamped.</summary>
    public int RegisterFor(double kilohertz)
    {
        if (!(kilohertz > 0) || double.IsInfinity(kilohertz))
        {
            throw new ArgumentOutOfRangeException(nameof(kilohertz), kilohertz, "deviation must be a positive number of kHz");
        }

        return (int)Math.Round(ReferenceRegister + StepsPerOctave * Math.Log2(kilohertz / ReferenceKilohertz));
    }
}

/// <summary>
/// A TX deviation setting: the raw BK4819 REG_40&lt;11:0&gt; value, with its meaning in kHz under a
/// <see cref="DeviationLaw"/>. The register is what the radio stores; kHz is an estimate for a
/// full-scale tone from the TNC.
/// </summary>
public readonly record struct Deviation
{
    /// <summary>
    /// Highest register value the firmware accepts. From 0xB00 up the chip wraps to near zero
    /// deviation, so the firmware clamps to 0xA7F.
    /// </summary>
    public const ushort MaxRegister = 0x0A7F;

    private readonly DeviationLaw? _law;

    /// <summary>Creates a deviation from a raw register value.</summary>
    /// <exception cref="ArgumentOutOfRangeException">Above <see cref="MaxRegister"/>.</exception>
    public Deviation(ushort register, DeviationLaw? law = null)
    {
        if (register > MaxRegister)
        {
            throw new ArgumentOutOfRangeException(nameof(register), $"0x{register:X3} is above 0x{MaxRegister:X3}: the chip wraps to near-zero deviation from 0xB00");
        }

        Register = register;
        _law = law;
    }

    /// <summary>The raw REG_40&lt;11:0&gt; value.</summary>
    public ushort Register { get; }

    /// <summary>The law used for <see cref="Kilohertz"/> (the bench AIOC law if none was given).</summary>
    public DeviationLaw Law => _law ?? DeviationLaw.AiocWithBenchEq;

    /// <summary>Estimated deviation for a full-scale tone, kHz.</summary>
    public double Kilohertz => Law.KilohertzAt(Register);

    /// <summary>
    /// The register that gives <paramref name="kilohertz"/> for a full-scale tone under
    /// <paramref name="law"/>.
    /// </summary>
    /// <exception cref="ArgumentOutOfRangeException">The result would be above <see cref="MaxRegister"/> or below zero.</exception>
    public static Deviation FromKilohertz(double kilohertz, DeviationLaw? law = null)
    {
        law ??= DeviationLaw.AiocWithBenchEq;
        int reg = law.RegisterFor(kilohertz);
        if (reg is < 0 or > MaxRegister)
        {
            throw new ArgumentOutOfRangeException(nameof(kilohertz), kilohertz,
                string.Create(CultureInfo.InvariantCulture, $"{kilohertz} kHz needs register 0x{reg:X}, outside 0 to 0x{MaxRegister:X3} under the law '{law.Description}'"));
        }

        return new Deviation((ushort)reg, law);
    }

    /// <summary>The same register interpreted under another law.</summary>
    public Deviation WithLaw(DeviationLaw law) => new(Register, law);

    /// <summary>The setting 0x100 lower: half the deviation (the firmware's narrow default is wide minus 0x100).</summary>
    public Deviation Halved() => new((ushort)Math.Max(0, Register - 0x100), _law);

    /// <inheritdoc/>
    public override string ToString() =>
        string.Create(CultureInfo.InvariantCulture, $"0x{Register:X3} (about {Kilohertz:F2} kHz at 0 dBFS)");
}

/// <summary>TX power.</summary>
public enum TxPower : byte
{
    /// <summary>Low.</summary>
    Low = 0,

    /// <summary>Mid.</summary>
    Mid = 1,

    /// <summary>High.</summary>
    High = 2,
}

/// <summary>Channel bandwidth.</summary>
public enum Bandwidth : byte
{
    /// <summary>Wide (25 kHz).</summary>
    Wide = 0,

    /// <summary>Narrow (12.5 kHz).</summary>
    Narrow = 1,
}

/// <summary>What the firmware's busy (carrier detect) state is built from.</summary>
[Flags]
public enum BusySources : byte
{
    /// <summary>None (not a valid setting).</summary>
    None = 0,

    /// <summary>The chip's squelch detector (REG_0C bit 1), polled every 1 ms, with the thresholds of the busy detector level. A detector only: nothing is muted.</summary>
    Squelch = 1,

    /// <summary>RSSI thresholds with hang time, sampled every 2 ms.</summary>
    Rssi = 2,
}

/// <summary>AGC mode (diagnostic parameter AGC_FIX).</summary>
public readonly record struct AgcSetting
{
    private AgcSetting(byte raw) => Raw = raw;

    /// <summary>Automatic AGC.</summary>
    public static AgcSetting Auto { get; } = new(0xFF);

    /// <summary>The raw parameter byte: 0xFF auto, else the fixed index.</summary>
    public byte Raw { get; }

    /// <summary>True for automatic AGC.</summary>
    public bool IsAuto => Raw == 0xFF;

    /// <summary>A fixed AGC index, 0 to 7 (REG_7E&lt;14:12&gt; code).</summary>
    public static AgcSetting Fixed(int index) =>
        index is >= 0 and <= 7 ? new AgcSetting((byte)index) : throw new ArgumentOutOfRangeException(nameof(index), index, "AGC index is 0 to 7");

    internal static AgcSetting FromRaw(byte raw) => new(raw);

    /// <inheritdoc/>
    public override string ToString() => IsAuto ? "auto" : $"fixed {Raw}";
}

/// <summary>An AGC readback: REG_7E&lt;15&gt; (fixed) and REG_7E&lt;14:12&gt; (index).</summary>
/// <param name="Raw">Encoded as in GET_STATUS: bit 7 fixed, bits 2:0 index.</param>
public readonly record struct AgcReading(byte Raw)
{
    /// <summary>AGC is fixed (REG_7E bit 15).</summary>
    public bool IsFixed => (Raw & 0x80) != 0;

    /// <summary>AGC index, 0 to 7.</summary>
    public int Index => Raw & 7;

    /// <inheritdoc/>
    public override string ToString() => $"{(IsFixed ? "fixed" : "auto")} {Index}";
}

/// <summary>Raw thresholds of the chip's squelch detector (BUSY_SQL_RAW, RAM only): override the busy detector level's table row until BUSY_SQL_LEVEL is set or the radio reboots.</summary>
/// <param name="RssiOpen">RSSI open threshold, 0 to 255.</param>
/// <param name="RssiClose">RSSI close threshold, 0 to 255.</param>
/// <param name="NoiseOpen">Noise open threshold, 0 to 127.</param>
/// <param name="NoiseClose">Noise close threshold, 0 to 127.</param>
/// <param name="GlitchOpen">Glitch open threshold, 0 to 255.</param>
/// <param name="GlitchClose">Glitch close threshold, 0 to 255.</param>
public readonly record struct BusySquelchThresholds(byte RssiOpen, byte RssiClose, byte NoiseOpen, byte NoiseClose, byte GlitchOpen, byte GlitchClose);
