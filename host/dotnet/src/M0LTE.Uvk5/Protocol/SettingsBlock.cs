using System.Buffers.Binary;

namespace M0LTE.Uvk5.Protocol;

/// <summary>
/// Packet firmware v1 settings in EEPROM: the 16-byte settings block at 0x1D00 and the 8-byte
/// timing block at 0x1D50 (docs/packet-fw.md). A byte out of range means "use the default".
/// Bytes 3 (was the mic gain) and 11 (was the battery type) are reserved: the current firmware
/// ignores them and writes 0xFF, so they are neither decoded nor written here.
/// </summary>
internal static class SettingsBlock
{
    public const int Address = 0x1D00;
    public const int Length = 16;
    public const int TimingAddress = 0x1D50;
    public const int TimingLength = 8;
    public const byte Layout = 1;

    public static readonly RadioParameterId[] V1Parameters =
    [
        RadioParameterId.DeviationWide, RadioParameterId.DeviationNarrow, RadioParameterId.BusySquelchLevel,
        RadioParameterId.RxGain, RadioParameterId.RxDacGain, RadioParameterId.TxTimeoutSeconds, RadioParameterId.PttPressMs,
        RadioParameterId.PttReleaseMs, RadioParameterId.PaEnableDelayMs, RadioParameterId.PaBiasDelayMs,
        RadioParameterId.Backlight, RadioParameterId.KeyLock,
    ];

    public static bool IsValid(ReadOnlySpan<byte> block) => block.Length >= 1 && block[0] == Layout;

    /// <summary>
    /// The settings the firmware is using, given the two blocks: an out-of-range byte reports the
    /// default the firmware falls back to. RX gain's default is the factory calibration, which is
    /// not decoded, so an out-of-range RX gain is reported as null.
    /// </summary>
    public static RadioSettings Decode(ReadOnlySpan<byte> block, ReadOnlySpan<byte> timing, DeviationLaw law)
    {
        static int U8(ReadOnlySpan<byte> b, int off, int min, int max, int def) => b[off] >= min && b[off] <= max ? b[off] : def;
        static ushort Dev(ReadOnlySpan<byte> b, int off, ushort def)
        {
            ushort v = BinaryPrimitives.ReadUInt16LittleEndian(b[off..]);
            return v <= Deviation.MaxRegister ? v : def;
        }

        int rxGain = block[8];
        return new RadioSettings
        {
            BusySquelchLevel = U8(block, 1, 0, 9, 1),
            TxTimeout = TimeSpan.FromSeconds(ParameterCodec.TxTimeoutSeconds[U8(block, 2, 0, 6, 4)]),
            DeviationWide = new Deviation(Dev(block, 4, 0x856), law),
            DeviationNarrow = new Deviation(Dev(block, 6, 0x756), law),
            RxGain = rxGain <= 63 ? rxGain : null,
            RxDacGain = U8(block, 9, 0, 15, 15),
            Backlight = U8(block, 10, 0, 7, 3),
            KeyLock = U8(block, 12, 0, 1, 0) == 1,
            PttPressDebounce = TimeSpan.FromMilliseconds(U8(timing, 0, 1, 40, 5)),
            PttReleaseDebounce = TimeSpan.FromMilliseconds(U8(timing, 1, 2, 40, 5)),
            PaEnableDelay = TimeSpan.FromMilliseconds(U8(timing, 2, 1, 20, 1)),
            PaBiasDelay = TimeSpan.FromMilliseconds(U8(timing, 3, 0, 20, 2)),
        };
    }

    /// <summary>
    /// Applies <paramref name="changes"/> to copies of the blocks. Throws
    /// <see cref="K5FirmwareNotSupportedException"/> for settings v1 does not keep in these blocks.
    /// </summary>
    public static (byte[] Block, byte[] Timing) Encode(ReadOnlySpan<byte> block, ReadOnlySpan<byte> timing, RadioSettings changes)
    {
        var b = block.ToArray();
        var t = timing.ToArray();
        foreach (var (id, v) in ParameterCodec.ToWire(changes))
        {
            switch (id)
            {
                case RadioParameterId.BusySquelchLevel: b[1] = (byte)v; break;
                case RadioParameterId.TxTimeoutSeconds: b[2] = (byte)Array.IndexOf(ParameterCodec.TxTimeoutSeconds, (int)v); break;
                case RadioParameterId.DeviationWide: BinaryPrimitives.WriteUInt16LittleEndian(b.AsSpan(4), (ushort)v); break;
                case RadioParameterId.DeviationNarrow: BinaryPrimitives.WriteUInt16LittleEndian(b.AsSpan(6), (ushort)v); break;
                case RadioParameterId.RxGain: b[8] = (byte)v; break;
                case RadioParameterId.RxDacGain: b[9] = (byte)v; break;
                case RadioParameterId.Backlight: b[10] = (byte)v; break;
                case RadioParameterId.KeyLock: b[12] = (byte)v; break;
                case RadioParameterId.PttPressMs: t[0] = (byte)v; break;
                case RadioParameterId.PttReleaseMs: t[1] = (byte)v; break;
                case RadioParameterId.PaEnableDelayMs: t[2] = (byte)v; break;
                case RadioParameterId.PaBiasDelayMs: t[3] = (byte)v; break;
                default:
                    throw new K5FirmwareNotSupportedException($"setting {id}", FirmwareKind.PacketV1, "protocol v2 firmware (v1 keeps it outside the settings block)");
            }
        }

        return (b, t);
    }
}
