namespace M0LTE.Uvk5;

/// <summary>Base class for errors talking to a UV-K5.</summary>
public class K5Exception : Exception
{
    /// <summary>Creates the exception.</summary>
    public K5Exception(string message)
        : base(message)
    {
    }

    /// <summary>Creates the exception with an inner exception.</summary>
    public K5Exception(string message, Exception inner)
        : base(message, inner)
    {
    }
}

/// <summary>The radio did not answer in time.</summary>
public sealed class K5TimeoutException : K5Exception
{
    /// <summary>Creates the exception.</summary>
    public K5TimeoutException(string message)
        : base(message)
    {
    }
}

/// <summary>The radio answered with something the protocol does not allow (short body, wrong layout).</summary>
public sealed class K5ProtocolException : K5Exception
{
    /// <summary>Creates the exception.</summary>
    public K5ProtocolException(string message)
        : base(message)
    {
    }
}

/// <summary>
/// The library refused to do something because it could damage the radio or its calibration:
/// an EEPROM write into 0x1E00 to 0x1FFF, an EEPROM write without a verified backup of this radio,
/// a write to a register on the refusal list, a flash to an unknown bootloader, and so on. Nothing
/// was sent to the radio.
/// </summary>
public sealed class K5SafetyException : K5Exception
{
    /// <summary>Creates the exception.</summary>
    public K5SafetyException(string message)
        : base(message)
    {
    }
}

/// <summary>A protocol v2 command was answered with a non-OK status.</summary>
public sealed class K5CommandRejectedException : K5Exception
{
    /// <summary>Creates the exception.</summary>
    public K5CommandRejectedException(string command, K5Status status, byte detail)
        : base(Describe(command, status, detail))
    {
        Command = command;
        Status = status;
        Detail = detail;
    }

    /// <summary>The command that was rejected.</summary>
    public string Command { get; }

    /// <summary>The status the radio gave.</summary>
    public K5Status Status { get; }

    /// <summary>The detail byte: the offending parameter id, register or field offset, else 0.</summary>
    public byte Detail { get; }

    private static string Describe(string command, K5Status status, byte detail)
    {
        string what = status switch
        {
            K5Status.UnknownCommand => "the firmware does not know this command",
            K5Status.BadLength => "body length rejected",
            K5Status.BadParameter => $"unknown or repeated parameter 0x{detail:X2} ({(RadioParameterId)detail})",
            K5Status.Range => $"value out of range (parameter or field 0x{detail:X2})",
            K5Status.TxBand => "the frequency is not TX-allowed and RequireTxAllowed was set",
            K5Status.State => "not possible in the radio's current state (reduced service?)",
            K5Status.Refused => $"register 0x{detail:X2} is on the firmware's refusal list",
            K5Status.Unsupported => "capability absent or uncalibrated",
            K5Status.Eeprom => "the settings block in EEPROM is not valid, so nothing can be persisted",
            K5Status.NotPersistable => $"parameter 0x{detail:X2} ({(RadioParameterId)detail}) is RAM-only and cannot be persisted",
            _ => $"status 0x{(byte)status:X2}",
        };
        return $"{command} rejected by the radio: {what}";
    }
}

/// <summary>
/// The call needs a newer firmware than the radio runs (for example a protocol v2 command on
/// packet firmware v1 or stock firmware). Nothing was sent.
/// </summary>
public sealed class K5FirmwareNotSupportedException : NotSupportedException
{
    /// <summary>Creates the exception.</summary>
    public K5FirmwareNotSupportedException(string operation, FirmwareKind actual, string requirement)
        : base($"{operation} needs {requirement}; the radio runs {Describe(actual)}")
    {
        Operation = operation;
        Actual = actual;
    }

    /// <summary>What was attempted.</summary>
    public string Operation { get; }

    /// <summary>The firmware the radio runs.</summary>
    public FirmwareKind Actual { get; }

    private static string Describe(FirmwareKind k) => k switch
    {
        FirmwareKind.PacketV2 => "packet firmware with protocol v2",
        FirmwareKind.PacketV1 => "packet firmware v1 (legacy commands and the EEPROM settings block only)",
        FirmwareKind.Stock => "stock or another third-party firmware (legacy commands only)",
        _ => "an unidentified firmware",
    };
}

/// <summary>
/// A frame was about to be sent while the host holds a <see cref="TransmitWindow"/>. On the AIOC
/// any byte written to the serial port releases PTT, so the frame would end the transmission.
/// Nothing was sent.
/// </summary>
public sealed class K5TransmitInProgressException : InvalidOperationException
{
    /// <summary>Creates the exception.</summary>
    public K5TransmitInProgressException(string message)
        : base(message)
    {
    }
}
