using System.Buffers.Binary;
using System.Text;
using System.Threading.Channels;
using M0LTE.Uvk5.Protocol;

namespace M0LTE.Uvk5.Bootloader;

/// <summary>A bootloader beacon (0x0518 for bootloader 2.x, 0x057A for 5.x).</summary>
/// <param name="Id">Message id.</param>
/// <param name="Version">Bootloader version, for example <c>2.00.06</c>, null if the beacon has none.</param>
/// <param name="ChipId">The chip id bytes, hex.</param>
public sealed record BootloaderBeacon(ushort Id, string? Version, string ChipId)
{
    /// <summary>True for a bootloader 5.x beacon (AES flashing, not supported).</summary>
    public bool IsVersion5 => Id == MessageIds.BootBeaconV5;
}

/// <summary>Options for <see cref="K5Bootloader.FlashAsync"/>.</summary>
public sealed record FlashOptions
{
    /// <summary>Actually write. Default false: a dry run that listens for the beacon, checks compatibility and sends nothing.</summary>
    public bool ReallyFlash { get; init; }

    /// <summary>Bootloader versions accepted (default 2.00.06, the V1 hardware this firmware runs on).</summary>
    public IReadOnlyList<string> AllowedBootloaders { get; init; } = ["2.00.06"];

    /// <summary>How long to wait for the first beacon.</summary>
    public TimeSpan BeaconTimeout { get; init; } = TimeSpan.FromSeconds(10);

    /// <summary>How long to wait for each block's acknowledgement.</summary>
    public TimeSpan AckTimeout { get; init; } = TimeSpan.FromSeconds(3);

    /// <summary>Resends of a block whose ack timed out.</summary>
    public int Retries { get; init; } = 2;

    /// <summary>Stray beacons tolerated per block while waiting for its ack (K5TOOL allows 10).</summary>
    public int MaxStrayBeacons { get; init; } = 10;

    /// <summary>Sequence id for the write frames; random if null.</summary>
    public uint? SequenceId { get; init; }

    /// <summary>Called for the start and outcome of a real flash.</summary>
    public Action<K5AuditEntry>? Audit { get; init; }

    /// <summary>Clock for timeouts.</summary>
    public TimeProvider TimeProvider { get; init; } = TimeProvider.System;
}

/// <summary>Flash progress.</summary>
/// <param name="Stage">What is happening.</param>
/// <param name="BlocksDone">Blocks acknowledged.</param>
/// <param name="BlockCount">Blocks in all.</param>
public readonly record struct FlashProgress(string Stage, int BlocksDone, int BlockCount);

/// <summary>Result of <see cref="K5Bootloader.FlashAsync"/>.</summary>
/// <param name="Flashed">False for a dry run.</param>
/// <param name="Beacon">The beacon the bootloader sent.</param>
/// <param name="Blocks">Blocks in the image.</param>
public sealed record FlashResult(bool Flashed, BootloaderBeacon Beacon, int Blocks);

/// <summary>
/// Flashes firmware through the UV-K5 bootloader (radio powered on with PTT held). Dry run by
/// default. Follows the vendor updater's frames as captured by K5TOOL: the bootloader repeats a
/// 0x0518 beacon until the host sends 0x0530 with the image version, then acknowledges each
/// 0x0519 block with 0x051A.
/// </summary>
/// <remarks>
/// <para>Quirks handled, all seen on the bench radio's bootloader 2.00.06 or in K5TOOL: the CRC
/// field of bootloader frames is raw <c>FF FF</c> on the wire (not obfuscated); stale beacons are
/// already buffered when flashing starts, so they are dropped first; the beacon answering 0x0530
/// may be one that was already in flight, so the real answer and further beacons turn up while
/// waiting for block acks and up to <see cref="FlashOptions.MaxStrayBeacons"/> are skipped per
/// block; a rejection ack carries chunk 0 whatever block it refers to, so any ack with a non-zero
/// result is taken as this block's refusal.</para>
/// <para>Safety: the image must look like a DP32G030 vector table and end below 0xF000; only
/// allowed bootloader versions with a 0x0518 beacon are accepted (5.x wants AES and is refused;
/// V2 and V3 hardware has another MCU); the image's version must be a <c>*</c> wildcard or match
/// the bootloader's major version.</para>
/// </remarks>
public sealed class K5Bootloader : IAsyncDisposable
{
    private readonly Stream _stream;
    private readonly bool _ownsStream;
    private readonly TimeProvider _time;
    private readonly K5FrameDecoder _decoder = new(obfuscated: true);
    private readonly Channel<(K5Frame Frame, long At)> _frames = Channel.CreateUnbounded<(K5Frame, long)>();
    private readonly CancellationTokenSource _stopping = new();
    private readonly Task _reader;
    private long _drainedAt;

    /// <summary>Creates a flasher over an open stream (38400 8N1).</summary>
    public K5Bootloader(Stream stream, bool ownsStream = false, TimeProvider? timeProvider = null)
    {
        _stream = stream;
        _ownsStream = ownsStream;
        _time = timeProvider ?? TimeProvider.System;
        _reader = Task.Run(() => ReadLoopAsync(_stopping.Token));
    }

    /// <summary>Opens a serial port (see <see cref="K5SerialPort"/>) for flashing.</summary>
    public static K5Bootloader OpenSerial(string portName, TimeProvider? timeProvider = null) =>
        new(K5SerialPort.Open(portName), ownsStream: true, timeProvider);

    /// <summary>Waits for a bootloader beacon.</summary>
    public async Task<BootloaderBeacon> WaitForBeaconAsync(TimeSpan timeout, CancellationToken cancellationToken = default)
    {
        K5Frame f = await ExpectAsync(id => id is MessageIds.BootBeacon or MessageIds.BootBeaconV5, _ => true, timeout, 0, "a bootloader beacon", cancellationToken).ConfigureAwait(false);
        return ParseBeacon(f);
    }

    /// <summary>
    /// Checks <paramref name="image"/> and the bootloader, and (only with
    /// <see cref="FlashOptions.ReallyFlash"/>) writes it.
    /// </summary>
    public async Task<FlashResult> FlashAsync(FirmwareImage image, FlashOptions? options = null, IProgress<FlashProgress>? progress = null, CancellationToken cancellationToken = default)
    {
        ArgumentNullException.ThrowIfNull(image);
        options ??= new FlashOptions();
        image.Validate();
        progress?.Report(new FlashProgress("waiting for the bootloader beacon", 0, image.BlockCount));
        BootloaderBeacon beacon = await WaitForBeaconAsync(options.BeaconTimeout, cancellationToken).ConfigureAwait(false);
        CheckCompatibility(beacon, image, options.AllowedBootloaders);
        int n = image.BlockCount;
        if (!options.ReallyFlash)
        {
            progress?.Report(new FlashProgress("dry run: bootloader compatible, nothing sent", 0, n));
            return new FlashResult(false, beacon, n);
        }

        string outcome = "started";
        int done = 0;
        Audit(options, $"K5 flash {image.Version} ({n} blocks) starting", outcome, image, beacon, 0);
        try
        {
            // Stale beacons are buffered; drop them, then say which firmware is coming.
            _drainedAt = _time.GetTimestamp();
            await SendAsync(MessageIds.BootVersion, image.VersionField.ToArray(), cancellationToken).ConfigureAwait(false);
            await ExpectAsync(id => id == MessageIds.BootBeacon, _ => true, options.AckTimeout, 0, "the beacon answering 0x0530", cancellationToken).ConfigureAwait(false);

            uint seq = options.SequenceId ?? (uint)Random.Shared.NextInt64(0, uint.MaxValue);
            for (int chunk = 0; chunk < n; chunk++)
            {
                byte[] body = MakeWriteBody(seq, chunk, n, image.Raw.Span.Slice(chunk * FirmwareImage.BlockSize, Math.Min(FirmwareImage.BlockSize, image.Raw.Length - chunk * FirmwareImage.BlockSize)));
                K5Frame ack = default;
                for (int attempt = 0; ; attempt++)
                {
                    await SendAsync(MessageIds.BootWrite, body, cancellationToken).ConfigureAwait(false);
                    try
                    {
                        int c = chunk;
                        ack = await ExpectAsync(id => id == MessageIds.BootWriteAck,
                            f => f.Body.Length >= 7 && (BinaryPrimitives.ReadUInt16LittleEndian(f.Body.Span[4..]) == c || f.Body.Span[6] != 0),
                            options.AckTimeout, options.MaxStrayBeacons, $"the ack for block {chunk}", cancellationToken).ConfigureAwait(false);
                        break;
                    }
                    catch (K5TimeoutException) when (attempt < options.Retries)
                    {
                        progress?.Report(new FlashProgress($"block {chunk}: no ack, resending", done, n));
                    }
                }

                ReadOnlySpan<byte> a = ack.Body.Span;
                uint rseq = BinaryPrimitives.ReadUInt32LittleEndian(a);
                ushort rchunk = BinaryPrimitives.ReadUInt16LittleEndian(a[4..]);
                byte result = a[6];
                if (result != 0)
                {
                    throw new K5ProtocolException($"bootloader refused block {chunk} (result {result}, ack names chunk {rchunk})");
                }

                if (rseq != seq)
                {
                    throw new K5ProtocolException($"ack for block {chunk} carries sequence 0x{rseq:X8}, sent 0x{seq:X8}");
                }

                done = chunk + 1;
                progress?.Report(new FlashProgress("writing", done, n));
            }

            outcome = "ok";
            return new FlashResult(true, beacon, n);
        }
        catch (Exception e)
        {
            outcome = $"{e.GetType().Name}: {e.Message}";
            throw;
        }
        finally
        {
            Audit(options, $"K5 flash {image.Version}: {done} of {n} blocks acknowledged", outcome, image, beacon, done);
        }
    }

    /// <inheritdoc/>
    public async ValueTask DisposeAsync()
    {
        if (_stopping.IsCancellationRequested)
        {
            return;
        }

        _stopping.Cancel();
        if (_ownsStream)
        {
            await _stream.DisposeAsync().ConfigureAwait(false);
        }

        try
        {
            await _reader.ConfigureAwait(false);
        }
        catch (Exception)
        {
        }

        _stopping.Dispose();
    }

    /// <summary>The compatibility rules (public for tests and for tools that want to explain a refusal).</summary>
    /// <exception cref="K5SafetyException">Not compatible.</exception>
    public static void CheckCompatibility(BootloaderBeacon beacon, FirmwareImage image, IReadOnlyList<string> allowedVersions)
    {
        ArgumentNullException.ThrowIfNull(beacon);
        ArgumentNullException.ThrowIfNull(image);
        if (beacon.IsVersion5)
        {
            throw new K5SafetyException($"bootloader 5.x beacon (0x057A, version {beacon.Version}): needs AES flashing, not supported");
        }

        if (beacon.Id != MessageIds.BootBeacon)
        {
            throw new K5SafetyException($"unexpected beacon id 0x{beacon.Id:X4}");
        }

        if (string.IsNullOrEmpty(beacon.Version))
        {
            throw new K5SafetyException("beacon without a version string (very old bootloader): refused");
        }

        if (!allowedVersions.Contains(beacon.Version))
        {
            throw new K5SafetyException(
                $"bootloader {beacon.Version} is not in the allowed list [{string.Join(", ", allowedVersions)}]. Newer K5 (V2, V3 hardware) uses other MCUs and this firmware will not run there.");
        }

        string v = image.Version;
        if (v.Length > 0 && v[0] != '*' && v[0] != beacon.Version[0])
        {
            throw new K5SafetyException($"image version '{v}' is not a '*' wildcard and does not match bootloader {beacon.Version}");
        }
    }

    internal static byte[] MakeWriteBody(uint seq, int chunk, int chunkCount, ReadOnlySpan<byte> data)
    {
        if (data.Length > FirmwareImage.BlockSize || chunk < 0 || chunk >= chunkCount || chunkCount * FirmwareImage.BlockSize > FirmwareImage.FlashLimit)
        {
            throw new K5SafetyException($"bad flash block {chunk}/{chunkCount} length {data.Length}");
        }

        var body = new byte[12 + FirmwareImage.BlockSize];
        BinaryPrimitives.WriteUInt32LittleEndian(body, seq);
        BinaryPrimitives.WriteUInt16LittleEndian(body.AsSpan(4), (ushort)chunk);
        BinaryPrimitives.WriteUInt16LittleEndian(body.AsSpan(6), (ushort)chunkCount);
        BinaryPrimitives.WriteUInt16LittleEndian(body.AsSpan(8), (ushort)data.Length);
        data.CopyTo(body.AsSpan(12));
        return body;
    }

    internal static BootloaderBeacon ParseBeacon(K5Frame f)
    {
        ReadOnlySpan<byte> p = f.Payload.Span;
        string chip = p.Length >= 20 ? Convert.ToHexStringLower(p[4..20]) : string.Empty;
        string? version = null;
        if (p.Length > 20)
        {
            ReadOnlySpan<byte> v = p[20..Math.Min(36, p.Length)];
            int end = v.IndexOf((byte)0);
            version = Encoding.ASCII.GetString(end < 0 ? v : v[..end]);
        }

        return new BootloaderBeacon(f.Id, string.IsNullOrEmpty(version) ? null : version, chip);
    }

    private static void Audit(FlashOptions o, string what, string outcome, FirmwareImage image, BootloaderBeacon beacon, int acked) =>
        o.Audit?.Invoke(new K5AuditEntry(K5AuditKind.Flash, what, outcome, new Dictionary<string, string>
        {
            ["version"] = image.Version,
            ["raw_sha256"] = image.Sha256,
            ["bootloader"] = beacon.Version ?? string.Empty,
            ["blocks"] = image.BlockCount.ToString(System.Globalization.CultureInfo.InvariantCulture),
            ["blocks_acked"] = acked.ToString(System.Globalization.CultureInfo.InvariantCulture),
        }));

    private async Task SendAsync(ushort id, byte[] body, CancellationToken ct)
    {
        byte[] frame = K5FrameCodec.Encode(id, body, obfuscate: true);
        await _stream.WriteAsync(frame, ct).ConfigureAwait(false);
        await _stream.FlushAsync(ct).ConfigureAwait(false);
    }

    private async Task<K5Frame> ExpectAsync(Func<ushort, bool> wantId, Func<K5Frame, bool> accept, TimeSpan timeout, int ignoreBeacons, string what, CancellationToken ct)
    {
        using var cts = CancellationTokenSource.CreateLinkedTokenSource(ct);
        using var timer = _time.CreateTimer(_ => cts.Cancel(), null, timeout, Timeout.InfiniteTimeSpan);
        int beacons = 0;
        try
        {
            while (true)
            {
                var (f, at) = await _frames.Reader.ReadAsync(cts.Token).ConfigureAwait(false);
                if (at < _drainedAt || f.Crc == CrcStatus.Bad)
                {
                    continue;
                }

                if (wantId(f.Id) && accept(f))
                {
                    return f;
                }

                if (f.Id is MessageIds.BootBeacon or MessageIds.BootBeaconV5)
                {
                    beacons++;
                    if (beacons > ignoreBeacons)
                    {
                        throw new K5ProtocolException(ignoreBeacons > 0
                            ? $"more than {ignoreBeacons} bootloader beacons while waiting for {what}"
                            : $"a bootloader beacon while waiting for {what}");
                    }
                }
            }
        }
        catch (OperationCanceledException) when (!ct.IsCancellationRequested)
        {
            throw new K5TimeoutException($"no {what} within {timeout.TotalSeconds:F1} s");
        }
    }

    private async Task ReadLoopAsync(CancellationToken ct)
    {
        var buf = new byte[512];
        try
        {
            while (!ct.IsCancellationRequested)
            {
                int n = await _stream.ReadAsync(buf, ct).ConfigureAwait(false);
                if (n == 0)
                {
                    break;
                }

                long now = _time.GetTimestamp();
                foreach (var f in _decoder.Feed(buf.AsSpan(0, n)))
                {
                    _frames.Writer.TryWrite((f, now));
                }
            }
        }
        catch (Exception) when (ct.IsCancellationRequested)
        {
        }
        catch (Exception e)
        {
            _frames.Writer.TryComplete(e);
            return;
        }

        _frames.Writer.TryComplete();
    }
}
