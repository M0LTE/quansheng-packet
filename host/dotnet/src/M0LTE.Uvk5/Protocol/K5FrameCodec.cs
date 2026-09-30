using System.Buffers.Binary;

namespace M0LTE.Uvk5.Protocol;

/// <summary>
/// Encoding of UV-K5 serial frames, and the CRC and obfuscation they use. Stateless and
/// thread-safe.
/// </summary>
/// <remarks>
/// Wire format: <c>AB CD | len u16 | payload (len bytes) + crc16 u16 | DC BA</c>, where
/// <c>payload = id u16 | body_len u16 | body</c> and <c>crc16</c> is CRC-16/XMODEM over the
/// payload. In obfuscated mode payload and CRC are XORed with a 16-byte key cycling from payload
/// byte 0. The radio powers on obfuscated; a hello whose raw id bytes are <c>14 05</c> switches it
/// to plain and raw <c>02 69</c> switches it back.
/// </remarks>
public static class K5FrameCodec
{
    /// <summary>The 16-byte XOR key used in obfuscated mode.</summary>
    public static ReadOnlySpan<byte> ObfuscationKey =>
        [0x16, 0x6C, 0x14, 0xE6, 0x2E, 0x91, 0x0D, 0x40, 0x21, 0x35, 0xD5, 0x40, 0x13, 0x03, 0xE9, 0x80];

    /// <summary>
    /// The largest request payload the radio can take: its receive ring is 256 bytes, a frame adds
    /// 8 bytes of framing, and a frame filling the whole ring could never complete.
    /// </summary>
    public const int MaxRadioPayload = 247;

    /// <summary>
    /// CRC-16/XMODEM (polynomial 0x1021, initial value 0, no reflection, no final XOR). Check value
    /// for "123456789" is 0x31C3.
    /// </summary>
    public static ushort Crc16(ReadOnlySpan<byte> data, ushort crc = 0)
    {
        foreach (byte b in data)
        {
            crc ^= (ushort)(b << 8);
            for (int i = 0; i < 8; i++)
            {
                crc = (crc & 0x8000) != 0 ? (ushort)((crc << 1) ^ 0x1021) : (ushort)(crc << 1);
            }
        }

        return crc;
    }

    /// <summary>XORs <paramref name="data"/> in place with the obfuscation key, starting at key index 0.</summary>
    public static void Obfuscate(Span<byte> data)
    {
        ReadOnlySpan<byte> key = ObfuscationKey;
        for (int i = 0; i < data.Length; i++)
        {
            data[i] ^= key[i & 15];
        }
    }

    /// <summary>Builds a payload (<c>id | body_len | body</c>) from an id and a body.</summary>
    public static byte[] BuildPayload(ushort id, ReadOnlySpan<byte> body)
    {
        var payload = new byte[4 + body.Length];
        BinaryPrimitives.WriteUInt16LittleEndian(payload, id);
        BinaryPrimitives.WriteUInt16LittleEndian(payload.AsSpan(2), checked((ushort)body.Length));
        body.CopyTo(payload.AsSpan(4));
        return payload;
    }

    /// <summary>Encodes a whole frame for message <paramref name="id"/> with a real CRC.</summary>
    public static byte[] Encode(ushort id, ReadOnlySpan<byte> body, bool obfuscate) =>
        EncodePayload(BuildPayload(id, body), obfuscate);

    /// <summary>Encodes a whole frame around an already-built payload, with a real CRC.</summary>
    public static byte[] EncodePayload(ReadOnlySpan<byte> payload, bool obfuscate) =>
        EncodePayload(payload, obfuscate, Crc16(payload));

    /// <summary>
    /// Encodes a frame whose CRC field is 0xFFFF, obfuscated along with the payload when
    /// <paramref name="obfuscate"/> is set: what legacy firmware replies look like. For simulators.
    /// </summary>
    public static byte[] EncodeWithoutCrc(ReadOnlySpan<byte> payload, bool obfuscate) =>
        EncodePayload(payload, obfuscate, 0xFFFF);

    private static byte[] EncodePayload(ReadOnlySpan<byte> payload, bool obfuscate, ushort crc)
    {
        if (payload.Length > ushort.MaxValue)
        {
            throw new ArgumentException("payload too long for a frame", nameof(payload));
        }

        var frame = new byte[payload.Length + 8];
        frame[0] = 0xAB;
        frame[1] = 0xCD;
        BinaryPrimitives.WriteUInt16LittleEndian(frame.AsSpan(2), (ushort)payload.Length);
        payload.CopyTo(frame.AsSpan(4));
        BinaryPrimitives.WriteUInt16LittleEndian(frame.AsSpan(4 + payload.Length), crc);
        if (obfuscate)
        {
            Obfuscate(frame.AsSpan(4, payload.Length + 2));
        }

        frame[^2] = 0xDC;
        frame[^1] = 0xBA;
        return frame;
    }
}
