using System.Buffers.Binary;
using System.Security.Cryptography;
using System.Text;
using M0LTE.Uvk5.Protocol;

namespace M0LTE.Uvk5.Bootloader;

/// <summary>
/// A UV-K5 firmware image: the raw DP32G030 image plus the 16-byte version string the bootloader
/// is told. Loads packed (<c>fw-pack.py</c>: version inserted at 0x2000, XOR-obfuscated, CRC
/// appended) and raw images, and packs and unpacks them.
/// </summary>
public sealed class FirmwareImage
{
    /// <summary>Flash available to the firmware: 60 KiB from 0; the bootloader lives above.</summary>
    public const int FlashLimit = 0xF000;

    /// <summary>Flash write block.</summary>
    public const int BlockSize = 0x100;

    /// <summary>Where fw-pack.py inserts the version string.</summary>
    public const int VersionOffset = 0x2000;

    private const uint RamStart = 0x2000_0000;
    private const uint RamEnd = 0x2000_4000;

    private static ReadOnlySpan<byte> FwKey =>
    [
        0x47, 0x22, 0xc0, 0x52, 0x5d, 0x57, 0x48, 0x94, 0xb1, 0x60, 0x60, 0xdb, 0x6f, 0xe3, 0x4c, 0x7c,
        0xd8, 0x4a, 0xd6, 0x8b, 0x30, 0xec, 0x25, 0xe0, 0x4c, 0xd9, 0x00, 0x7f, 0xbf, 0xe3, 0x54, 0x05,
        0xe9, 0x3a, 0x97, 0x6b, 0xb0, 0x6e, 0x0c, 0xfb, 0xb1, 0x1a, 0xe2, 0xc9, 0xc1, 0x56, 0x47, 0xe9,
        0xba, 0xf1, 0x42, 0xb6, 0x67, 0x5f, 0x0f, 0x96, 0xf7, 0xc9, 0x3c, 0x84, 0x1b, 0x26, 0xe1, 0x4e,
        0x3b, 0x6f, 0x66, 0xe6, 0xa0, 0x6a, 0xb0, 0xbf, 0xc6, 0xa5, 0x70, 0x3a, 0xba, 0x18, 0x9e, 0x27,
        0x1a, 0x53, 0x5b, 0x71, 0xb1, 0x94, 0x1e, 0x18, 0xf2, 0xd6, 0x81, 0x02, 0x22, 0xfd, 0x5a, 0x28,
        0x91, 0xdb, 0xba, 0x5d, 0x64, 0xc6, 0xfe, 0x86, 0x83, 0x9c, 0x50, 0x1c, 0x73, 0x03, 0x11, 0xd6,
        0xaf, 0x30, 0xf4, 0x2c, 0x77, 0xb2, 0x7d, 0xbb, 0x3f, 0x29, 0x28, 0x57, 0x22, 0xd6, 0x92, 0x8b,
    ];

    private readonly byte[] _raw;
    private readonly byte[] _version;

    /// <summary>Creates an image from a raw binary and a version string (up to 16 ASCII characters).</summary>
    public FirmwareImage(ReadOnlySpan<byte> raw, string version, bool wasPacked = false)
        : this(raw.ToArray(), VersionBytes(version), wasPacked)
    {
    }

    private FirmwareImage(byte[] raw, byte[] version, bool wasPacked)
    {
        _raw = raw;
        _version = version;
        WasPacked = wasPacked;
    }

    /// <summary>The raw image, as it will sit in flash from address 0.</summary>
    public ReadOnlyMemory<byte> Raw => _raw;

    /// <summary>The 16 version bytes sent to the bootloader.</summary>
    public ReadOnlyMemory<byte> VersionField => _version;

    /// <summary>The version as text, for example <c>*PKTFW 2951c48</c>. A leading <c>*</c> is a wildcard for the bootloader's major version.</summary>
    public string Version
    {
        get
        {
            int end = Array.IndexOf(_version, (byte)0);
            return Encoding.ASCII.GetString(_version, 0, end < 0 ? _version.Length : end);
        }
    }

    /// <summary>True if the image was loaded from a packed file.</summary>
    public bool WasPacked { get; }

    /// <summary>256-byte flash blocks needed.</summary>
    public int BlockCount => (_raw.Length + BlockSize - 1) / BlockSize;

    /// <summary>SHA-256 of the raw image, lower-case hex.</summary>
    public string Sha256 => Convert.ToHexStringLower(SHA256.HashData(_raw));

    /// <summary>
    /// Loads a packed or raw image. A file whose last two bytes are a valid CRC over the rest, and
    /// which unpacks to a plausible image, is treated as packed. <paramref name="versionOverride"/>
    /// replaces the version (raw images default to <c>*</c>).
    /// </summary>
    /// <exception cref="InvalidDataException">Not a UV-K5 image.</exception>
    public static FirmwareImage Load(ReadOnlySpan<byte> file, string? versionOverride = null)
    {
        if (PackedCrcOk(file))
        {
            try
            {
                var (raw, ver) = Unpack(file);
                CheckVectorTable(raw);
                return new FirmwareImage(raw, versionOverride is null ? ver : VersionBytes(versionOverride), wasPacked: true);
            }
            catch (InvalidDataException)
            {
                // fall through: maybe a raw image whose tail happens to look like a CRC
            }
        }

        CheckVectorTable(file);
        return new FirmwareImage(file.ToArray(), VersionBytes(versionOverride ?? "*"), wasPacked: false);
    }

    /// <summary>Loads a packed or raw image file.</summary>
    public static async Task<FirmwareImage> LoadAsync(string path, string? versionOverride = null, CancellationToken cancellationToken = default) =>
        Load(await File.ReadAllBytesAsync(path, cancellationToken).ConfigureAwait(false), versionOverride);

    /// <summary>
    /// Same output as upstream fw-pack.py: insert the 16-byte version at 0x2000, obfuscate, append
    /// CRC-16/XMODEM little-endian.
    /// </summary>
    public static byte[] Pack(ReadOnlySpan<byte> raw, string version)
    {
        byte[] ver = VersionBytes(version);
        int split = Math.Min(VersionOffset, raw.Length);
        var plain = new byte[raw.Length + 16];
        raw[..split].CopyTo(plain);
        ver.CopyTo(plain.AsSpan(split));
        raw[split..].CopyTo(plain.AsSpan(split + 16));
        FwXor(plain);
        var packed = new byte[plain.Length + 2];
        plain.CopyTo(packed, 0);
        BinaryPrimitives.WriteUInt16LittleEndian(packed.AsSpan(plain.Length), K5FrameCodec.Crc16(plain));
        return packed;
    }

    /// <summary>Undoes <see cref="Pack(ReadOnlySpan{byte}, string)"/>: returns the raw image and the 16 version bytes.</summary>
    /// <exception cref="InvalidDataException">CRC mismatch.</exception>
    public static (byte[] Raw, byte[] Version) Unpack(ReadOnlySpan<byte> packed)
    {
        if (!PackedCrcOk(packed))
        {
            throw new InvalidDataException("packed image CRC mismatch");
        }

        byte[] plain = packed[..^2].ToArray();
        FwXor(plain);
        if (plain.Length < VersionOffset + 16)
        {
            throw new InvalidDataException("packed image too short to hold a version at 0x2000");
        }

        byte[] version = plain.AsSpan(VersionOffset, 16).ToArray();
        byte[] raw = [.. plain.AsSpan(0, VersionOffset), .. plain.AsSpan(VersionOffset + 16)];
        return (raw, version);
    }

    /// <summary>This image, packed.</summary>
    public byte[] Pack() => Pack(_raw, Version);

    /// <summary>
    /// Checks the image can be flashed: a DP32G030 vector table (initial stack pointer in RAM,
    /// Thumb reset vector inside the image) and a size that stays below the bootloader at 0xF000.
    /// </summary>
    /// <exception cref="K5SafetyException">Too large.</exception>
    /// <exception cref="InvalidDataException">Not a UV-K5 image.</exception>
    public void Validate()
    {
        CheckVectorTable(_raw);
        if (BlockCount * BlockSize > FlashLimit)
        {
            throw new K5SafetyException($"image is {_raw.Length} bytes; it would write past 0x{FlashLimit:X4} into the bootloader");
        }
    }

    /// <summary>The block plan, one line per 256-byte block.</summary>
    public IEnumerable<string> BlockPlan()
    {
        for (int c = 0; c < BlockCount; c++)
        {
            yield return $"block {c,3}: 0x{c * BlockSize:X4}..0x{c * BlockSize + BlockSize - 1:X4} len 0x{Math.Min(BlockSize, _raw.Length - c * BlockSize):X3}";
        }
    }

    internal static byte[] VersionBytes(string version)
    {
        var v = new byte[16];
        Encoding.ASCII.GetBytes(version.AsSpan(0, Math.Min(16, version.Length)), v);
        return v;
    }

    private static bool PackedCrcOk(ReadOnlySpan<byte> data) =>
        data.Length > 2 && K5FrameCodec.Crc16(data[..^2]) == BinaryPrimitives.ReadUInt16LittleEndian(data[^2..]);

    private static void FwXor(Span<byte> data)
    {
        ReadOnlySpan<byte> key = FwKey;
        for (int i = 0; i < data.Length; i++)
        {
            data[i] ^= key[i & 127];
        }
    }

    private static void CheckVectorTable(ReadOnlySpan<byte> raw)
    {
        if (raw.Length < 8)
        {
            throw new InvalidDataException("image too short");
        }

        uint sp = BinaryPrimitives.ReadUInt32LittleEndian(raw);
        uint reset = BinaryPrimitives.ReadUInt32LittleEndian(raw[4..]);
        if (!(sp > RamStart && sp <= RamEnd))
        {
            throw new InvalidDataException($"initial stack pointer 0x{sp:X8} is not in RAM: not a plain UV-K5 image (a packed file with a bad CRC?)");
        }

        if ((reset & 1) == 0 || (reset & ~1u) >= raw.Length)
        {
            throw new InvalidDataException($"reset vector 0x{reset:X8} is not a Thumb address inside the image");
        }
    }
}
