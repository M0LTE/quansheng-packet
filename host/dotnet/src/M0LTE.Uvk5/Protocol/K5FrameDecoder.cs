using System.Buffers.Binary;

namespace M0LTE.Uvk5.Protocol;

/// <summary>
/// Incremental decoder for a byte stream of UV-K5 frames. Resynchronises on <c>AB CD</c> after
/// garbage, truncated frames and unframed text such as the power-on banner. Not thread-safe: feed
/// it from one reader.
/// </summary>
/// <remarks>
/// <para>Follows the host parser of protocol v2 section 2.3: scan for <c>AB CD</c>; a length over
/// <see cref="MaxPayload"/> drops the <c>AB</c> and rescans; a bad footer drops only the
/// <c>AB</c> and rescans from the next byte, so a truncated frame followed by a good one is
/// recoverable.</para>
/// <para>One addition: while a frame is incomplete, the decoder looks inside the bytes already
/// received for a complete, well-formed frame (right footer, and a CRC that is either valid or
/// 0xFFFF). If it finds one, the incomplete frame was truncated (the AIOC drops radio bytes while
/// PTT is asserted) and the good frame is delivered at once instead of waiting for enough later
/// bytes to disprove the truncated one.</para>
/// <para>The bootloader sends its CRC field as raw <c>FF FF</c>, not obfuscated, so the CRC field
/// is checked for 0xFFFF on the wire before the XOR is undone as well as after.</para>
/// </remarks>
public sealed class K5FrameDecoder
{
    private byte[] _buf = new byte[1024];
    private int _start;
    private int _count;

    /// <summary>Creates a decoder.</summary>
    /// <param name="obfuscated">Whether frames are obfuscated (the radio's power-on mode).</param>
    /// <param name="maxPayload">Largest payload accepted; longer lengths are treated as garbage.
    /// The v2 spec uses 250 for frames from the radio; a bootloader simulator decoding host flash
    /// writes (272 bytes) needs more.</param>
    public K5FrameDecoder(bool obfuscated = true, int maxPayload = 250)
    {
        Obfuscated = obfuscated;
        MaxPayload = maxPayload;
    }

    /// <summary>Whether incoming frames are obfuscated. May be changed between feeds.</summary>
    public bool Obfuscated { get; set; }

    /// <summary>Largest payload length accepted.</summary>
    public int MaxPayload { get; }

    /// <summary>Bytes discarded outside frames since construction (garbage, text, damaged frames).</summary>
    public long DiscardedBytes { get; private set; }

    /// <summary>Frames decoded with <see cref="CrcStatus.Bad"/> since construction.</summary>
    public long BadCrcFrames { get; private set; }

    /// <summary>True while part of a frame is buffered.</summary>
    public bool HasPartialFrame => _count > 0;

    /// <summary>
    /// Feeds bytes and returns every complete frame they finish, in order. Bytes discarded as
    /// not belonging to any frame are passed to <paramref name="discarded"/> (in order) if given.
    /// Frames with a bad CRC are returned too, marked <see cref="CrcStatus.Bad"/>.
    /// </summary>
    public List<K5Frame> Feed(ReadOnlySpan<byte> data, Action<ReadOnlySpan<byte>>? discarded = null)
    {
        Append(data);
        var frames = new List<K5Frame>();
        while (_count > 0)
        {
            ReadOnlySpan<byte> b = _buf.AsSpan(_start, _count);
            int i = b.IndexOf((byte)0xAB);
            if (i < 0)
            {
                Discard(_count, discarded);
                break;
            }

            if (i > 0)
            {
                Discard(i, discarded);
                continue;
            }

            if (b.Length < 2)
            {
                break;
            }

            if (b[1] != 0xCD)
            {
                Discard(1, discarded);
                continue;
            }

            if (b.Length < 4)
            {
                break;
            }

            int n = BinaryPrimitives.ReadUInt16LittleEndian(b[2..]);
            if (n > MaxPayload || n < 4)
            {
                Discard(1, discarded);
                continue;
            }

            int total = n + 8;
            if (b.Length < total)
            {
                int nested = FindCompleteNestedFrame(b);
                if (nested > 0)
                {
                    Discard(nested, discarded);
                    continue;
                }

                break;
            }

            if (b[total - 2] != 0xDC || b[total - 1] != 0xBA)
            {
                Discard(1, discarded);
                continue;
            }

            frames.Add(DecodeAt(b, n));
            _start += total;
            _count -= total;
        }

        if (_count == 0)
        {
            _start = 0;
        }

        return frames;
    }

    /// <summary>
    /// Gives up on a buffered partial frame: drops its <c>AB</c> and rescans what follows, which
    /// may hold complete frames. Call it when the link has been quiet for longer than a frame
    /// could take (a frame's bytes arrive back to back, so a long gap means it was truncated).
    /// </summary>
    public List<K5Frame> DropPartialFrame(Action<ReadOnlySpan<byte>>? discarded = null)
    {
        if (_count > 0)
        {
            Discard(1, discarded);
        }

        return Feed([], discarded);
    }

    /// <summary>Drops any buffered partial frame (for example after changing mode or port).</summary>
    public void Reset()
    {
        _start = 0;
        _count = 0;
    }

    private K5Frame DecodeAt(ReadOnlySpan<byte> b, int n)
    {
        var body = b.Slice(4, n + 2).ToArray();
        bool rawNone = body[n] == 0xFF && body[n + 1] == 0xFF;
        if (Obfuscated)
        {
            K5FrameCodec.Obfuscate(body);
        }

        ushort crc = BinaryPrimitives.ReadUInt16LittleEndian(body.AsSpan(n));
        CrcStatus status;
        if (crc == K5FrameCodec.Crc16(body.AsSpan(0, n)))
        {
            status = CrcStatus.Valid;
        }
        else if (rawNone || crc == 0xFFFF)
        {
            status = CrcStatus.Absent;
        }
        else
        {
            status = CrcStatus.Bad;
            BadCrcFrames++;
        }

        var payload = body.AsMemory(0, n);
        ushort id = BinaryPrimitives.ReadUInt16LittleEndian(payload.Span);
        return new K5Frame(id, payload, status);
    }

    // Returns the offset of a complete, well-formed frame starting after offset 0, or -1.
    private int FindCompleteNestedFrame(ReadOnlySpan<byte> b)
    {
        for (int j = 1; j + 8 <= b.Length; j++)
        {
            if (b[j] != 0xAB || b[j + 1] != 0xCD)
            {
                continue;
            }

            int n = BinaryPrimitives.ReadUInt16LittleEndian(b[(j + 2)..]);
            if (n > MaxPayload || n < 4 || j + n + 8 > b.Length)
            {
                continue;
            }

            if (b[j + n + 6] != 0xDC || b[j + n + 7] != 0xBA)
            {
                continue;
            }

            if (DecodeAtNoCount(b[j..], n).Crc != CrcStatus.Bad)
            {
                return j;
            }
        }

        return -1;
    }

    private K5Frame DecodeAtNoCount(ReadOnlySpan<byte> b, int n)
    {
        long before = BadCrcFrames;
        var f = DecodeAt(b, n);
        BadCrcFrames = before;
        return f;
    }

    private void Discard(int n, Action<ReadOnlySpan<byte>>? discarded)
    {
        discarded?.Invoke(_buf.AsSpan(_start, n));
        DiscardedBytes += n;
        _start += n;
        _count -= n;
    }

    private void Append(ReadOnlySpan<byte> data)
    {
        if (_start + _count + data.Length > _buf.Length)
        {
            if (_count + data.Length <= _buf.Length)
            {
                Buffer.BlockCopy(_buf, _start, _buf, 0, _count);
            }
            else
            {
                var bigger = new byte[Math.Max(_buf.Length * 2, _count + data.Length)];
                Buffer.BlockCopy(_buf, _start, bigger, 0, _count);
                _buf = bigger;
            }

            _start = 0;
        }

        data.CopyTo(_buf.AsSpan(_start + _count));
        _count += data.Length;
    }
}
