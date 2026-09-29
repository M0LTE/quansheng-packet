using System.Buffers.Binary;
using M0LTE.Uvk5.Bootloader;
using M0LTE.Uvk5.Protocol;

namespace M0LTE.Uvk5.Simulation;

/// <summary>Options for <see cref="SimulatedBootloader"/>.</summary>
public sealed record SimulatedBootloaderOptions
{
    /// <summary>Clock.</summary>
    public TimeProvider TimeProvider { get; init; } = TimeProvider.System;

    /// <summary>Beacon period.</summary>
    public TimeSpan BeaconInterval { get; init; } = TimeSpan.FromMilliseconds(50);

    /// <summary>Behave as bootloader 5.x (0x057A beacon).</summary>
    public bool Version5 { get; init; }

    /// <summary>Refuse this block (ack result 1, naming chunk 0, as the real bootloader does).</summary>
    public int? RefuseBlock { get; init; }

    /// <summary>Drop the ack for this block once.</summary>
    public int? DropAckForBlock { get; init; }

    /// <summary>Keep beaconing after 0x0530 until the last block (stray beacons during block writes).</summary>
    public bool KeepBeaconing { get; init; }

    /// <summary>Hold back the beacon answering 0x0530 by this long, so a beacon already on the wire is taken as the answer.</summary>
    public TimeSpan VersionReplyDelay { get; init; }
}

/// <summary>
/// An in-process UV-K5 bootloader 2.00.06 (or 5.x) for testing flashers: beacons until 0x0530,
/// then acknowledges 0x0519 blocks with 0x051A. Frames carry a raw <c>FF FF</c> CRC, as the real
/// one does. Thread-safe.
/// </summary>
public sealed class SimulatedBootloader : IDisposable
{
    /// <summary>A captured bootloader 2.00.06 beacon payload (K5TOOL, identical to k5prog's comment).</summary>
    public static ReadOnlySpan<byte> BeaconV2 => Convert.FromHexString("18052000010202061c53504a3747ff0f8c005300322e30302e303600340a000000000020");

    private static readonly byte[] BeaconV5Payload = Convert.FromHexString("7a052000010202061c53504a3747ff1093008900352e30302e303100280c000000000020");

    private readonly SimulatedBootloaderOptions _options;
    private readonly Lock _gate = new();
    private readonly ByteQueue _toHost = new();
    private readonly K5FrameDecoder _decoder = new(obfuscated: true, maxPayload: 0x200);
    private readonly byte[] _flash = new byte[FirmwareImage.FlashLimit];
    private readonly List<(uint Seq, int Chunk, int Count, int Length)> _blocks = [];
    private readonly List<string> _violations = [];
    private readonly ITimer _beaconTimer;
    private bool _handshake;
    private bool _done;
    private bool _dropped;
    private bool _disposed;

    /// <summary>Creates the bootloader; it starts beaconing at once.</summary>
    public SimulatedBootloader(SimulatedBootloaderOptions? options = null)
    {
        _options = options ?? new SimulatedBootloaderOptions();
        Array.Fill(_flash, (byte)0xFF);
        HostStream = new LoopbackStream.End(_toHost, new SinkQueue(this));
        _beaconTimer = _options.TimeProvider.CreateTimer(_ => Beacon(periodic: true), null, TimeSpan.Zero, _options.BeaconInterval);
    }

    /// <summary>The host end of the link.</summary>
    public Stream HostStream { get; }

    /// <summary>The version sent with 0x0530, once received.</summary>
    public string? FlashVersion { get; private set; }

    /// <summary>True once the last block was written.</summary>
    public bool Finished
    {
        get
        {
            lock (_gate)
            {
                return _done;
            }
        }
    }

    /// <summary>Blocks written, in order.</summary>
    public IReadOnlyList<(uint Seq, int Chunk, int Count, int Length)> Blocks
    {
        get
        {
            lock (_gate)
            {
                return [.. _blocks];
            }
        }
    }

    /// <summary>Protocol violations (a block before 0x0530, a write into the bootloader area).</summary>
    public IReadOnlyList<string> Violations
    {
        get
        {
            lock (_gate)
            {
                return [.. _violations];
            }
        }
    }

    /// <summary>A copy of the simulated flash, 0 to 0xEFFF.</summary>
    public byte[] Flash
    {
        get
        {
            lock (_gate)
            {
                return (byte[])_flash.Clone();
            }
        }
    }

    /// <inheritdoc/>
    public void Dispose()
    {
        lock (_gate)
        {
            _disposed = true;
        }

        _beaconTimer.Dispose();
        _toHost.Complete();
    }

    private void Beacon(bool periodic)
    {
        lock (_gate)
        {
            if (_disposed || (periodic && _handshake && !(_options.KeepBeaconing && !_done)))
            {
                return;
            }

            SendRaw(_options.Version5 ? BeaconV5Payload : BeaconV2.ToArray());
        }
    }

    /// <summary>Encodes as the bootloader does: obfuscated payload, raw FF FF in the CRC field.</summary>
    private void SendRaw(byte[] payload)
    {
        byte[] frame = K5FrameCodec.Encode(BinaryPrimitives.ReadUInt16LittleEndian(payload), payload.AsSpan(4), obfuscate: true);
        frame[^4] = 0xFF;
        frame[^3] = 0xFF;
        _toHost.Write(frame);
    }

    private void OnHostBytes(ReadOnlySpan<byte> data)
    {
        lock (_gate)
        {
            if (_disposed)
            {
                return;
            }

            foreach (var f in _decoder.Feed(data))
            {
                if (f.Crc != CrcStatus.Valid)
                {
                    continue;
                }

                Handle(f);
            }
        }
    }

    private void Handle(K5Frame f)
    {
        ReadOnlySpan<byte> p = f.Payload.Span;
        if (f.Id == MessageIds.BootVersion)
        {
            FlashVersion = System.Text.Encoding.ASCII.GetString(p.Slice(4, Math.Min(16, p.Length - 4))).TrimEnd('\0');
            _handshake = true;
            if (_options.VersionReplyDelay > TimeSpan.Zero)
            {
                ITimer? t = null;
                t = _options.TimeProvider.CreateTimer(_ =>
                {
                    Beacon(periodic: false);
                    t?.Dispose();
                }, null, _options.VersionReplyDelay, Timeout.InfiniteTimeSpan);
            }
            else
            {
                SendRaw(_options.Version5 ? BeaconV5Payload : BeaconV2.ToArray());
            }

            return;
        }

        if (f.Id != MessageIds.BootWrite)
        {
            return;
        }

        if (!_handshake)
        {
            _violations.Add("flash block before the 0x0530 version message");
            return;
        }

        uint seq = BinaryPrimitives.ReadUInt32LittleEndian(p[4..]);
        int chunk = BinaryPrimitives.ReadUInt16LittleEndian(p[8..]);
        int count = BinaryPrimitives.ReadUInt16LittleEndian(p[10..]);
        int length = BinaryPrimitives.ReadUInt16LittleEndian(p[12..]);
        ReadOnlySpan<byte> data = p.Slice(16, Math.Min(FirmwareImage.BlockSize, p.Length - 16));
        _blocks.Add((seq, chunk, count, length));
        if (chunk == _options.DropAckForBlock && !_dropped)
        {
            _dropped = true;
            return;
        }

        bool refuse = chunk == _options.RefuseBlock;
        if (!refuse)
        {
            if ((chunk + 1) * FirmwareImage.BlockSize > FirmwareImage.FlashLimit)
            {
                _violations.Add("write into the bootloader area");
                return;
            }

            data.CopyTo(_flash.AsSpan(chunk * FirmwareImage.BlockSize));
        }

        var ack = new WireWriter().U32(seq).U16(refuse ? 0 : chunk).U8(refuse ? 1 : 0).U8(0).ToArray();
        SendRaw(K5FrameCodec.BuildPayload(MessageIds.BootWriteAck, ack));
        if (!refuse && chunk == count - 1)
        {
            _done = true;
        }
    }

    private sealed class SinkQueue(SimulatedBootloader owner) : IByteSink
    {
        public void Write(ReadOnlySpan<byte> data) => owner.OnHostBytes(data);

        public void Complete()
        {
        }
    }
}
