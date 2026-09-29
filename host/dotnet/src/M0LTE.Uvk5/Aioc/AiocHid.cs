using System.Runtime.InteropServices;
using System.Runtime.Versioning;
using Microsoft.Win32.SafeHandles;

namespace M0LTE.Uvk5.Aioc;

/// <summary>
/// The AIOC's configuration registers, through its HID feature report (Linux hidraw). Only what a
/// packet station needs: turning off RXIGNPTT so the radio's frames reach the host while PTT is
/// held (for the v2 LIVE_TX option), and putting it back.
/// </summary>
/// <remarks>
/// <para>Feature report layout (AIOC firmware <c>usb_hid.c</c>): 6 bytes, <c>ctrl, address, data
/// u32 LE</c>. Control bit 0x01 is the write strobe (RAM only); 0x80 would store the settings in
/// the AIOC's flash, 0x10 load defaults, 0x40 recall, 0x20 reboot. <b>This class only ever uses the
/// write strobe</b>: nothing it does survives unplugging the AIOC. Reading sets the address with
/// ctrl 0 and then gets the report.</para>
/// <para>Register 0x60 (serial control): TXFRCPTT in bits 11:8 (serial writes release these PTT
/// outputs) and RXIGNPTT in bits 19:16 (radio bytes are dropped while these are asserted). Default
/// 0x00010100. TXFRCPTT for PTT1 must stay on: without it the AIOC's push-pull UART output fights
/// the PTT pull-down on the shared contact.</para>
/// </remarks>
public sealed class AiocHid : IDisposable
{
    /// <summary>AIOC USB vendor id.</summary>
    public const ushort VendorId = 0x1209;

    /// <summary>AIOC USB product id.</summary>
    public const ushort ProductId = 0x7388;

    /// <summary>The serial control register.</summary>
    public const byte SerialControlRegister = 0x60;

    /// <summary>TXFRCPTT field mask in register 0x60.</summary>
    public const uint TxForcePttMask = 0x0000_0F00;

    /// <summary>RXIGNPTT field mask in register 0x60.</summary>
    public const uint RxIgnorePttMask = 0x000F_0000;

    private const byte WriteStrobe = 0x01;
    private readonly IFeatureReportDevice _device;
    private readonly Lock _gate = new();

    internal AiocHid(IFeatureReportDevice device) => _device = device;

    /// <summary>hidraw devices whose USB id is the AIOC's (Linux).</summary>
    [SupportedOSPlatform("linux")]
    public static IReadOnlyList<string> FindDevices()
    {
        var found = new List<string>();
        const string root = "/sys/class/hidraw";
        if (!Directory.Exists(root))
        {
            return found;
        }

        string want = $"HID_ID=0003:{VendorId:X8}:{ProductId:X8}";
        foreach (string dir in Directory.GetDirectories(root).Order(StringComparer.Ordinal))
        {
            try
            {
                string uevent = File.ReadAllText(Path.Combine(dir, "device", "uevent"));
                if (uevent.Contains(want, StringComparison.OrdinalIgnoreCase))
                {
                    found.Add("/dev/" + Path.GetFileName(dir));
                }
            }
            catch (IOException)
            {
            }
            catch (UnauthorizedAccessException)
            {
            }
        }

        return found;
    }

    /// <summary>Opens an AIOC hidraw device, for example <c>/dev/hidraw2</c> (Linux).</summary>
    [SupportedOSPlatform("linux")]
    public static AiocHid Open(string hidrawPath) => new(new LinuxHidraw(hidrawPath));

    /// <summary>Reads a configuration register.</summary>
    public uint ReadRegister(byte address)
    {
        lock (_gate)
        {
            _device.SetFeature([0x00, 0x00, address, 0, 0, 0, 0]);
            Span<byte> buf = stackalloc byte[7];
            _device.GetFeature(buf);
            return BitConverter.ToUInt32(buf[3..7]);
        }
    }

    /// <summary>Writes a configuration register in RAM only (never stored to the AIOC's flash).</summary>
    public void WriteRegister(byte address, uint value)
    {
        lock (_gate)
        {
            _device.SetFeature([0x00, WriteStrobe, address, (byte)value, (byte)(value >> 8), (byte)(value >> 16), (byte)(value >> 24)]);
        }
    }

    /// <summary>
    /// Clears RXIGNPTT (keeping TXFRCPTT), so frames from the radio reach the host while PTT is held.
    /// Disposing the result restores the register as it was. RAM only: unplugging the AIOC also
    /// restores its stored setting. Measured on the bench: with the default 0x00010100 four of four
    /// replies sent while keyed were lost; with 0x00000100 all four arrived intact.
    /// </summary>
    /// <exception cref="InvalidOperationException">TXFRCPTT does not include PTT1 (unsafe on the K1 wiring).</exception>
    public IDisposable AllowRadioOutputWhileKeyed()
    {
        uint before = ReadRegister(SerialControlRegister);
        if ((before & 0x100) == 0)
        {
            throw new InvalidOperationException(
                $"AIOC register 0x60 is 0x{before:X8}: TXFRCPTT does not include PTT1. It must, on the K1 wiring, so this refuses to go further.");
        }

        WriteRegister(SerialControlRegister, before & ~RxIgnorePttMask);
        return new Restore(this, before);
    }

    /// <inheritdoc/>
    public void Dispose() => _device.Dispose();

    private sealed class Restore(AiocHid hid, uint value) : IDisposable
    {
        private int _done;

        public void Dispose()
        {
            if (Interlocked.Exchange(ref _done, 1) == 0)
            {
                hid.WriteRegister(SerialControlRegister, value);
            }
        }
    }
}

/// <summary>A HID device that takes and gives feature reports (report id first).</summary>
internal interface IFeatureReportDevice : IDisposable
{
    void SetFeature(ReadOnlySpan<byte> report);

    void GetFeature(Span<byte> report);
}

[SupportedOSPlatform("linux")]
internal sealed partial class LinuxHidraw : IFeatureReportDevice
{
    private readonly SafeFileHandle _handle;

    public LinuxHidraw(string path) => _handle = File.OpenHandle(path, FileMode.Open, FileAccess.ReadWrite);

    // _IOC(_IOC_READ | _IOC_WRITE, 'H', nr, len)
    private static nuint Ioc(int nr, int len) => (nuint)((3u << 30) | ((uint)len << 16) | ('H' << 8) | (uint)nr);

    public unsafe void SetFeature(ReadOnlySpan<byte> report)
    {
        fixed (byte* p = report)
        {
            Check(Ioctl((int)_handle.DangerousGetHandle(), Ioc(0x06, report.Length), p), "HIDIOCSFEATURE");
        }
    }

    public unsafe void GetFeature(Span<byte> report)
    {
        fixed (byte* p = report)
        {
            Check(Ioctl((int)_handle.DangerousGetHandle(), Ioc(0x07, report.Length), p), "HIDIOCGFEATURE");
        }
    }

    public void Dispose() => _handle.Dispose();

    private static void Check(int rc, string what)
    {
        if (rc < 0)
        {
            throw new IOException($"{what} failed: errno {Marshal.GetLastPInvokeError()}");
        }
    }

    [LibraryImport("libc", EntryPoint = "ioctl", SetLastError = true)]
    private static unsafe partial int Ioctl(int fd, nuint request, byte* arg);
}
