using System.Security.Cryptography;
using System.Text;
using System.Text.Json;

namespace M0LTE.Uvk5;

/// <summary>
/// A verified 8 KiB EEPROM image of one radio. EEPROM writes need one (see
/// <see cref="K5Radio.AuthorizeEepromWritesAsync"/>). The file format is k5.py's: the image, a
/// <c>.sha256</c> beside it (<c>digest  name</c>) and a <c>.json</c> with metadata, so backups made
/// by either tool work with the other.
/// </summary>
public sealed class EepromBackup
{
    private readonly byte[] _data;

    private EepromBackup(byte[] data, string? firmwareVersion, DateTimeOffset? takenAt)
    {
        K5Safety.CheckBackupContent(data);
        _data = data;
        FirmwareVersion = firmwareVersion;
        TakenAt = takenAt;
        Sha256 = Convert.ToHexStringLower(SHA256.HashData(data));
    }

    /// <summary>The image, 0x0000 to 0x1FFF.</summary>
    public ReadOnlyMemory<byte> Data => _data;

    /// <summary>The factory calibration area, 0x1E00 to 0x1FFF: identifies the radio.</summary>
    public ReadOnlyMemory<byte> Calibration => _data.AsMemory(K5Safety.CalibrationStart);

    /// <summary>SHA-256 of the image, lower-case hex.</summary>
    public string Sha256 { get; }

    /// <summary>Firmware version the radio reported when the backup was taken, if known.</summary>
    public string? FirmwareVersion { get; }

    /// <summary>When the backup was taken, if known.</summary>
    public DateTimeOffset? TakenAt { get; }

    /// <summary>Wraps an image read from a radio (checked for size and blankness).</summary>
    public static EepromBackup FromImage(ReadOnlySpan<byte> image, string? firmwareVersion = null, DateTimeOffset? takenAt = null) =>
        new(image.ToArray(), firmwareVersion, takenAt);

    /// <summary>
    /// Loads a backup and verifies it against the <c>.sha256</c> file beside it. Throws
    /// <see cref="K5SafetyException"/> if the checksum file is missing or does not match, or the
    /// image is not a usable backup.
    /// </summary>
    public static async Task<EepromBackup> LoadAsync(string path, CancellationToken cancellationToken = default)
    {
        byte[] data;
        string recorded;
        try
        {
            data = await File.ReadAllBytesAsync(path, cancellationToken).ConfigureAwait(false);
            recorded = (await File.ReadAllTextAsync(path + ".sha256", cancellationToken).ConfigureAwait(false))
                .Split((char[]?)null, StringSplitOptions.RemoveEmptyEntries).FirstOrDefault() ?? string.Empty;
        }
        catch (IOException e)
        {
            throw new K5SafetyException($"no usable EEPROM backup at {path} ({e.Message})");
        }
        catch (UnauthorizedAccessException e)
        {
            throw new K5SafetyException($"no usable EEPROM backup at {path} ({e.Message})");
        }

        if (data.Length != K5Safety.EepromSize)
        {
            throw new K5SafetyException($"backup {path} is {data.Length} bytes, expected {K5Safety.EepromSize}");
        }

        string actual = Convert.ToHexStringLower(SHA256.HashData(data));
        if (!string.Equals(actual, recorded, StringComparison.OrdinalIgnoreCase))
        {
            throw new K5SafetyException($"backup {path} does not match its .sha256");
        }

        string? version = null;
        DateTimeOffset? taken = null;
        try
        {
            using var doc = JsonDocument.Parse(await File.ReadAllBytesAsync(path + ".json", cancellationToken).ConfigureAwait(false));
            if (doc.RootElement.TryGetProperty("firmware", out var fw) && fw.ValueKind == JsonValueKind.String)
            {
                version = fw.GetString();
            }

            if (doc.RootElement.TryGetProperty("time", out var t) && t.ValueKind == JsonValueKind.String
                && DateTimeOffset.TryParse(t.GetString(), System.Globalization.CultureInfo.InvariantCulture, System.Globalization.DateTimeStyles.AssumeUniversal, out var parsed))
            {
                taken = parsed;
            }
        }
        catch (Exception e) when (e is IOException or JsonException)
        {
            // metadata is optional
        }

        return new EepromBackup(data, version, taken);
    }

    /// <summary>
    /// Writes the image, <c>path.sha256</c> and <c>path.json</c>. Refuses to overwrite an existing
    /// backup.
    /// </summary>
    public async Task SaveAsync(string path, CancellationToken cancellationToken = default)
    {
        if (File.Exists(path))
        {
            throw new IOException($"{path} exists; refusing to overwrite a backup");
        }

        string? dir = Path.GetDirectoryName(Path.GetFullPath(path));
        if (dir is not null)
        {
            Directory.CreateDirectory(dir);
        }

        await using (var f = new FileStream(path, FileMode.CreateNew, FileAccess.Write))
        {
            await f.WriteAsync(_data, cancellationToken).ConfigureAwait(false);
        }

        await File.WriteAllTextAsync(path + ".sha256", $"{Sha256}  {Path.GetFileName(path)}\n", cancellationToken).ConfigureAwait(false);

        using var ms = new MemoryStream();
        await using (var w = new Utf8JsonWriter(ms, new JsonWriterOptions { Indented = true }))
        {
            w.WriteStartObject();
            w.WriteString("sha256", Sha256);
            w.WriteNumber("size", _data.Length);
            if (FirmwareVersion is not null)
            {
                w.WriteString("firmware", FirmwareVersion);
            }
            else
            {
                w.WriteNull("firmware");
            }

            w.WriteString("time", (TakenAt ?? DateTimeOffset.UtcNow).ToUniversalTime().ToString("yyyy-MM-ddTHH:mm:ssZ", System.Globalization.CultureInfo.InvariantCulture));
            w.WriteNumber("reads_matched", 2);
            w.WriteString("tool", "M0LTE.Uvk5");
            w.WriteEndObject();
        }

        await File.WriteAllTextAsync(path + ".json", Encoding.UTF8.GetString(ms.ToArray()) + "\n", cancellationToken).ConfigureAwait(false);
    }
}
