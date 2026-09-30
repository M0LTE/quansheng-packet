using System.Buffers.Binary;
using M0LTE.Uvk5.Bootloader;
using M0LTE.Uvk5.Protocol;
using M0LTE.Uvk5.Simulation;

namespace M0LTE.Uvk5.Tests;

public class BootloaderTests
{
    // K5TOOL Packet2FlashWriteReq: vendor updater, first and last block of a 0xE6-block image.
    private static readonly byte[] FlashFirst = Convert.FromHexString(
        "19050c01945d6a2c0000e6000001000088130020d5000000d9000000db000000000000000000000000000000"
        + "00000000000000000000000000000000dd0000000000000000000000df00000025c40000e3000000e5000000"
        + "e7000000e9000000eb000000ed000000ef000000f1000000f3000000f5000000f7000000f9000000fb000000"
        + "fd000000ff00000001010000030100000501000007010000090100000b0100000d0100000f01000011010000"
        + "130100001501000017010000190100001b0100001d0100001f010000210100000348854600f058fb00480047"
        + "edd100008813002013480047fee7fee7fee7fee7fee7fee7fee7fee7fee7fee7fee7fee7fee7fee7fee7fee7"
        + "fee7fee7fee7fee7");

    private static readonly byte[] FlashLast = Convert.FromHexString(
        "19050c01272f5d07e500e600f4000000fff74eff2e4948602c480830fff748ff2b49886029480c30fff742"
        + "ff2849c860f920c000fff736ff2549403988632548fff730ff22494039c86322482030fff729ff214908623f"
        + "204001fff723ff1e494862fb20c000fff71dff1b49086319481030fff717ff184948637d200001fff711ff15"
        + "49886313480830fff70bff1249c863f720c000fff705ff104908600d480838fff7fffe044601208007806801"
        + "214906084349018860e0b209490863a001800d48630020fff713ff10bd18f0000080000040c40700000008"
        + "004078040020c0a00b40e6006cdc023093e8034201021c014201021801021001d2ff53ffff04ebff00ff00"
        + "0000000000000000000000");

    internal static byte[] FakeImage(int size = 0x3456, int seed = 1)
    {
        var rnd = new Random(seed);
        var b = new byte[size];
        rnd.NextBytes(b);
        BinaryPrimitives.WriteUInt32LittleEndian(b, 0x20004000);
        BinaryPrimitives.WriteUInt32LittleEndian(b.AsSpan(4), 0xD5);
        return b;
    }

    [Fact]
    public void Write_frame_matches_vendor_first_block()
    {
        uint seq = BinaryPrimitives.ReadUInt32LittleEndian(FlashFirst.AsSpan(4));
        byte[] body = K5Bootloader.MakeWriteBody(seq, 0, 0xE6, FlashFirst.AsSpan(16));
        Assert.Equal(FlashFirst, K5FrameCodec.BuildPayload(0x0519, body));
    }

    [Fact]
    public void Write_frame_matches_vendor_last_block()
    {
        uint seq = BinaryPrimitives.ReadUInt32LittleEndian(FlashLast.AsSpan(4));
        byte[] body = K5Bootloader.MakeWriteBody(seq, 0xE5, 0xE6, FlashLast.AsSpan(16, 0xF4));
        Assert.Equal(FlashLast, K5FrameCodec.BuildPayload(0x0519, body));
    }

    [Fact]
    public void Write_frame_refuses_the_bootloader_area() =>
        Assert.Throws<K5SafetyException>(() => K5Bootloader.MakeWriteBody(1, 0xF0, 0xF1, new byte[256]));

    [Fact]
    public void Pack_unpack_round_trip()
    {
        byte[] raw = FakeImage();
        byte[] packed = FirmwareImage.Pack(raw, "*EGZUMER test");
        Assert.Equal(raw.Length + 18, packed.Length);
        var (back, ver) = FirmwareImage.Unpack(packed);
        Assert.Equal(raw, back);
        Assert.Equal("*EGZUMER test", System.Text.Encoding.ASCII.GetString(ver).TrimEnd('\0'));
    }

    [Fact]
    public void Load_detects_packed_and_raw()
    {
        byte[] raw = FakeImage();
        var r = FirmwareImage.Load(raw);
        var p = FirmwareImage.Load(FirmwareImage.Pack(raw, "*X"));
        Assert.False(r.WasPacked);
        Assert.Equal("*", r.Version);
        Assert.True(p.WasPacked);
        Assert.Equal("*X", p.Version);
        Assert.Equal(raw, p.Raw.ToArray());
    }

    [Fact]
    public void Packed_file_with_bad_crc_is_not_mistaken_for_raw()
    {
        byte[] bad = FirmwareImage.Pack(FakeImage(), "*X");
        bad[^1] ^= 0xFF;
        Assert.Throws<InvalidDataException>(() => FirmwareImage.Load(bad));
    }

    [Fact]
    public void Oversized_image_refused()
    {
        Assert.Throws<K5SafetyException>(() => new FirmwareImage(FakeImage(FirmwareImage.FlashLimit + 1), "*").Validate());
        new FirmwareImage(FakeImage(FirmwareImage.FlashLimit), "*").Validate();
    }

    [Fact]
    public void Compatibility_rules()
    {
        var img = new FirmwareImage(FakeImage(), "*EGZUMER v1");
        var ok = new BootloaderBeacon(0x0518, "2.00.06", "");
        string[] allowed = ["2.00.06"];
        K5Bootloader.CheckCompatibility(ok, img, allowed);
        Assert.Throws<K5SafetyException>(() => K5Bootloader.CheckCompatibility(new BootloaderBeacon(0x057A, "5.00.01", ""), img, allowed));
        Assert.Throws<K5SafetyException>(() => K5Bootloader.CheckCompatibility(ok with { Version = "2.00.99" }, img, allowed));
        K5Bootloader.CheckCompatibility(ok with { Version = "2.00.99" }, img, ["2.00.99"]);
        Assert.Throws<K5SafetyException>(() => K5Bootloader.CheckCompatibility(ok, new FirmwareImage(FakeImage(), "3.01.23"), allowed));
        Assert.Throws<K5SafetyException>(() => K5Bootloader.CheckCompatibility(ok with { Version = null }, img, allowed));
    }

    [Fact]
    public void Beacon_parses_version_and_chip()
    {
        var f = Assert.Single(new K5FrameDecoder().Feed(CodecTests.RealBeaconWire));
        var b = K5Bootloader.ParseBeacon(f);
        Assert.Equal("2.00.06", b.Version);
        Assert.False(b.IsVersion5);
    }

    [Fact]
    public async Task Dry_run_listens_and_sends_nothing()
    {
        using var sim = new SimulatedBootloader();
        await using var bl = new K5Bootloader(sim.HostStream);
        var result = await bl.FlashAsync(new FirmwareImage(FakeImage(), "*PKTFW test"), cancellationToken: TestContext.Current.CancellationToken);
        Assert.False(result.Flashed);
        Assert.Equal("2.00.06", result.Beacon.Version);
        Assert.Null(sim.FlashVersion);
        Assert.Empty(sim.Blocks);
    }

    [Fact]
    public async Task Really_flash_writes_the_image()
    {
        using var sim = new SimulatedBootloader();
        await using var bl = new K5Bootloader(sim.HostStream);
        byte[] raw = FakeImage(0x1234);
        var progress = new List<FlashProgress>();
        var audit = new List<K5AuditEntry>();
        var result = await bl.FlashAsync(new FirmwareImage(raw, "*PKTFW test"),
            new FlashOptions { ReallyFlash = true, SequenceId = 0x11223344, Audit = audit.Add }, new SyncProgress<FlashProgress>(progress.Add), TestContext.Current.CancellationToken);
        Assert.True(result.Flashed);
        Assert.True(sim.Finished);
        Assert.Equal("*PKTFW test", sim.FlashVersion);
        Assert.Equal(raw, sim.Flash[..raw.Length]);
        Assert.Equal(0x13, sim.Blocks.Count);
        Assert.All(sim.Blocks, b => Assert.Equal(0x11223344u, b.Seq));
        Assert.Empty(sim.Violations);
        Assert.Equal(0x13, progress.Last().BlocksDone);
        Assert.Equal("ok", audit.Last().Outcome);
    }

    [Fact]
    public async Task Stray_beacons_during_block_writes_are_tolerated()
    {
        using var sim = new SimulatedBootloader(new SimulatedBootloaderOptions { KeepBeaconing = true, BeaconInterval = TimeSpan.FromMilliseconds(5) });
        await using var bl = new K5Bootloader(sim.HostStream);
        var result = await bl.FlashAsync(new FirmwareImage(FakeImage(0x800), "*T"), new FlashOptions { ReallyFlash = true }, cancellationToken: TestContext.Current.CancellationToken);
        Assert.True(result.Flashed);
        Assert.True(sim.Finished);
    }

    [Fact]
    public async Task Delayed_version_reply_is_handled()
    {
        using var sim = new SimulatedBootloader(new SimulatedBootloaderOptions { VersionReplyDelay = TimeSpan.FromMilliseconds(80), BeaconInterval = TimeSpan.FromMilliseconds(20) });
        await using var bl = new K5Bootloader(sim.HostStream);
        var result = await bl.FlashAsync(new FirmwareImage(FakeImage(0x800), "*T"), new FlashOptions { ReallyFlash = true }, cancellationToken: TestContext.Current.CancellationToken);
        Assert.True(result.Flashed);
    }

    [Fact]
    public async Task Refused_block_is_reported_although_the_ack_names_chunk_0()
    {
        using var sim = new SimulatedBootloader(new SimulatedBootloaderOptions { RefuseBlock = 3 });
        await using var bl = new K5Bootloader(sim.HostStream);
        var e = await Assert.ThrowsAsync<K5ProtocolException>(() => bl.FlashAsync(new FirmwareImage(FakeImage(0x800), "*T"), new FlashOptions { ReallyFlash = true }, cancellationToken: TestContext.Current.CancellationToken));
        Assert.Contains("refused block 3", e.Message);
        Assert.Contains("names chunk 0", e.Message);
    }

    [Fact]
    public async Task Lost_ack_is_resent()
    {
        using var sim = new SimulatedBootloader(new SimulatedBootloaderOptions { DropAckForBlock = 2 });
        await using var bl = new K5Bootloader(sim.HostStream);
        var result = await bl.FlashAsync(new FirmwareImage(FakeImage(0x800), "*T"), new FlashOptions { ReallyFlash = true, AckTimeout = TimeSpan.FromMilliseconds(200) }, cancellationToken: TestContext.Current.CancellationToken);
        Assert.True(result.Flashed);
        Assert.Equal(2, sim.Blocks.Count(b => b.Chunk == 2));
    }

    [Fact]
    public async Task Bootloader_5_is_refused_before_anything_is_sent()
    {
        using var sim = new SimulatedBootloader(new SimulatedBootloaderOptions { Version5 = true });
        await using var bl = new K5Bootloader(sim.HostStream);
        await Assert.ThrowsAsync<K5SafetyException>(() => bl.FlashAsync(new FirmwareImage(FakeImage(0x800), "*T"), new FlashOptions { ReallyFlash = true }, cancellationToken: TestContext.Current.CancellationToken));
        Assert.Null(sim.FlashVersion);
        Assert.Empty(sim.Blocks);
    }
}

/// <summary>An IProgress that reports synchronously (Progress&lt;T&gt; posts to the thread pool).</summary>
internal sealed class SyncProgress<T>(Action<T> report) : IProgress<T>
{
    public void Report(T value) => report(value);
}
