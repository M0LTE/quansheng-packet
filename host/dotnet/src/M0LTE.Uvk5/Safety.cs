namespace M0LTE.Uvk5;

/// <summary>
/// The library's hard safety rules, in one place. Every EEPROM write and register write goes
/// through these checks before anything is sent; there is no way round them.
/// </summary>
public static class K5Safety
{
    /// <summary>EEPROM size.</summary>
    public const int EepromSize = 0x2000;

    /// <summary>First byte of the factory calibration area (0x1E00 to 0x1FFF), never written.</summary>
    public const int CalibrationStart = 0x1E00;

    /// <summary>
    /// BK4819 registers the packet firmware refuses in its override table and REG_WRITE, and this
    /// library refuses everywhere (including legacy 0x0602, which the firmware itself leaves
    /// unrestricted): 0x00 soft reset, 0x30 TX/RX enables, 0x33 GPIO outputs (PA enable, RX enable,
    /// LNA switch, LEDs), 0x36 PA bias and gain, 0x37 power and LDOs, 0x38 and 0x39 frequency, 0x3B
    /// and 0x3C crystal trim, and anything above 0x7F.
    /// </summary>
    public static IReadOnlySet<int> RefusedRegisters { get; } = new HashSet<int> { 0x00, 0x30, 0x33, 0x36, 0x37, 0x38, 0x39, 0x3B, 0x3C };

    /// <summary>True if the library will refuse to write BK4819 register <paramref name="register"/>.</summary>
    public static bool IsRegisterRefused(int register) => register is < 0 or > 0x7F || RefusedRegisters.Contains(register);

    /// <summary>Throws <see cref="K5SafetyException"/> if <paramref name="register"/> may not be written.</summary>
    public static void CheckRegisterWrite(int register)
    {
        if (IsRegisterRefused(register))
        {
            throw new K5SafetyException(
                $"BK4819 register 0x{register:X2} is on the refusal list (soft reset, TX/RX enables, GPIO/PA enable, PA bias, power, frequency, crystal trim, or above 0x7F): write refused");
        }
    }

    /// <summary>
    /// Throws <see cref="K5SafetyException"/> unless <c>[address, address + length)</c> is inside the
    /// EEPROM, below the calibration area and made of whole aligned 8-byte blocks (the firmware
    /// writes 8-byte pages and refuses or drops anything else).
    /// </summary>
    public static void CheckEepromWrite(int address, int length)
    {
        if (length <= 0)
        {
            throw new K5SafetyException("empty EEPROM write");
        }

        long end = (long)address + length;
        if (address < 0 || end > EepromSize)
        {
            throw new K5SafetyException($"EEPROM write 0x{address:X4}+{length} is outside the EEPROM");
        }

        if (end > CalibrationStart)
        {
            throw new K5SafetyException(
                $"EEPROM write 0x{address:X4}..0x{end - 1:X4} touches the calibration area 0x1E00-0x1FFF: refused");
        }

        if (address % 8 != 0 || length % 8 != 0)
        {
            throw new K5SafetyException("EEPROM writes must be 8-byte aligned and a multiple of 8 bytes");
        }
    }

    /// <summary>
    /// Throws <see cref="K5SafetyException"/> if <paramref name="image"/> is not a usable backup:
    /// wrong size, or all 0x00 or all 0xFF as a whole or in the calibration area (what a locked,
    /// blank or misread radio returns).
    /// </summary>
    public static void CheckBackupContent(ReadOnlySpan<byte> image)
    {
        if (image.Length != EepromSize)
        {
            throw new K5SafetyException($"an EEPROM backup is {EepromSize} bytes, this is {image.Length}");
        }

        Check(image, "the EEPROM");
        Check(image[CalibrationStart..], "the calibration area 0x1E00-0x1FFF");

        static void Check(ReadOnlySpan<byte> part, string what)
        {
            if (!part.ContainsAnyExcept((byte)0))
            {
                throw new K5SafetyException($"{what} reads as all 0x00: not a usable backup");
            }

            if (!part.ContainsAnyExcept((byte)0xFF))
            {
                throw new K5SafetyException($"{what} reads as all 0xFF: not a usable backup");
            }
        }
    }
}

/// <summary>What <see cref="K5RadioOptions.Audit"/> is told about.</summary>
public enum K5AuditKind
{
    /// <summary>An EEPROM block write was sent (logged whatever the answer: the radio may have written it anyway).</summary>
    EepromWrite,

    /// <summary>A BK4819 register write was sent.</summary>
    RegisterWrite,

    /// <summary>A register override change was sent.</summary>
    RegisterOverride,

    /// <summary>Settings were changed.</summary>
    SettingsChange,

    /// <summary>A firmware flash started or finished.</summary>
    Flash,
}

/// <summary>An audit record of a change sent to the radio.</summary>
/// <param name="Kind">What kind of change.</param>
/// <param name="Description">One line, human readable.</param>
/// <param name="Outcome">"ok", "timeout", or the error.</param>
/// <param name="Details">Key/value details (addresses, bytes before and after, and so on).</param>
public sealed record K5AuditEntry(K5AuditKind Kind, string Description, string Outcome, IReadOnlyDictionary<string, string> Details);
