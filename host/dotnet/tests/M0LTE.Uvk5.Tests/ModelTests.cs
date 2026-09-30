using M0LTE.Uvk5.Aioc;
using M0LTE.Uvk5.Protocol;

namespace M0LTE.Uvk5.Tests;

public class ModelTests
{
    [Fact]
    public void Deviation_follows_the_measured_log_law()
    {
        var law = DeviationLaw.AiocWithBenchEq;
        Assert.Equal(2.8, law.KilohertzAt(0x856), 6);
        Assert.Equal(5.6, law.KilohertzAt(0x956), 6);        // +0x100 doubles
        Assert.Equal(1.4, law.KilohertzAt(0x756), 6);        // narrow default: half
        Assert.Equal(0x856, Deviation.FromKilohertz(2.8).Register);
        Assert.Equal(0x856, Deviation.FromKilohertz(2.8).Halved().Register + 0x100);
        Assert.Equal(0.0235, 20 * Math.Log10(2) / 256, 4);  // about 0.0235 dB per step

        // The bench table (results.md): 0x600 918 Hz, 0x700 1838 Hz, 0x800 3671 Hz, 0x900 7358 Hz.
        var bench = new DeviationLaw(0x800, 3.671, "results.md at -1.94 dBFS");
        Assert.InRange(bench.KilohertzAt(0x600) / 0.918, 0.99, 1.01);
        Assert.InRange(bench.KilohertzAt(0x700) / 1.838, 0.99, 1.01);
        Assert.InRange(bench.KilohertzAt(0x900) / 7.358, 0.99, 1.01);
    }

    [Fact]
    public void Stock_aioc_law_is_5_74_db_lower_in_register()
    {
        Assert.Equal(2.8, DeviationLaw.StockAioc.KilohertzAt(0x762), 6);
        double db = 20 * Math.Log10(DeviationLaw.AiocWithBenchEq.KilohertzAt(0x856) / DeviationLaw.AiocWithBenchEq.KilohertzAt(0x762));
        Assert.Equal(5.74, db, 1);
        Assert.Equal(0x762, Deviation.FromKilohertz(2.8, DeviationLaw.StockAioc).Register);
    }

    [Fact]
    public void Deviation_register_is_clamped_below_the_wrap()
    {
        Assert.Throws<ArgumentOutOfRangeException>(() => new Deviation(0xA80));
        Assert.Equal(Deviation.MaxRegister, new Deviation(0xA7F).Register);
        Assert.Contains("kHz", new Deviation(0x856).ToString());
    }

    [Fact]
    public void Rssi_conversion()
    {
        Assert.Equal(-105, new Rssi(110).Dbm);
        Assert.Equal(-108, new Rssi(104).Dbm);
        Assert.Equal(110, Rssi.FromDbm(-105).Raw);
    }

    [Fact]
    public void Rssi_as_the_radio_shows_it()
    {
        // Tom's bench radio on 144.800 MHz into a terminated load: -107 dBm uncorrected, -127 on the screen.
        Assert.Equal(-107, new Rssi(106).Dbm);
        Assert.Equal(-127, new Rssi(106).DbmAt(144_800_000));
        Assert.Equal(-127, new Rssi(107).DbmAt(144_800_000)); // the screen halves the raw value rounding down
        Assert.Equal("S0", new Rssi(106).SMeterAt(144_800_000));
        Assert.Equal("-127 dBm S0 (raw 106)", new Rssi(106).Describe(144_800_000));
        Assert.Equal(106, new Rssi(106).Raw);
    }

    [Theory]
    [InlineData(0, -15)]
    [InlineData(50_000_000, -15)]
    [InlineData(107_999_990, -15)]
    [InlineData(108_000_000, -25)]
    [InlineData(136_999_990, -25)]
    [InlineData(137_000_000, -20)]
    [InlineData(173_999_990, -20)]
    [InlineData(174_000_000, -4)]
    [InlineData(349_999_990, -4)]
    [InlineData(350_000_000, -7)]
    [InlineData(399_999_990, -7)]
    [InlineData(400_000_000, -6)]
    [InlineData(469_999_990, -6)]
    [InlineData(470_000_000, -1)]
    [InlineData(600_000_000, -1)]
    public void Rssi_band_correction_edges(long hz, int db)
    {
        Assert.Equal(db, RssiScale.CorrectionDb(hz));
        Assert.Equal(-60 + db, new Rssi(200).DbmAt(hz));
    }

    [Theory]
    [InlineData(-160, "S0")]
    [InlineData(-125, "S0")]
    [InlineData(-124, "S1")]
    [InlineData(-82, "S8")]
    [InlineData(-77, "S8")]
    [InlineData(-76, "S9")]
    [InlineData(-67, "S9")]
    [InlineData(-66, "S9+10")]
    [InlineData(-43, "S9+33")]
    [InlineData(40, "S9+99")]
    public void S_meter_matches_the_radio(int dbm, string expected) =>
        Assert.Equal(expected, RssiScale.SMeter(dbm));

    [Fact]
    public void Settings_merge_and_ids()
    {
        var a = new RadioSettings { BusySquelchLevel = 1, Power = TxPower.Low };
        var b = new RadioSettings { Power = TxPower.High, KeyLock = true };
        var m = a.Merge(b);
        Assert.Equal(1, m.BusySquelchLevel);
        Assert.Equal(TxPower.High, m.Power);
        Assert.Equal([RadioParameterId.Power, RadioParameterId.KeyLock, RadioParameterId.BusySquelchLevel], m.SetIds);
    }

    [Fact]
    public void Settings_wire_round_trip()
    {
        var s = new RadioSettings
        {
            FrequencyHz = 433_500_000,
            DeviationNarrow = new Deviation(0x662),
            BusySquelchThresholds = new BusySquelchThresholds(90, 80, 40, 50, 20, 30),
            Agc = AgcSetting.Fixed(3),
            SerialLock = TimeSpan.FromMilliseconds(40),
            TxTimeout = TimeSpan.FromSeconds(120),
        };
        var w = new WireWriter();
        foreach (var (id, v) in ParameterCodec.ToWire(s))
        {
            ParameterCodec.WriteRecord(w, id, v);
        }

        var r = new WireReader(w.ToArray(), "test");
        var back = ParameterCodec.ReadRecords(ref r, DeviationLaw.AiocWithBenchEq, out var order);
        Assert.Equal(s.SetIds, order);
        Assert.Equal(s.FrequencyHz, back.FrequencyHz);
        Assert.Equal(s.DeviationNarrow!.Value.Register, back.DeviationNarrow!.Value.Register);
        Assert.Equal(s.BusySquelchThresholds, back.BusySquelchThresholds);
        Assert.Equal(s.Agc, back.Agc);
        Assert.Equal(s.SerialLock, back.SerialLock);
        Assert.Equal(s.TxTimeout, back.TxTimeout);
    }

    private sealed class FakeHid : IFeatureReportDevice
    {
        public List<byte[]> Sets { get; } = [];

        public uint Register60 { get; set; } = 0x00010100;

        public void SetFeature(ReadOnlySpan<byte> report)
        {
            Sets.Add(report.ToArray());
            if ((report[1] & 0x01) != 0 && report[2] == 0x60)
            {
                Register60 = BitConverter.ToUInt32(report[3..7]);
            }
        }

        public void GetFeature(Span<byte> report)
        {
            report[1] = 0;
            report[2] = 0x60;
            BitConverter.TryWriteBytes(report[3..7], Register60);
        }

        public void Dispose()
        {
        }
    }

    [Fact]
    public void Aioc_live_tx_scope_clears_rxignptt_in_ram_and_restores_it()
    {
        var dev = new FakeHid();
        using var hid = new AiocHid(dev);
        using (hid.AllowRadioOutputWhileKeyed())
        {
            Assert.Equal(0x00000100u, dev.Register60);
        }

        Assert.Equal(0x00010100u, dev.Register60);
        Assert.All(dev.Sets, s => Assert.Equal(0, s[1] & 0xF0));     // never store, default, recall or reboot
        Assert.All(dev.Sets, s => Assert.Equal(7, s.Length));
    }

    [Fact]
    public void Aioc_refuses_without_txfrcptt()
    {
        var dev = new FakeHid { Register60 = 0x00010000 };
        using var hid = new AiocHid(dev);
        Assert.Throws<InvalidOperationException>(() => hid.AllowRadioOutputWhileKeyed());
        Assert.DoesNotContain(dev.Sets, s => (s[1] & 1) != 0);
    }
}
