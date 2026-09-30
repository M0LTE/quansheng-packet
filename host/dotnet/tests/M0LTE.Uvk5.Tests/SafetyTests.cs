using M0LTE.Uvk5.Simulation;

namespace M0LTE.Uvk5.Tests;

/// <summary>The calibration guard, the backup rule and the register refusal list.</summary>
public class SafetyTests
{
    [Theory]
    [InlineData(0x1E00, 8)]
    [InlineData(0x1DF8, 16)]
    [InlineData(0x1FF8, 8)]
    [InlineData(0x0000, 0x2000)]
    [InlineData(0x1D00, 0x108)]
    [InlineData(0x2000, 8)]
    [InlineData(-8, 8)]
    public void Calibration_and_out_of_range_writes_refused(int addr, int length) =>
        Assert.Throws<K5SafetyException>(() => K5Safety.CheckEepromWrite(addr, length));

    [Theory]
    [InlineData(0x0000, 8)]
    [InlineData(0x1DF8, 8)]
    [InlineData(0x0E70, 0x40)]
    [InlineData(0, 0x1E00)]
    public void Allowed_writes(int addr, int length) => K5Safety.CheckEepromWrite(addr, length);

    [Theory]
    [InlineData(0x0E71, 8)]
    [InlineData(0x0E70, 7)]
    [InlineData(0x0E70, 12)]
    public void Unaligned_writes_refused(int addr, int length) =>
        Assert.Throws<K5SafetyException>(() => K5Safety.CheckEepromWrite(addr, length));

    [Theory]
    [InlineData(0x00)]
    [InlineData(0x30)]
    [InlineData(0x33)]
    [InlineData(0x36)]
    [InlineData(0x37)]
    [InlineData(0x38)]
    [InlineData(0x39)]
    [InlineData(0x3B)]
    [InlineData(0x3C)]
    [InlineData(0x80)]
    [InlineData(0xFF)]
    public void Refused_registers(int reg)
    {
        Assert.True(K5Safety.IsRegisterRefused(reg));
        Assert.Throws<K5SafetyException>(() => K5Safety.CheckRegisterWrite(reg));
    }

    [Theory]
    [InlineData(0x40)]
    [InlineData(0x7D)]
    [InlineData(0x2B)]
    [InlineData(0x7F)]
    public void Allowed_registers(int reg) => Assert.False(K5Safety.IsRegisterRefused(reg));

    [Fact]
    public void Blank_backups_refused()
    {
        Assert.Throws<K5SafetyException>(() => EepromBackup.FromImage(new byte[0x2000]));
        var ff = new byte[0x2000];
        Array.Fill(ff, (byte)0xFF);
        Assert.Throws<K5SafetyException>(() => EepromBackup.FromImage(ff));
        var blankCal = new byte[0x2000];
        Array.Fill(blankCal, (byte)0xFF);
        blankCal[5] = 1;
        Assert.Throws<K5SafetyException>(() => EepromBackup.FromImage(blankCal));
        Assert.Throws<K5SafetyException>(() => EepromBackup.FromImage(new byte[100]));
    }

    [Theory]
    [InlineData(FirmwareKind.PacketV2)]
    [InlineData(FirmwareKind.PacketV1)]
    [InlineData(FirmwareKind.Stock)]
    public async Task Eeprom_write_without_a_backup_sends_nothing(FirmwareKind kind)
    {
        await using var rig = await Rig.StartAsync(kind);
        int before = rig.Sim.ReceivedIds.Count;
        await Assert.ThrowsAsync<K5SafetyException>(() => rig.Radio.WriteEepromAsync(0x0100, new byte[8], TestContext.Current.CancellationToken));
        Assert.Equal(before, rig.Sim.ReceivedIds.Count);
    }

    [Theory]
    [InlineData(FirmwareKind.PacketV2)]
    [InlineData(FirmwareKind.Stock)]
    public async Task Calibration_write_is_refused_even_with_a_backup(FirmwareKind kind)
    {
        // Stock firmware would write the calibration area; the library must never ask it to.
        await using var rig = await Rig.StartAsync(kind);
        await rig.Radio.BackupEepromAsync(cancellationToken: TestContext.Current.CancellationToken);
        int before = rig.Sim.ReceivedIds.Count;
        await Assert.ThrowsAsync<K5SafetyException>(() => rig.Radio.WriteEepromAsync(0x1DF8, new byte[16], TestContext.Current.CancellationToken));
        await Assert.ThrowsAsync<K5SafetyException>(() => rig.Radio.WriteEepromAsync(0x1E00, new byte[8], TestContext.Current.CancellationToken));
        Assert.Equal(before, rig.Sim.ReceivedIds.Count);
        Assert.Empty(rig.Sim.Violations);
    }

    [Fact]
    public async Task Backup_of_another_radio_is_refused()
    {
        await using var other = await Rig.StartAsync(simOptions: new SimulatedRadioOptions { CalibrationSeed = 99 });
        var foreign = await other.Radio.BackupEepromAsync(cancellationToken: TestContext.Current.CancellationToken);

        await using var rig = await Rig.StartAsync();
        var e = await Assert.ThrowsAsync<K5SafetyException>(() => rig.Radio.AuthorizeEepromWritesAsync(foreign, TestContext.Current.CancellationToken));
        Assert.Contains("another radio", e.Message);
        await Assert.ThrowsAsync<K5SafetyException>(() => rig.Radio.WriteEepromAsync(0x0100, new byte[8], TestContext.Current.CancellationToken));
    }

    [Fact]
    public async Task Backup_round_trips_through_k5py_file_format()
    {
        await using var rig = await Rig.StartAsync();
        var backup = await rig.Radio.BackupEepromAsync(cancellationToken: TestContext.Current.CancellationToken);
        Assert.Equal(rig.Sim.Eeprom, backup.Data.ToArray());

        string dir = Path.Combine(Path.GetTempPath(), "uvk5-" + Guid.NewGuid().ToString("N"));
        string path = Path.Combine(dir, "k5.bin");
        try
        {
            await backup.SaveAsync(path, TestContext.Current.CancellationToken);
            await Assert.ThrowsAsync<IOException>(() => backup.SaveAsync(path, TestContext.Current.CancellationToken));
            string sha = await File.ReadAllTextAsync(path + ".sha256", TestContext.Current.CancellationToken);
            Assert.Equal($"{backup.Sha256}  k5.bin\n", sha);

            var loaded = await EepromBackup.LoadAsync(path, TestContext.Current.CancellationToken);
            Assert.Equal(backup.Sha256, loaded.Sha256);
            Assert.Equal(rig.Sim.Version, loaded.FirmwareVersion);

            // A fresh client authorises writes with the loaded backup and writes.
            await rig.Radio.AuthorizeEepromWritesAsync(loaded, TestContext.Current.CancellationToken);
            await rig.Radio.WriteEepromAsync(0x0E70, Enumerable.Range(1, 16).Select(i => (byte)i).ToArray(), TestContext.Current.CancellationToken);
            Assert.Equal(Enumerable.Range(1, 16).Select(i => (byte)i), rig.Sim.Eeprom[0x0E70..0x0E80]);

            // A tampered file is refused.
            byte[] data = await File.ReadAllBytesAsync(path, TestContext.Current.CancellationToken);
            data[10] ^= 1;
            await File.WriteAllBytesAsync(path, data, TestContext.Current.CancellationToken);
            await Assert.ThrowsAsync<K5SafetyException>(() => EepromBackup.LoadAsync(path, TestContext.Current.CancellationToken));
        }
        finally
        {
            Directory.Delete(dir, recursive: true);
        }
    }

    [Fact]
    public async Task Eeprom_writes_are_audited()
    {
        var audit = new List<K5AuditEntry>();
        await using var rig = await Rig.StartAsync(options: new K5RadioOptions { Audit = audit.Add });
        await rig.Radio.BackupEepromAsync(cancellationToken: TestContext.Current.CancellationToken);
        await rig.Radio.WriteEepromAsync(0x0E70, new byte[0x48], TestContext.Current.CancellationToken);
        Assert.Equal(2, audit.Count(a => a.Kind == K5AuditKind.EepromWrite));
        Assert.All(audit, a => Assert.Equal("ok", a.Outcome));
        Assert.Equal("0x0E70", audit[0].Details["addr"]);
    }

    [Theory]
    [InlineData(FirmwareKind.PacketV2)]
    [InlineData(FirmwareKind.PacketV1)]
    public async Task Refused_register_writes_send_nothing(FirmwareKind kind)
    {
        await using var rig = await Rig.StartAsync(kind);
        int before = rig.Sim.ReceivedIds.Count;
        await Assert.ThrowsAsync<K5SafetyException>(() => rig.Radio.WriteRegistersAsync([(0x7D, 0xE940), (0x36, 0)], TestContext.Current.CancellationToken));
        Assert.Equal(before, rig.Sim.ReceivedIds.Count);
    }

    [Fact]
    public async Task Refused_override_registers_send_nothing()
    {
        await using var rig = await Rig.StartAsync();
        int before = rig.Sim.ReceivedIds.Count;
        await Assert.ThrowsAsync<K5SafetyException>(() => rig.Radio.AddOverridesAsync([new RegisterOverride(OverridePhase.Tx, 0x30, 0, 0)], TimeSpan.FromSeconds(10), null, TestContext.Current.CancellationToken));
        Assert.Equal(before, rig.Sim.ReceivedIds.Count);
    }
}
