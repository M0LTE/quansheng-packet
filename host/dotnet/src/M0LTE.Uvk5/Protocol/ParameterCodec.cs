using System.Globalization;

namespace M0LTE.Uvk5.Protocol;

/// <summary>TLV encoding of <see cref="RadioSettings"/> for GET_PARAMS and SET_PARAMS, and the range table.</summary>
internal static class ParameterCodec
{
    public static readonly int[] TxTimeoutSeconds = [5, 10, 15, 20, 30, 60, 120];

    /// <summary>Wire size of each parameter's value.</summary>
    public static int SizeOf(RadioParameterId id) => id switch
    {
        RadioParameterId.FrequencyHz => 4,
        RadioParameterId.DeviationWide or RadioParameterId.DeviationNarrow or RadioParameterId.SerialLockMs
            or RadioParameterId.BusyRssiOpen or RadioParameterId.BusyRssiClose => 2,
        RadioParameterId.SquelchRaw => 6,
        _ when IsKnown(id) => 1,
        _ => -1,
    };

    public static bool IsKnown(RadioParameterId id) => id is >= RadioParameterId.FrequencyHz and <= RadioParameterId.KeyLock;

    public static bool IsRamOnly(RadioParameterId id) =>
        id is RadioParameterId.SquelchRaw or RadioParameterId.AgcFix or RadioParameterId.Afc;

    public static IReadOnlyList<RadioParameterId> IdsPresent(RadioSettings s)
    {
        var ids = new List<RadioParameterId>();
        void Add(bool present, RadioParameterId id)
        {
            if (present)
            {
                ids.Add(id);
            }
        }

        Add(s.FrequencyHz.HasValue, RadioParameterId.FrequencyHz);
        Add(s.Power.HasValue, RadioParameterId.Power);
        Add(s.Bandwidth.HasValue, RadioParameterId.Bandwidth);
        Add(s.DeviationWide.HasValue, RadioParameterId.DeviationWide);
        Add(s.DeviationNarrow.HasValue, RadioParameterId.DeviationNarrow);
        Add(s.MicGain.HasValue, RadioParameterId.MicGain);
        Add(s.Squelch.HasValue, RadioParameterId.Squelch);
        Add(s.RxGain.HasValue, RadioParameterId.RxGain);
        Add(s.RxDacGain.HasValue, RadioParameterId.RxDacGain);
        Add(s.TxTimeout.HasValue, RadioParameterId.TxTimeoutSeconds);
        Add(s.PttPressDebounce.HasValue, RadioParameterId.PttPressMs);
        Add(s.PttReleaseDebounce.HasValue, RadioParameterId.PttReleaseMs);
        Add(s.PaEnableDelay.HasValue, RadioParameterId.PaEnableDelayMs);
        Add(s.PaBiasDelay.HasValue, RadioParameterId.PaBiasDelayMs);
        Add(s.SerialLock.HasValue, RadioParameterId.SerialLockMs);
        Add(s.BusySource.HasValue, RadioParameterId.BusySource);
        Add(s.BusyRssiOpen.HasValue, RadioParameterId.BusyRssiOpen);
        Add(s.BusyRssiClose.HasValue, RadioParameterId.BusyRssiClose);
        Add(s.BusyHang.HasValue, RadioParameterId.BusyHangMs);
        Add(s.SquelchThresholds.HasValue, RadioParameterId.SquelchRaw);
        Add(s.Agc.HasValue, RadioParameterId.AgcFix);
        Add(s.Afc.HasValue, RadioParameterId.Afc);
        Add(s.Backlight.HasValue, RadioParameterId.Backlight);
        Add(s.KeyLock.HasValue, RadioParameterId.KeyLock);
        return ids;
    }

    public static RadioSettings Merge(RadioSettings a, RadioSettings b) => a with
    {
        FrequencyHz = b.FrequencyHz ?? a.FrequencyHz,
        Power = b.Power ?? a.Power,
        Bandwidth = b.Bandwidth ?? a.Bandwidth,
        DeviationWide = b.DeviationWide ?? a.DeviationWide,
        DeviationNarrow = b.DeviationNarrow ?? a.DeviationNarrow,
        MicGain = b.MicGain ?? a.MicGain,
        Squelch = b.Squelch ?? a.Squelch,
        RxGain = b.RxGain ?? a.RxGain,
        RxDacGain = b.RxDacGain ?? a.RxDacGain,
        TxTimeout = b.TxTimeout ?? a.TxTimeout,
        PttPressDebounce = b.PttPressDebounce ?? a.PttPressDebounce,
        PttReleaseDebounce = b.PttReleaseDebounce ?? a.PttReleaseDebounce,
        PaEnableDelay = b.PaEnableDelay ?? a.PaEnableDelay,
        PaBiasDelay = b.PaBiasDelay ?? a.PaBiasDelay,
        SerialLock = b.SerialLock ?? a.SerialLock,
        BusySource = b.BusySource ?? a.BusySource,
        BusyRssiOpen = b.BusyRssiOpen ?? a.BusyRssiOpen,
        BusyRssiClose = b.BusyRssiClose ?? a.BusyRssiClose,
        BusyHang = b.BusyHang ?? a.BusyHang,
        SquelchThresholds = b.SquelchThresholds ?? a.SquelchThresholds,
        Agc = b.Agc ?? a.Agc,
        Afc = b.Afc ?? a.Afc,
        Backlight = b.Backlight ?? a.Backlight,
        KeyLock = b.KeyLock ?? a.KeyLock,
    };

    /// <summary>
    /// Validates every set value against the protocol's ranges and returns the raw wire value per
    /// id. Throws <see cref="ArgumentOutOfRangeException"/> naming the setting.
    /// </summary>
    public static List<(RadioParameterId Id, ulong Value)> ToWire(RadioSettings s)
    {
        var list = new List<(RadioParameterId, ulong)>();
        if (s.FrequencyHz is { } f)
        {
            if (f is < 50_000_000 or > 600_000_000 || f % 10 != 0)
            {
                throw Range(nameof(s.FrequencyHz), f, "50 to 600 MHz, a multiple of 10 Hz");
            }

            list.Add((RadioParameterId.FrequencyHz, (ulong)f));
        }

        if (s.Power is { } p)
        {
            Check(nameof(s.Power), (int)p, 0, 2);
            list.Add((RadioParameterId.Power, (ulong)p));
        }

        if (s.Bandwidth is { } bw)
        {
            Check(nameof(s.Bandwidth), (int)bw, 0, 1);
            list.Add((RadioParameterId.Bandwidth, (ulong)bw));
        }

        if (s.DeviationWide is { } dw)
        {
            list.Add((RadioParameterId.DeviationWide, dw.Register));
        }

        if (s.DeviationNarrow is { } dn)
        {
            list.Add((RadioParameterId.DeviationNarrow, dn.Register));
        }

        Int(list, RadioParameterId.MicGain, nameof(s.MicGain), s.MicGain, 0, 31);
        Int(list, RadioParameterId.Squelch, nameof(s.Squelch), s.Squelch, 0, 9);
        Int(list, RadioParameterId.RxGain, nameof(s.RxGain), s.RxGain, 0, 63);
        Int(list, RadioParameterId.RxDacGain, nameof(s.RxDacGain), s.RxDacGain, 0, 15);
        if (s.TxTimeout is { } tt)
        {
            int sec = WholeUnits(nameof(s.TxTimeout), tt, 1000);
            if (Array.IndexOf(TxTimeoutSeconds, sec) < 0)
            {
                throw Range(nameof(s.TxTimeout), tt, "5, 10, 15, 20, 30, 60 or 120 s");
            }

            list.Add((RadioParameterId.TxTimeoutSeconds, (ulong)sec));
        }

        Ms(list, RadioParameterId.PttPressMs, nameof(s.PttPressDebounce), s.PttPressDebounce, 1, 40);
        Ms(list, RadioParameterId.PttReleaseMs, nameof(s.PttReleaseDebounce), s.PttReleaseDebounce, 2, 40);
        Ms(list, RadioParameterId.PaEnableDelayMs, nameof(s.PaEnableDelay), s.PaEnableDelay, 1, 20);
        Ms(list, RadioParameterId.PaBiasDelayMs, nameof(s.PaBiasDelay), s.PaBiasDelay, 0, 20);
        if (s.SerialLock is { } sl)
        {
            int ms = WholeUnits(nameof(s.SerialLock), sl, 1);
            if (ms is < 0 or > 1500 || ms % 10 != 0)
            {
                throw Range(nameof(s.SerialLock), sl, "0 to 1500 ms in 10 ms steps");
            }

            list.Add((RadioParameterId.SerialLockMs, (ulong)ms));
        }

        if (s.BusySource is { } bs)
        {
            Check(nameof(s.BusySource), (int)bs, 1, 3);
            list.Add((RadioParameterId.BusySource, (ulong)bs));
        }

        if (s.BusyRssiOpen is { } ro)
        {
            Check(nameof(s.BusyRssiOpen), ro.Raw, 0, 511);
            list.Add((RadioParameterId.BusyRssiOpen, ro.Raw));
        }

        if (s.BusyRssiClose is { } rc)
        {
            Check(nameof(s.BusyRssiClose), rc.Raw, 0, 511);
            if (s.BusyRssiOpen is { } open && rc.Raw > open.Raw)
            {
                throw Range(nameof(s.BusyRssiClose), rc, "at most BusyRssiOpen");
            }

            list.Add((RadioParameterId.BusyRssiClose, rc.Raw));
        }

        Ms(list, RadioParameterId.BusyHangMs, nameof(s.BusyHang), s.BusyHang, 0, 250);
        if (s.SquelchThresholds is { } q)
        {
            if (q.NoiseOpen > 127 || q.NoiseClose > 127)
            {
                throw Range(nameof(s.SquelchThresholds), q, "noise thresholds 0 to 127");
            }

            ulong v = q.RssiOpen | ((ulong)q.RssiClose << 8) | ((ulong)q.NoiseOpen << 16) | ((ulong)q.NoiseClose << 24)
                | ((ulong)q.GlitchOpen << 32) | ((ulong)q.GlitchClose << 40);
            list.Add((RadioParameterId.SquelchRaw, v));
        }

        if (s.Agc is { } agc)
        {
            list.Add((RadioParameterId.AgcFix, agc.Raw));
        }

        if (s.Afc is { } afc)
        {
            list.Add((RadioParameterId.Afc, afc ? 1UL : 0UL));
        }

        Int(list, RadioParameterId.Backlight, nameof(s.Backlight), s.Backlight, 0, 7);
        if (s.KeyLock is { } kl)
        {
            list.Add((RadioParameterId.KeyLock, kl ? 1UL : 0UL));
        }

        return list;
    }

    public static void WriteRecord(WireWriter w, RadioParameterId id, ulong value)
    {
        w.U8((byte)id);
        int size = SizeOf(id);
        for (int i = 0; i < size; i++)
        {
            w.U8((byte)(value >> (8 * i)));
        }
    }

    /// <summary>Reads <c>(id, value)</c> records to the end of the body into settings.</summary>
    public static RadioSettings ReadRecords(ref WireReader r, DeviationLaw law, out List<RadioParameterId> order)
    {
        var s = new RadioSettings();
        order = [];
        while (r.Remaining > 0)
        {
            var id = (RadioParameterId)r.U8();
            int size = SizeOf(id);
            if (size < 0)
            {
                throw new K5ProtocolException($"parameter record with unknown id 0x{(byte)id:X2}: cannot know its size");
            }

            ulong v = 0;
            ReadOnlySpan<byte> bytes = r.Bytes(size);
            for (int i = 0; i < size; i++)
            {
                v |= (ulong)bytes[i] << (8 * i);
            }

            s = Apply(s, id, v, law);
            order.Add(id);
        }

        return s;
    }

    public static RadioSettings Apply(RadioSettings s, RadioParameterId id, ulong v, DeviationLaw law) => id switch
    {
        RadioParameterId.FrequencyHz => s with { FrequencyHz = (long)v },
        RadioParameterId.Power => s with { Power = (TxPower)v },
        RadioParameterId.Bandwidth => s with { Bandwidth = (Bandwidth)v },
        RadioParameterId.DeviationWide => s with { DeviationWide = new Deviation((ushort)Math.Min(v, Deviation.MaxRegister), law) },
        RadioParameterId.DeviationNarrow => s with { DeviationNarrow = new Deviation((ushort)Math.Min(v, Deviation.MaxRegister), law) },
        RadioParameterId.MicGain => s with { MicGain = (int)v },
        RadioParameterId.Squelch => s with { Squelch = (int)v },
        RadioParameterId.RxGain => s with { RxGain = (int)v },
        RadioParameterId.RxDacGain => s with { RxDacGain = (int)v },
        RadioParameterId.TxTimeoutSeconds => s with { TxTimeout = TimeSpan.FromSeconds(v) },
        RadioParameterId.PttPressMs => s with { PttPressDebounce = TimeSpan.FromMilliseconds(v) },
        RadioParameterId.PttReleaseMs => s with { PttReleaseDebounce = TimeSpan.FromMilliseconds(v) },
        RadioParameterId.PaEnableDelayMs => s with { PaEnableDelay = TimeSpan.FromMilliseconds(v) },
        RadioParameterId.PaBiasDelayMs => s with { PaBiasDelay = TimeSpan.FromMilliseconds(v) },
        RadioParameterId.SerialLockMs => s with { SerialLock = TimeSpan.FromMilliseconds(v) },
        RadioParameterId.BusySource => s with { BusySource = (BusySources)v },
        RadioParameterId.BusyRssiOpen => s with { BusyRssiOpen = new Rssi((ushort)v) },
        RadioParameterId.BusyRssiClose => s with { BusyRssiClose = new Rssi((ushort)v) },
        RadioParameterId.BusyHangMs => s with { BusyHang = TimeSpan.FromMilliseconds(v) },
        RadioParameterId.SquelchRaw => s with
        {
            SquelchThresholds = new SquelchThresholds((byte)v, (byte)(v >> 8), (byte)(v >> 16), (byte)(v >> 24), (byte)(v >> 32), (byte)(v >> 40)),
        },
        RadioParameterId.AgcFix => s with { Agc = AgcSetting.FromRaw((byte)v) },
        RadioParameterId.Afc => s with { Afc = v != 0 },
        RadioParameterId.Backlight => s with { Backlight = (int)v },
        RadioParameterId.KeyLock => s with { KeyLock = v != 0 },
        _ => s,
    };

    private static void Int(List<(RadioParameterId, ulong)> list, RadioParameterId id, string name, int? value, int min, int max)
    {
        if (value is { } v)
        {
            Check(name, v, min, max);
            list.Add((id, (ulong)v));
        }
    }

    private static void Ms(List<(RadioParameterId, ulong)> list, RadioParameterId id, string name, TimeSpan? value, int min, int max)
    {
        if (value is { } t)
        {
            int ms = WholeUnits(name, t, 1);
            Check(name, ms, min, max, " ms");
            list.Add((id, (ulong)ms));
        }
    }

    private static int WholeUnits(string name, TimeSpan t, int msPerUnit)
    {
        double units = t.TotalMilliseconds / msPerUnit;
        if (Math.Abs(units - Math.Round(units)) > 1e-9 || units > int.MaxValue || units < int.MinValue)
        {
            throw Range(name, t, msPerUnit == 1 ? "a whole number of milliseconds" : "a whole number of seconds");
        }

        return (int)Math.Round(units);
    }

    private static void Check(string name, int v, int min, int max, string unit = "")
    {
        if (v < min || v > max)
        {
            throw Range(name, v, $"{min} to {max}{unit}");
        }
    }

    private static ArgumentOutOfRangeException Range(string name, object value, string allowed) =>
        new(name, value, string.Create(CultureInfo.InvariantCulture, $"{name} must be {allowed}"));
}
