using System.Buffers.Binary;
using M0LTE.Uvk5.Protocol;

namespace M0LTE.Uvk5.Tests;

/// <summary>
/// Framing, CRC and obfuscation against frames captured by other people's tools (the same golden
/// vectors as tools/k5/test_protocol.py in uvk5-packet-bench).
/// </summary>
public class CodecTests
{
    // amnemonic/Quansheng_UV-K5_Firmware docs/communication.md, "Get firmware version"
    internal static readonly byte[] HelloTx = Convert.FromHexString("abcd0800026910e6b1dd58242bdfdcba");
    internal static readonly byte[] HelloRx = Convert.FromHexString(
        "abcd2800036930e645a452720f05e46e2130e9802a8e14e62e910d4066c929359d488b9884eba7b453e58337decadcba");

    // Captured from the bench K5's bootloader 2.00.06 on 28 September 2026: CRC field raw FF FF.
    internal static readonly byte[] RealBeaconWire = Convert.FromHexString(
        "abcd24000e6934e62f930e422d669f735e401697696c9be61cbf3d700f05e3402709e980166c14c6ffffdcba");

    [Fact]
    public void Crc_check_value() => Assert.Equal(0x31C3, K5FrameCodec.Crc16("123456789"u8));

    [Fact]
    public void Hello_frame_matches_capture() =>
        Assert.Equal(HelloTx, K5FrameCodec.Encode(0x0514, Convert.FromHexString("9f4c5564"), obfuscate: true));

    [Fact]
    public void Decodes_captured_hello_reply()
    {
        var frames = new K5FrameDecoder().Feed(HelloRx);
        var f = Assert.Single(frames);
        Assert.Equal(0x0515, f.Id);
        Assert.Equal(CrcStatus.Absent, f.Crc);
        Assert.Equal("k5_2.01.23", System.Text.Encoding.ASCII.GetString(f.Body.Span[..16]).Split('\0')[0]);
    }

    [Fact]
    public void Captured_hello_request_has_valid_crc()
    {
        var f = Assert.Single(new K5FrameDecoder().Feed(HelloTx));
        Assert.Equal(CrcStatus.Valid, f.Crc);
        Assert.Equal(0x0514, f.Id);
    }

    [Fact]
    public void Plain_frames_round_trip_with_id_visible()
    {
        byte[] frame = K5FrameCodec.Encode(0x0601, [0x7D], obfuscate: false);
        Assert.Equal(new byte[] { 0x01, 0x06 }, frame[4..6]);
        var f = Assert.Single(new K5FrameDecoder(obfuscated: false).Feed(frame));
        Assert.Equal(CrcStatus.Valid, f.Crc);
        Assert.Equal(0x7D, f.Body.Span[0]);
    }

    [Fact]
    public void Obfuscated_hello_raw_id_is_0x6902()
    {
        byte[] frame = K5FrameCodec.Encode(0x0514, new byte[4], obfuscate: true);
        Assert.Equal(0x6902, frame[4] | (frame[5] << 8));
    }

    [Fact]
    public void Resyncs_on_garbage_and_split_input()
    {
        var frames = Enumerable.Range(0, 5).Select(r => K5FrameCodec.Encode(0x0601, [(byte)r], true)).ToArray();
        byte[] stream = [0x00, 0xAB, 0x12, .. frames[0], 0xAB, 0xCD, 0xFF, .. frames.Skip(1).SelectMany(x => x)];
        var dec = new K5FrameDecoder();
        var got = new List<K5Frame>();
        foreach (byte b in stream)
        {
            got.AddRange(dec.Feed([b]));
        }

        Assert.Equal([0, 1, 2, 3, 4], got.Select(f => (int)f.Body.Span[0]));
    }

    [Fact]
    public void Flags_bad_crc()
    {
        byte[] frame = K5FrameCodec.Encode(0x0601, [0x10], true);
        frame[5] ^= 1;
        Assert.Equal(CrcStatus.Bad, Assert.Single(new K5FrameDecoder().Feed(frame)).Crc);
    }

    [Fact]
    public void Legacy_reply_footer_is_an_obfuscated_ffff()
    {
        // The firmware's footer padding is Obfuscation[(Size+i)%16]^0xFF.
        byte[] payload = K5FrameCodec.BuildPayload(0x0601, [0x7D, 0x40, 0xE9]);
        byte[] frame = K5FrameCodec.EncodeWithoutCrc(payload, obfuscate: true);
        int size = payload.Length;
        Assert.Equal(K5FrameCodec.ObfuscationKey[size % 16] ^ 0xFF, frame[4 + size]);
        Assert.Equal(K5FrameCodec.ObfuscationKey[(size + 1) % 16] ^ 0xFF, frame[5 + size]);
        var f = Assert.Single(new K5FrameDecoder().Feed(frame));
        Assert.Equal(CrcStatus.Absent, f.Crc);
        Assert.Equal(payload, f.Payload.ToArray());
    }

    [Fact]
    public void Real_bootloader_beacon_with_raw_ffff_crc()
    {
        var f = Assert.Single(new K5FrameDecoder().Feed(RealBeaconWire));
        Assert.Equal(CrcStatus.Absent, f.Crc);
        Assert.Equal(0x0518, f.Id);
        Assert.Contains("2.00.06", System.Text.Encoding.ASCII.GetString(f.Payload.Span));
    }

    [Fact]
    public void Truncated_frame_followed_by_a_good_one_is_recovered_at_once()
    {
        byte[] cut = K5FrameCodec.Encode(0x50C1, new byte[36], true)[..20];
        byte[] good = K5FrameCodec.Encode(0x50C0, [1, 2, 3, 4, 5, 6, 7, 1, 1, 0, 0x70, 0, 10, 2], true);
        var dec = new K5FrameDecoder();
        var frames = dec.Feed([.. cut, .. good]);
        var f = Assert.Single(frames);
        Assert.Equal(0x50C0, f.Id);
        Assert.Equal(CrcStatus.Valid, f.Crc);
        Assert.False(dec.HasPartialFrame);
    }

    [Fact]
    public void Drop_partial_frame_gives_up_on_a_cut_frame()
    {
        var dec = new K5FrameDecoder();
        Assert.Empty(dec.Feed(K5FrameCodec.Encode(0x50C0, new byte[14], true)[..10]));
        Assert.True(dec.HasPartialFrame);
        Assert.Empty(dec.DropPartialFrame());
        Assert.False(dec.HasPartialFrame);
        Assert.Single(dec.Feed(K5FrameCodec.Encode(0x50C6, new byte[19], true)));
    }

    [Fact]
    public void Oversize_length_is_garbage()
    {
        var dec = new K5FrameDecoder();
        byte[] junk = [0xAB, 0xCD, 0xFB, 0x00, 1, 2, 3];
        byte[] good = K5FrameCodec.Encode(0x0528, [0x7A, 0, 0x3B, 0x29], true);
        Assert.Single(dec.Feed([.. junk, .. good]));
    }

    [Fact]
    public void Banner_text_is_passed_to_the_discard_callback()
    {
        var dec = new K5FrameDecoder();
        var text = new List<byte>();
        byte[] banner = "UV-K5 packet firmware, PKTFW 2951c48\r\n"u8.ToArray();
        byte[] good = K5FrameCodec.Encode(0x0528, [0x7A, 0, 0x3B, 0x29], true);
        var frames = dec.Feed([.. banner, .. good], s => text.AddRange(s.ToArray()));
        Assert.Single(frames);
        Assert.Equal(banner, text.ToArray());
    }

    [Fact]
    public void Rssi_reply_capture_layout()
    {
        // K5TOOL PacketReadRssiAck: 28 05 04 00 7a 00 3b 29
        byte[] p = Convert.FromHexString("280504007a003b29");
        Assert.Equal(0x7A, BinaryPrimitives.ReadUInt16LittleEndian(p.AsSpan(4)));
        Assert.Equal(0x3B, p[6]);
        Assert.Equal(0x29, p[7]);
    }

    [Theory]
    [InlineData(true)]
    [InlineData(false)]
    public void V2_get_info_request_golden(bool obfuscate)
    {
        // Hand-built from protocol-v2.md sections 2 and 4.2: id 0x5000, body_len 1, tag 0x01.
        byte[] payload = [0x00, 0x50, 0x01, 0x00, 0x01];
        ushort crc = K5FrameCodec.Crc16(payload);
        byte[] inner = [.. payload, (byte)crc, (byte)(crc >> 8)];
        if (obfuscate)
        {
            for (int i = 0; i < inner.Length; i++)
            {
                inner[i] ^= K5FrameCodec.ObfuscationKey[i % 16];
            }
        }

        byte[] expected = [0xAB, 0xCD, 0x05, 0x00, .. inner, 0xDC, 0xBA];
        Assert.Equal(expected, K5FrameCodec.Encode(0x5000, [0x01], obfuscate));
    }

    [Fact]
    public void V2_ids_never_look_like_a_hello_in_either_mode()
    {
        for (int id = 0x5000; id <= 0x50FF; id++)
        {
            Assert.NotEqual(0x0514, id);
            Assert.NotEqual(0x6902, id);
            Assert.NotEqual(0x0514, id ^ 0x6C16);
            Assert.NotEqual(0x6902, id ^ 0x6C16);
        }
    }
}
