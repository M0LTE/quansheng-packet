using M0LTE.Uvk5.Simulation;

namespace M0LTE.Uvk5.Tests;

/// <summary>Every command against the simulated radio, on each firmware.</summary>
public class CommandTests
{
    private static CancellationToken Ct => TestContext.Current.CancellationToken;

    [Theory]
    [InlineData(FirmwareKind.PacketV2, true)]
    [InlineData(FirmwareKind.PacketV2, false)]
    [InlineData(FirmwareKind.PacketV1, true)]
    [InlineData(FirmwareKind.PacketV1, false)]
    [InlineData(FirmwareKind.Stock, true)]
    public async Task Identifies_the_firmware(FirmwareKind kind, bool obfuscate)
    {
        await using var rig = await Rig.StartAsync(kind, options: new K5RadioOptions { Obfuscate = obfuscate });
        Assert.Equal(kind, rig.Radio.Firmware.Kind);
        Assert.Equal(rig.Sim.Version, rig.Radio.Firmware.Version);
        Assert.Equal(obfuscate, rig.Sim.IsObfuscated);
        Assert.Equal(kind == FirmwareKind.PacketV2, rig.Radio.Firmware.Supports(K5Features.Events));
        Assert.Equal(kind != FirmwareKind.Stock, rig.Radio.Firmware.Supports(K5Features.Settings));
        Assert.Empty(rig.Sim.Violations);
    }

    [Fact]
    public async Task Get_info_fills_capabilities()
    {
        await using var rig = await Rig.StartAsync();
        var fw = rig.Radio.Firmware;
        Assert.Equal(new Version(2, 0), fw.ProtocolVersion);
        Assert.True(fw.Capabilities.HasFlag(RadioCapabilities.LiveTx));
        Assert.False(fw.Capabilities.HasFlag(RadioCapabilities.LevelToneCalibrated));
        Assert.Equal(TimeSpan.FromMilliseconds(20), fw.SerialLock);
        Assert.Equal(TimeSpan.FromMilliseconds(30), fw.LateKeyMax);
        Assert.Equal(120, fw.MaxRequestBody);
        Assert.True(fw.Supports(RadioParameterId.KeyLock));
        Assert.Equal((RadioEvents)0x1FFF, fw.SupportedEvents);
        Assert.Equal(2, fw.SettingsBlockLayout);        // the signed family (v1.0.1 on)
        Assert.Equal(1, fw.V2BlockLayout);
        Assert.Equal(FirmwareInfo.FixedTxBandPolicy, fw.TxBandPlan);
        Assert.Equal(0, fw.TxBandFlags);
    }

    [Fact]
    public async Task Status_reports_the_radio()
    {
        await using var rig = await Rig.StartAsync();
        var s = await rig.Radio.GetStatusAsync(Ct);
        Assert.Equal(144_800_000, s.FrequencyHz);
        Assert.Equal(RadioState.Idle, s.State);
        Assert.Equal(TxPower.Low, s.Power);
        Assert.Equal(0x856, s.Deviation.Register);
        Assert.True(s.IsTxAllowed);
        Assert.Equal(7.8, s.BatteryVolts, 2);
        Assert.Null(s.TxTimeLeft);
        Assert.Equal(TimeSpan.FromSeconds(30), s.TxTimeout);
    }

    [Theory]
    [InlineData(FirmwareKind.PacketV1)]
    [InlineData(FirmwareKind.Stock)]
    public async Task V2_only_calls_throw_not_supported_and_send_nothing(FirmwareKind kind)
    {
        await using var rig = await Rig.StartAsync(kind);
        int before = rig.Sim.ReceivedIds.Count;
        await Assert.ThrowsAsync<K5FirmwareNotSupportedException>(() => rig.Radio.GetStatusAsync(Ct));
        await Assert.ThrowsAsync<K5FirmwareNotSupportedException>(() => rig.Radio.SubscribeAsync(EventSubscription.ForTnc, Ct));
        await Assert.ThrowsAsync<K5FirmwareNotSupportedException>(() => rig.Radio.SyncClockAsync(1, Ct));
        await Assert.ThrowsAsync<K5FirmwareNotSupportedException>(() => rig.Radio.GetCountersAsync(false, Ct));
        await Assert.ThrowsAsync<K5FirmwareNotSupportedException>(() => rig.Radio.StartLevelToneRawAsync(1000, 40, TimeSpan.FromSeconds(1), Ct));
        await Assert.ThrowsAsync<K5FirmwareNotSupportedException>(() => rig.Radio.GetOverridesAsync(Ct));
        Assert.Equal(before, rig.Sim.ReceivedIds.Count);
    }

    [Fact]
    public async Task Stock_has_no_settings()
    {
        await using var rig = await Rig.StartAsync(FirmwareKind.Stock);
        await Assert.ThrowsAsync<K5FirmwareNotSupportedException>(() => rig.Radio.GetSettingsAsync(cancellationToken: Ct));
    }

    [Fact]
    public async Task Settings_read_all_and_some()
    {
        await using var rig = await Rig.StartAsync();
        var all = await rig.Radio.GetSettingsAsync(cancellationToken: Ct);
        Assert.Equal(144_800_000, all.FrequencyHz);
        Assert.Equal(0x856, all.DeviationWide!.Value.Register);
        Assert.Equal(2.8, all.DeviationWide!.Value.Kilohertz, 3);
        Assert.Equal(TimeSpan.FromMilliseconds(20), all.SerialLock);
        Assert.Equal(AgcSetting.Auto, all.Agc);
        Assert.Equal(23, all.SetIds.Count);     // 0x01 to 0x19 except the retired 0x06 and 0x07

        var some = await rig.Radio.GetSettingsAsync(false, [RadioParameterId.BusySquelchLevel, RadioParameterId.Power], Ct);
        Assert.Equal([RadioParameterId.Power, RadioParameterId.BusySquelchLevel], some.SetIds);

        var stored = await rig.Radio.GetSettingsAsync(true, cancellationToken: Ct);
        Assert.Null(stored.BusySquelchThresholds);
        Assert.Null(stored.Agc);
        Assert.Null(stored.Afc);
    }

    [Fact]
    public async Task Settings_change_atomically_with_read_back()
    {
        await using var rig = await Rig.StartAsync();
        var r = await rig.Radio.SetSettingsAsync(new RadioSettings
        {
            FrequencyHz = 145_025_000,
            Power = TxPower.High,
            DeviationWide = Deviation.FromKilohertz(3.0),
            SerialLock = TimeSpan.FromMilliseconds(30),
        }, cancellationToken: Ct);
        Assert.True(r.TxAllowed);
        Assert.True(r.Retuned);
        Assert.False(r.PersistQueued);
        Assert.Equal(145_025_000, r.Applied.FrequencyHz);
        Assert.Equal(TxPower.High, rig.Sim.LiveSettings.Power);
        Assert.Equal(Deviation.FromKilohertz(3.0).Register, rig.Sim.LiveSettings.DeviationWide!.Value.Register);
        Assert.Equal(TimeSpan.FromMilliseconds(30), rig.Radio.Firmware.SerialLock);
    }

    [Fact]
    public async Task Settings_are_validated_before_sending()
    {
        await using var rig = await Rig.StartAsync();
        int before = rig.Sim.ReceivedIds.Count;
        await Assert.ThrowsAsync<ArgumentOutOfRangeException>(() => rig.Radio.SetSettingsAsync(new RadioSettings { BusySquelchLevel = 10 }, cancellationToken: Ct));
        await Assert.ThrowsAsync<ArgumentOutOfRangeException>(() => rig.Radio.SetSettingsAsync(new RadioSettings { FrequencyHz = 145_000_005 }, cancellationToken: Ct));
        await Assert.ThrowsAsync<ArgumentOutOfRangeException>(() => rig.Radio.SetSettingsAsync(new RadioSettings { TxTimeout = TimeSpan.FromSeconds(25) }, cancellationToken: Ct));
        await Assert.ThrowsAsync<ArgumentOutOfRangeException>(() => rig.Radio.SetSettingsAsync(new RadioSettings { SerialLock = TimeSpan.FromMilliseconds(25) }, cancellationToken: Ct));
        await Assert.ThrowsAsync<ArgumentOutOfRangeException>(() => rig.Radio.SetSettingsAsync(new RadioSettings { BusyRssiOpen = new Rssi(100), BusyRssiClose = new Rssi(120) }, cancellationToken: Ct));
        await Assert.ThrowsAsync<ArgumentException>(() => rig.Radio.SetSettingsAsync(new RadioSettings { Afc = false }, SetSettingsFlags.Persist, Ct));
        Assert.Throws<ArgumentOutOfRangeException>(() => new Deviation(0xB00));
        Assert.Throws<ArgumentOutOfRangeException>(() => Deviation.FromKilohertz(40));
        Assert.Equal(before, rig.Sim.ReceivedIds.Count);
    }

    [Fact]
    public async Task Radio_rejections_are_typed()
    {
        await using var rig = await Rig.StartAsync();
        var e = await Assert.ThrowsAsync<K5CommandRejectedException>(() =>
            rig.Radio.SetSettingsAsync(new RadioSettings { FrequencyHz = 200_000_000, BusySquelchLevel = 3 }, SetSettingsFlags.RequireTxAllowed, Ct));
        Assert.Equal(K5Status.TxBand, e.Status);
        Assert.Equal(1, rig.Sim.LiveSettings.BusySquelchLevel);   // nothing changed

        var tone = await Assert.ThrowsAsync<K5CommandRejectedException>(() => rig.Radio.StartLevelToneAsync(1000, 3000, TimeSpan.FromSeconds(1), Ct));
        Assert.Equal(K5Status.Unsupported, tone.Status);
    }

    [Fact]
    public async Task There_is_no_squelch()
    {
        await using var rig = await Rig.StartAsync();
        Assert.Equal(0x03FFFF3Eu, rig.Radio.Firmware.SupportedParameters.Aggregate(0u, (m, id) => m | (1u << (int)id)));
        var e = await Assert.ThrowsAsync<K5CommandRejectedException>(() => rig.Radio.GetSettingsAsync(false, [(RadioParameterId)0x07], Ct));
        Assert.Equal(K5Status.Unsupported, e.Status);
        Assert.Equal(0x07, e.Detail);
        Assert.Contains("no squelch", e.Message);

        // The busy detector level never mutes anything; busy follows the chip's detector.
        await rig.Radio.SetSettingsAsync(new RadioSettings { BusySquelchLevel = 9 }, cancellationToken: Ct);
        Assert.Equal(Simulation.SimulatedAudioOutput.Receive, rig.Sim.AudioOutput);
        rig.Sim.StartCarrier(Rssi.FromDbm(-90));
        var s = await rig.Radio.GetStatusAsync(Ct);
        Assert.Equal(RadioState.Busy, s.State);
        Assert.True(s.Flags.HasFlag(StatusFlags.SquelchDetector));
        Assert.Equal(9, s.BusySquelchLevel);
        Assert.Equal(0xFF, s.Channel);
        rig.Sim.StopCarrier();
        Assert.Equal(RadioState.Idle, (await rig.Radio.GetStatusAsync(Ct)).State);
    }

    [Fact]
    public async Task There_is_no_mic_gain()
    {
        await using var rig = await Rig.StartAsync();
        Assert.False(rig.Radio.Firmware.Supports((RadioParameterId)0x06));
        Assert.DoesNotContain((RadioParameterId)0x06, (await rig.Radio.GetSettingsAsync(cancellationToken: Ct)).SetIds);
        var e = await Assert.ThrowsAsync<K5CommandRejectedException>(() => rig.Radio.GetSettingsAsync(true, [RadioParameterId.RxGain, (RadioParameterId)0x06], Ct));
        Assert.Equal(K5Status.Unsupported, e.Status);
        Assert.Equal(0x06, e.Detail);
        Assert.Contains("fixed at the maximum", e.Message);
    }

    [Fact]
    public async Task Level_tone_returns_to_receive_audio()
    {
        await using var rig = await Rig.StartAsync();
        await rig.Radio.StartLevelToneRawAsync(1000, 40, TimeSpan.FromSeconds(5), Ct);
        Assert.Equal(Simulation.SimulatedAudioOutput.Tone, rig.Sim.AudioOutput);
        await rig.Radio.StopLevelToneAsync(Ct);
        Assert.Equal(Simulation.SimulatedAudioOutput.Receive, rig.Sim.AudioOutput);
    }

    [Fact]
    public async Task Frequency_persists_in_the_operating_channel_block()
    {
        await using var rig = await Rig.StartAsync();
        await rig.Radio.SetSettingsAsync(new RadioSettings { FrequencyHz = 433_500_000, Power = TxPower.Mid }, SetSettingsFlags.Persist, Ct);
        byte[] e = rig.Sim.Eeprom;
        Assert.Equal(43_350_000u, System.Buffers.Binary.BinaryPrimitives.ReadUInt32LittleEndian(e.AsSpan(0x1D58)));
        Assert.Equal(1, e[0x1D5C]);
        Assert.Equal(0, e[0x1D5D]);
        var stored = await rig.Radio.GetSettingsAsync(true, [RadioParameterId.FrequencyHz, RadioParameterId.Power], Ct);
        Assert.Equal(433_500_000, stored.FrequencyHz);
        Assert.Equal(TxPower.Mid, stored.Power);
    }

    [Fact]
    public async Task A_blank_eeprom_is_signed_at_power_on_so_persist_works_at_once()
    {
        var blank = new byte[0x2000];
        Array.Fill(blank, (byte)0xFF);
        new Random(3).NextBytes(blank.AsSpan(0x1E00));
        await using var rig = await Rig.StartAsync(simOptions: new SimulatedRadioOptions { Eeprom = blank });
        Assert.Equal(2, rig.Radio.Firmware.SettingsBlockLayout);
        await rig.Radio.SetSettingsAsync(new RadioSettings { BusySquelchLevel = 5 }, SetSettingsFlags.Persist, Ct);
        Assert.Equal(5, (await rig.Radio.GetSettingsAsync(true, [RadioParameterId.BusySquelchLevel], Ct)).BusySquelchLevel);
        Assert.Equal(5, rig.Sim.Eeprom[0x1D01]);
    }

    [Fact]
    public async Task Dry_run_changes_nothing()
    {
        await using var rig = await Rig.StartAsync();
        var r = await rig.Radio.SetSettingsAsync(new RadioSettings { BusySquelchLevel = 5 }, SetSettingsFlags.DryRun, Ct);
        Assert.Equal(5, r.Applied.BusySquelchLevel);
        Assert.Equal(1, rig.Sim.LiveSettings.BusySquelchLevel);
    }

    [Fact]
    public async Task Persist_save_and_revert()
    {
        await using var rig = await Rig.StartAsync();
        var r = await rig.Radio.SetSettingsAsync(new RadioSettings { BusySquelchLevel = 4, DeviationWide = new Deviation(0x800) }, SetSettingsFlags.Persist, Ct);
        Assert.True(r.PersistQueued);
        Assert.Equal(4, rig.Sim.Eeprom[0x1D01]);
        Assert.Equal(0x00, rig.Sim.Eeprom[0x1D04]);
        Assert.Equal(0x08, rig.Sim.Eeprom[0x1D05]);

        await rig.Radio.SetSettingsAsync(new RadioSettings { BusySquelchLevel = 7, Backlight = 6 }, cancellationToken: Ct);
        var reverted = await rig.Radio.RevertSettingsAsync(Ct);
        Assert.Contains(RadioParameterId.BusySquelchLevel, reverted);
        Assert.Equal(4, rig.Sim.LiveSettings.BusySquelchLevel);

        await rig.Radio.SetSettingsAsync(new RadioSettings { BusySquelchLevel = 6 }, cancellationToken: Ct);
        var saved = await rig.Radio.SaveSettingsAsync(Ct);
        Assert.Equal([RadioParameterId.BusySquelchLevel], saved);
        Assert.Equal(6, rig.Sim.Eeprom[0x1D01]);
    }

    [Fact]
    public async Task Level_tone_raw()
    {
        await using var rig = await Rig.StartAsync();
        var r = await rig.Radio.StartLevelToneRawAsync(1000, 40, TimeSpan.FromSeconds(5), Ct);
        Assert.Equal(40, r.GainCode);
        Assert.Equal(10324, r.FrequencyWord);
        Assert.True(rig.Sim.IsToneRunning);
        await rig.Radio.StopLevelToneAsync(Ct);
        Assert.False(rig.Sim.IsToneRunning);
    }

    [Fact]
    public async Task Registers_v2()
    {
        await using var rig = await Rig.StartAsync();
        ushort[] all = await rig.Radio.ReadRegistersAsync(0, 0x80, cancellationToken: Ct);
        Assert.Equal(0x1040, all[0x40]);
        Assert.Equal(0, all[0x5F]);
        ushort[] back = await rig.Radio.WriteRegistersAsync([(0x2B, 0x1234), (0x7D, 0xE94A)], Ct);
        Assert.Equal([0x1234, 0xE94A], back);
        Assert.Equal(0xE94A, await rig.Radio.ReadRegisterAsync(0x7D, Ct));
        Assert.Equal(3, rig.Sim.ReceivedIds.Count(i => i == 0x5008));   // two for the dump, one single read
        Assert.DoesNotContain((ushort)0x0602, rig.Sim.ReceivedIds);      // v2 writes go through REG_WRITE only
    }

    [Fact]
    public async Task V2_release_build_ignores_legacy_register_write_but_starts_the_lock()
    {
        var time = new Microsoft.Extensions.Time.Testing.FakeTimeProvider();
        using var sim = new SimulatedRadio(new SimulatedRadioOptions { TimeProvider = time });
        time.Advance(TimeSpan.FromSeconds(1));
        Assert.Equal(TimeSpan.Zero, sim.LockRemaining);
        await sim.HostStream.WriteAsync(Protocol.K5FrameCodec.EncodePayload(Protocol.K5FrameCodec.BuildPayload(0x0602, [0x2B, 0x34, 0x12]), true), Ct);
        Assert.Equal(0x102B, sim.Registers[0x2B]);                       // not written
        Assert.Equal(TimeSpan.FromMilliseconds(20), sim.LockRemaining);  // but the frame counts
        Assert.Contains((ushort)0x0602, sim.ReceivedIds);
        Assert.Empty(sim.Violations);
    }

    [Fact]
    public async Task V2_bench_build_has_legacy_register_write()
    {
        await using var rig = await Rig.StartAsync(simOptions: new SimulatedRadioOptions { RawRegisterWrite = true });
        Assert.True(rig.Radio.Firmware.Capabilities.HasFlag(RadioCapabilities.RawRegisterWrite));
        await rig.Sim.HostStream.WriteAsync(Protocol.K5FrameCodec.EncodePayload(Protocol.K5FrameCodec.BuildPayload(0x0602, [0x2B, 0x34, 0x12]), true), Ct);
        Assert.Equal(0x1234, rig.Sim.Registers[0x2B]);

        await using var release = await Rig.StartAsync();
        Assert.False(release.Radio.Firmware.Capabilities.HasFlag(RadioCapabilities.RawRegisterWrite));
    }

    [Fact]
    public async Task Registers_v1_use_the_legacy_commands()
    {
        await using var rig = await Rig.StartAsync(FirmwareKind.PacketV1);
        ushort[] some = await rig.Radio.ReadRegistersAsync(0x5E, 3, cancellationToken: Ct);
        Assert.Equal([0x105E, 0, 0x1060], some);
        ushort[] back = await rig.Radio.WriteRegistersAsync([(0x2B, 0x4321)], Ct);
        Assert.Equal([0x4321], back);
        Assert.Contains((ushort)0x0602, rig.Sim.ReceivedIds);
    }

    [Fact]
    public async Task Overrides_add_list_clear_in_ram_only()
    {
        await using var rig = await Rig.StartAsync();
        var t = await rig.Radio.AddOverridesAsync([new RegisterOverride(OverridePhase.Rx, 0x2B, 0xFFF8, 0)], TimeSpan.FromSeconds(60), null, Ct);
        Assert.Single(t.Ram);
        Assert.Equal(TimeSpan.FromSeconds(60), t.ExpiresIn);
        Assert.Null(t.KeyUpsLeft);
        Assert.Equal(0x1028, rig.Sim.Registers[0x2B]);   // RX-phase applied at once: 0x102B & 0xFFF8
        Assert.True((await rig.Radio.GetStatusAsync(Ct)).Flags2.HasFlag(StatusFlags2.RamOverridesActive));

        var listed = await rig.Radio.GetOverridesAsync(Ct);
        Assert.Single(listed.Ram);
        var cleared = await rig.Radio.ClearOverridesAsync(Ct);
        Assert.Empty(cleared.Ram);
        Assert.Null(cleared.ExpiresIn);
        Assert.All(rig.Sim.Eeprom[0x1D18..0x1D50], b => Assert.Equal(0xFF, b));   // nothing stored (0x1D10 is the signature)
    }

    [Fact]
    public async Task Counters()
    {
        await using var rig = await Rig.StartAsync();
        var c = await rig.Radio.GetCountersAsync(cancellationToken: Ct);
        Assert.Equal(14, c.Raw.Count);
        Assert.True(c.FramesAccepted >= 2);
        await rig.Radio.GetCountersAsync(clear: true, Ct);
        var after = await rig.Radio.GetCountersAsync(cancellationToken: Ct);
        Assert.Equal(1u, after.FramesAccepted);
    }

    [Theory]
    [InlineData(FirmwareKind.PacketV2)]
    [InlineData(FirmwareKind.PacketV1)]
    [InlineData(FirmwareKind.Stock)]
    public async Task Rssi_and_battery(FirmwareKind kind)
    {
        await using var rig = await Rig.StartAsync(kind);
        rig.Sim.StartCarrier(Rssi.FromDbm(-90), 12, 3);
        var r = await rig.Radio.ReadRssiAsync(Ct);
        Assert.Equal(-90, r.Rssi.Dbm, 1);
        Assert.Equal(12, r.Noise);
        var b = await rig.Radio.ReadBatteryAsync(Ct);
        Assert.NotNull(b.Volts);
        Assert.Equal(7.8, b.Volts!.Value, 1);
        Assert.Equal(kind == FirmwareKind.PacketV2, b.Level is not null);
    }

    [Theory]
    [InlineData(FirmwareKind.PacketV2)]
    [InlineData(FirmwareKind.PacketV1)]
    [InlineData(FirmwareKind.Stock)]
    public async Task Eeprom_read_matches(FirmwareKind kind)
    {
        await using var rig = await Rig.StartAsync(kind);
        byte[] data = await rig.Radio.ReadEepromAsync(0x1D00, 0x300, Ct);
        Assert.Equal(rig.Sim.Eeprom[0x1D00..0x2000], data);
    }

    [Fact]
    public async Task V1_settings_come_from_the_eeprom_block()
    {
        await using var rig = await Rig.StartAsync(FirmwareKind.PacketV1);
        var s = await rig.Radio.GetSettingsAsync(cancellationToken: Ct);
        Assert.Equal(1, s.BusySquelchLevel);
        Assert.Equal(TimeSpan.FromSeconds(30), s.TxTimeout);
        Assert.Equal(0x856, s.DeviationWide!.Value.Register);
        Assert.Equal(0x756, s.DeviationNarrow!.Value.Register);
        Assert.Null(s.RxGain);      // factory calibration default, not decoded
        Assert.Equal(TimeSpan.FromMilliseconds(5), s.PttPressDebounce);
        Assert.Null(s.FrequencyHz);
    }

    [Fact]
    public async Task V1_settings_write_needs_a_backup_and_applies_after_quiet()
    {
        await using var rig = await Rig.StartAsync(FirmwareKind.PacketV1);
        await Assert.ThrowsAsync<K5SafetyException>(() => rig.Radio.SetSettingsAsync(new RadioSettings { BusySquelchLevel = 3 }, cancellationToken: Ct));
        await Assert.ThrowsAsync<K5FirmwareNotSupportedException>(() => rig.Radio.SetSettingsAsync(new RadioSettings { FrequencyHz = 145_000_000 }, cancellationToken: Ct));

        await rig.Radio.BackupEepromAsync(cancellationToken: Ct);
        var r = await rig.Radio.SetSettingsAsync(new RadioSettings { BusySquelchLevel = 3, DeviationWide = new Deviation(0x762), PaBiasDelay = TimeSpan.FromMilliseconds(4) }, cancellationToken: Ct);
        Assert.Equal(TimeSpan.FromSeconds(1.5), r.AppliesAfterQuiet);
        Assert.Equal(3, r.Applied.BusySquelchLevel);
        Assert.Equal(3, rig.Sim.Eeprom[0x1D01]);
        Assert.Equal(0x62, rig.Sim.Eeprom[0x1D04]);
        Assert.Equal(4, rig.Sim.Eeprom[0x1D53]);
        Assert.Equal(1, rig.Sim.LiveSettings.BusySquelchLevel);    // not yet: the reload waits for quiet
        await Rig.Until(() => rig.Sim.LiveSettings.BusySquelchLevel == 3, 4000);
    }

    [Fact]
    public async Task Timeouts_retry_once_then_throw()
    {
        await using var rig = await Rig.StartAsync();
        rig.Sim.DropReplies(1);
        var s = await rig.Radio.GetStatusAsync(Ct);      // first reply lost, retry answered
        Assert.Equal(144_800_000, s.FrequencyHz);
        rig.Sim.DropReplies(2);
        await Assert.ThrowsAsync<K5TimeoutException>(() => rig.Radio.GetStatusAsync(Ct));
    }

    [Fact]
    public async Task Register_writes_are_not_retried()
    {
        await using var rig = await Rig.StartAsync();
        rig.Sim.DropReplies(1);
        int before = rig.Sim.ReceivedIds.Count(i => i == 0x5009);
        await Assert.ThrowsAsync<K5TimeoutException>(() => rig.Radio.WriteRegistersAsync([(0x2B, 1)], Ct));
        Assert.Equal(before + 1, rig.Sim.ReceivedIds.Count(i => i == 0x5009));
    }

    [Fact]
    public async Task Reboot_is_noticed_and_the_subscription_restored()
    {
        await using var rig = await Rig.StartAsync();
        await rig.Radio.SubscribeAsync(EventSubscription.ForTnc, Ct);
        int rebooted = 0;
        rig.Radio.Rebooted += (_, _) => rebooted++;
        await rig.Radio.RebootAsync(Ct);
        await Rig.Until(() => rebooted == 1);
        await Rig.Until(() => rig.Sim.SubscribedEvents == (RadioEvents.Tnc | RadioEvents.Heartbeat), 5000);
    }

    [Fact]
    public async Task Link_loss_fails_pending_and_later_calls()
    {
        await using var rig = await Rig.StartAsync();
        rig.Sim.Dispose();
        await Rig.Until(() => rig.Radio.ChannelBusy is null);
        await Assert.ThrowsAnyAsync<Exception>(() => rig.Radio.GetStatusAsync(Ct));
    }

    /// <summary>
    /// What a radio that ran another firmware carries: its channels and settings everywhere, the CE
    /// TX plan with the 350 MHz band off at 0x0F40, and DTMF contacts over 0x1D00, starting 0x01.
    /// </summary>
    private static byte[] ForeignEeprom()
    {
        var e = new byte[0x2000];
        for (int i = 0; i < e.Length; i++)
        {
            e[i] = (byte)(i * 7 + 3);
        }

        e[0x0F40] = 2;
        e[0x0F45] = 0;
        e[0x1D00] = 0x01;
        return e;
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task First_power_on_over_another_firmware_or_v1_0_0_is_factory_fresh(bool v100)
    {
        byte[] before = ForeignEeprom();
        if (v100)
        {
            // v1.0.0's own layout: marker 1 at 0x1D00, settings, an operating channel, no signature
            Array.Fill(before, (byte)0xFF, 0x1D00, 0x70);
            byte[] settings = [0x01, 0x07, 0x05, 0xFF, 0x00, 0x09, 0x00, 0x08, 0x1E, 0x09, 0x07, 0xFF, 0x01, 0xFF, 0xFF, 0xFF];
            settings.CopyTo(before, 0x1D00);
            System.Buffers.Binary.BinaryPrimitives.WriteUInt32LittleEndian(before.AsSpan(0x1D58), 43_350_000);
            before[0x1D5C] = 2;
        }

        await using var rig = await Rig.StartAsync(simOptions: new SimulatedRadioOptions { Eeprom = before });
        byte[] e = rig.Sim.Eeprom;
        Assert.Equal([.. "PKFW"u8, 2, 0xFF, 0xFF, 0xFF], e[0x1D10..0x1D18]);
        Assert.Equal(0xFF, e[0x1D00]);                                  // no v1.0.0 marker
        Assert.All(e[0x1D18..0x1D50], b => Assert.Equal(0xFF, b));       // reserved: blank
        Assert.Equal(before[..0x1D00], e[..0x1D00]);                    // nothing outside the family written
        Assert.Equal(before[0x1D70..], e[0x1D70..]);
        Assert.Equal(2, rig.Radio.Firmware.SettingsBlockLayout);
        Assert.Equal(1, rig.Radio.Firmware.V2BlockLayout);
        int changed = Enumerable.Range(0, 14).Count(i => !before.AsSpan(0x1D00 + 8 * i, 8).SequenceEqual(e.AsSpan(0x1D00 + 8 * i, 8)));
        Assert.Equal(v100 ? 7 : 14, changed);                           // blocks already holding the defaults are left alone
        Assert.Equal((uint)changed, (await rig.Radio.GetCountersAsync(cancellationToken: Ct)).EepromBlocksWritten);

        var stored = await rig.Radio.GetSettingsAsync(true, cancellationToken: Ct);
        Assert.Equal(144_800_000, stored.FrequencyHz);
        Assert.Equal(TxPower.Low, stored.Power);
        Assert.Equal(1, stored.BusySquelchLevel);
        Assert.Equal(TimeSpan.FromSeconds(30), stored.TxTimeout);
        Assert.Equal(0x856, stored.DeviationWide!.Value.Register);
        Assert.Equal(TimeSpan.FromMilliseconds(20), stored.SerialLock);
        Assert.False((await rig.Radio.GetStatusAsync(Ct)).Flags2.HasFlag(StatusFlags2.UnsavedChanges));

        // the fixed TX policy, whatever 0x0F40 said; 380 MHz receivable (350EN off is not read)
        foreach (var (hz, tx) in new[] { (146_900_000L, true), (435_000_000L, true), (50_000_000L, false), (200_000_000L, false), (380_000_000L, false), (500_000_000L, false) })
        {
            var r = await rig.Radio.SetSettingsAsync(new RadioSettings { FrequencyHz = hz }, cancellationToken: Ct);
            Assert.Equal(tx, r.TxAllowed);
            Assert.Equal(tx, FirmwareInfo.IsFixedTxAllowed(hz));
            if (!tx)
            {
                var x = await Assert.ThrowsAsync<K5CommandRejectedException>(() =>
                    rig.Radio.SetSettingsAsync(new RadioSettings { FrequencyHz = hz }, SetSettingsFlags.RequireTxAllowed, Ct));
                Assert.Equal(K5Status.TxBand, x.Status);
            }
        }

        // user changes survive the next power-on, which writes nothing
        await rig.Radio.SetSettingsAsync(new RadioSettings { FrequencyHz = 433_500_000, Power = TxPower.High, BusySquelchLevel = 4 }, SetSettingsFlags.Persist, Ct);
        await Rig.Until(() => rig.Sim.Eeprom[0x1D01] == 4 && rig.Sim.Eeprom[0x1D5C] == 2);
        await using var again = await Rig.StartAsync(simOptions: new SimulatedRadioOptions { Eeprom = rig.Sim.Eeprom });
        Assert.Equal(0u, (await again.Radio.GetCountersAsync(cancellationToken: Ct)).EepromBlocksWritten);
        var live = await again.Radio.GetSettingsAsync(cancellationToken: Ct);
        Assert.Equal(433_500_000, live.FrequencyHz);
        Assert.Equal(TxPower.High, live.Power);
        Assert.Equal(4, live.BusySquelchLevel);
        Assert.Empty(rig.Sim.Violations);
    }
}
