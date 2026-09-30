using System.Buffers.Binary;

namespace M0LTE.Uvk5.Protocol;

/// <summary>What the CRC field of a received frame said.</summary>
public enum CrcStatus
{
    /// <summary>A real CRC-16/XMODEM over the payload, and it matched.</summary>
    Valid,

    /// <summary>
    /// The field held 0xFFFF (obfuscated or raw) instead of a CRC. Legacy firmware replies and the
    /// bootloader do this; v2 (0x50xx) frames never do.
    /// </summary>
    Absent,

    /// <summary>Neither a matching CRC nor 0xFFFF: the frame is damaged.</summary>
    Bad,
}

/// <summary>
/// One decoded UV-K5 frame: <c>AB CD | len | id u16 | body_len u16 | body | crc | DC BA</c>, with
/// the obfuscation already removed.
/// </summary>
/// <param name="Id">The message id (payload bytes 0 and 1, little-endian).</param>
/// <param name="Payload">The whole de-obfuscated payload, id and body length included.</param>
/// <param name="Crc">What the CRC field said.</param>
public readonly record struct K5Frame(ushort Id, ReadOnlyMemory<byte> Payload, CrcStatus Crc)
{
    /// <summary>The body: the payload after the 4-byte id and body-length header.</summary>
    public ReadOnlyMemory<byte> Body => Payload.Length > 4 ? Payload[4..] : ReadOnlyMemory<byte>.Empty;

    /// <summary>The body length the frame declared in its inner header (payload bytes 2 and 3).</summary>
    public int DeclaredBodyLength =>
        Payload.Length >= 4 ? BinaryPrimitives.ReadUInt16LittleEndian(Payload.Span[2..]) : 0;

    /// <summary>Total bytes the frame occupied on the wire: 8 bytes of framing plus the payload.</summary>
    public int WireLength => Payload.Length + 8;

    /// <inheritdoc/>
    public override string ToString() => $"K5Frame(0x{Id:X4}, {Payload.Length} bytes, CRC {Crc})";
}
