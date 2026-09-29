using System.Buffers.Binary;
using System.Text;

namespace M0LTE.Uvk5.Protocol;

/// <summary>Little-endian cursor over a received body. Throws <see cref="K5ProtocolException"/> when short.</summary>
internal ref struct WireReader
{
    private readonly ReadOnlySpan<byte> _data;
    private readonly string _what;
    private int _pos;

    public WireReader(ReadOnlySpan<byte> data, string what)
    {
        _data = data;
        _what = what;
        _pos = 0;
    }

    public readonly int Remaining => _data.Length - _pos;

    public readonly int Position => _pos;

    public byte U8() => Take(1)[0];

    public sbyte S8() => (sbyte)Take(1)[0];

    public ushort U16() => BinaryPrimitives.ReadUInt16LittleEndian(Take(2));

    public short S16() => BinaryPrimitives.ReadInt16LittleEndian(Take(2));

    public uint U32() => BinaryPrimitives.ReadUInt32LittleEndian(Take(4));

    public ReadOnlySpan<byte> Bytes(int n) => Take(n);

    public string Ascii(int n)
    {
        ReadOnlySpan<byte> s = Take(n);
        int end = s.IndexOf((byte)0);
        if (end >= 0)
        {
            s = s[..end];
        }

        return Encoding.ASCII.GetString(s);
    }

    private ReadOnlySpan<byte> Take(int n)
    {
        if (_pos + n > _data.Length)
        {
            throw new K5ProtocolException(
                $"{_what}: body too short ({_data.Length} bytes, needed at least {_pos + n})");
        }

        ReadOnlySpan<byte> s = _data.Slice(_pos, n);
        _pos += n;
        return s;
    }
}

/// <summary>Little-endian body builder.</summary>
internal sealed class WireWriter
{
    private readonly List<byte> _bytes = new(32);

    public int Length => _bytes.Count;

    public WireWriter U8(int v)
    {
        _bytes.Add((byte)v);
        return this;
    }

    public WireWriter U16(int v)
    {
        _bytes.Add((byte)v);
        _bytes.Add((byte)(v >> 8));
        return this;
    }

    public WireWriter S16(short v) => U16((ushort)v);

    public WireWriter U32(uint v)
    {
        U16((ushort)v);
        U16((ushort)(v >> 16));
        return this;
    }

    public WireWriter Bytes(ReadOnlySpan<byte> b)
    {
        foreach (byte x in b)
        {
            _bytes.Add(x);
        }

        return this;
    }

    public WireWriter Ascii(string s, int n)
    {
        var buf = new byte[n];
        Encoding.ASCII.GetBytes(s.AsSpan(0, Math.Min(s.Length, n)), buf);
        return Bytes(buf);
    }

    public byte[] ToArray() => [.. _bytes];
}
