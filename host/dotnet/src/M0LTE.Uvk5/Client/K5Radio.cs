using System.Buffers.Binary;
using System.Runtime.CompilerServices;
using System.Text;
using System.Threading.Channels;
using M0LTE.Uvk5.Client;
using M0LTE.Uvk5.Protocol;

namespace M0LTE.Uvk5;

/// <summary>
/// A connection to a UV-K5 over its serial port (through an AIOC, typically). Works with stock
/// firmware (legacy commands), packet firmware v1 (legacy plus the EEPROM settings block) and
/// packet firmware with protocol v2 (live settings, events, exact PTT-lock knowledge).
/// </summary>
/// <remarks>
/// <para><b>The one rule that matters for a TNC.</b> On the AIOC's K1 plug, PTT and the
/// host-to-radio data line are one contact. Every frame the host sends starts the radio's serial
/// PTT lock, and any byte written while PTT is held releases PTT at once: <b>sending anything
/// while keyed ends the transmission.</b> So: key only when <see cref="IsSafeToKey"/> (or after
/// <see cref="WaitUntilSafeToKeyAsync"/>), and preferably inside a <see cref="TransmitWindow"/>
/// from <see cref="BeginTransmitAsync"/>, which also stops this library sending anything until the
/// window is closed.</para>
/// <para>Commands are serialised: one frame is in flight at a time, as the protocol requires.
/// All methods are safe to call from several tasks.</para>
/// <para>Events (v2) arrive on a background reader and are delivered through
/// <see cref="EventReceived"/> and <see cref="BusyChanged"/> (synchronously on the reader: keep
/// handlers fast) and through <see cref="ReadEventsAsync"/> (buffered per reader).</para>
/// </remarks>
public sealed partial class K5Radio : IAsyncDisposable
{
    private static readonly TimeSpan LinkGapFlush = TimeSpan.FromMilliseconds(50);
    private static readonly TimeSpan LegacyFrameGap = TimeSpan.FromMilliseconds(12);

    private readonly Stream _stream;
    private readonly bool _ownsStream;
    private readonly K5RadioOptions _options;
    private readonly TimeProvider _time;
    private readonly K5FrameDecoder _decoder;
    private readonly SemaphoreSlim _wire = new(1, 1);
    private readonly AsyncLocal<WindowBox?> _flowWindow = new();
    private readonly PttLockTracker _lock;
    private readonly EventSequencer _sequencer = new();
    private readonly Lock _gate = new();
    private readonly List<Channel<RadioEvent>> _readers = [];
    private readonly CancellationTokenSource _stopping = new();
    private readonly StringBuilder _bannerLine = new();
    private readonly uint _sessionId;
    private readonly Task _readerTask;

    private Pending? _pending;
    private TransmitWindow? _openWindow;
    private long _writesAllowedAt;
    private long _lastWriteDone;
    private long _lastRead;
    private byte _tag;
    private bool? _channelBusy;
    private bool _busySubscribed;
    private EventSubscription? _lastSubscription;
    private EepromBackup? _authorizedBackup;
    private ushort? _batteryCalibration;
    private TimeSpan _pttRelease = TimeSpan.FromMilliseconds(5);
    private long _lastHeartbeat;
    private int _recovering;
    private int _resubscribing;
    private bool _inBootloader;
    private Exception? _linkError;

    private K5Radio(Stream stream, bool ownsStream, K5RadioOptions options)
    {
        _stream = stream;
        _ownsStream = ownsStream;
        _options = options;
        _time = options.TimeProvider;
        _decoder = new K5FrameDecoder(obfuscated: true);
        _lock = new PttLockTracker(_time, options.V1KeyAllowance, options.StockKeyAllowance);
        Clock = new RadioClock(_time);
        _sessionId = options.SessionId ?? (uint)Random.Shared.Next();
        _writesAllowedAt = _time.GetTimestamp();
        Firmware = new FirmwareInfo { Kind = FirmwareKind.Unknown, Version = string.Empty };
        _readerTask = Task.Run(() => ReadLoopAsync(_stopping.Token));
    }

    /// <summary>What the radio said about itself at the last hello (and GET_INFO on v2).</summary>
    public FirmwareInfo Firmware { get; private set; }

    /// <summary>The radio clock estimate, fed by TIME_SYNC, heartbeats and exactly-timed events.</summary>
    public RadioClock Clock { get; }

    /// <summary>The deviation law used to show deviation registers in kHz.</summary>
    public DeviationLaw DeviationLaw => _options.DeviationLaw;

    /// <summary>
    /// The radio's busy (carrier detect) state from its events: true busy, false clear, null when
    /// unknown (not v2, not subscribed to <see cref="RadioEvents.Busy"/>, the link was lost, the
    /// radio rebooted, or heartbeats stopped). Null is a real answer: callers must not treat it as
    /// busy, or a lost cable would silence the station.
    /// </summary>
    public bool? ChannelBusy
    {
        get
        {
            lock (_gate)
            {
                if (_channelBusy is not null && HeartbeatStale())
                {
                    _channelBusy = null;
                }

                return _channelBusy;
            }
        }
    }

    /// <summary>Time until a PTT press would be honoured at once (zero if now).</summary>
    public TimeSpan TimeUntilSafeToKey
    {
        get
        {
            TimeSpan lockLeft = _lock.Remaining;
            return Volatile.Read(ref _pending) is not null || _wire.CurrentCount == 0 && _openWindow is null
                ? Max(lockLeft, TimeSpan.FromMilliseconds(1))
                : lockLeft;
        }
    }

    /// <summary>The earliest host time at which a PTT press is honoured at once, as things stand.</summary>
    public DateTimeOffset ReadyToKeyAt => _time.GetUtcNow() + TimeUntilSafeToKey;

    /// <summary>
    /// True if a PTT press now would key at once: the serial lock has run out and no frame is in
    /// flight. Another task may still send a command a moment later; use
    /// <see cref="BeginTransmitAsync"/> to hold the port quiet.
    /// </summary>
    public bool IsSafeToKey => TimeUntilSafeToKey == TimeSpan.Zero;

    /// <summary>True while a <see cref="TransmitWindow"/> is open.</summary>
    public bool IsTransmitWindowOpen => Volatile.Read(ref _openWindow) is not null;

    /// <summary>Every event, synchronously on the reader task. Keep handlers fast and non-blocking.</summary>
    public event EventHandler<RadioEvent>? EventReceived;

    /// <summary>Busy (carrier detect) edges, synchronously on the reader task. Keep handlers fast.</summary>
    public event EventHandler<BusyEvent>? BusyChanged;

    /// <summary>
    /// The radio was seen to reboot (power-on banner, or its clock and sequence numbers restarting).
    /// Its subscription, live settings and serial mode are back to power-on state. With
    /// <see cref="K5RadioOptions.ResubscribeAfterReboot"/> the library says hello and resubscribes
    /// by itself.
    /// </summary>
    public event EventHandler? Rebooted;

    /// <summary>
    /// Opens a serial port (38400 8N1, DTR and RTS low, no breaks: see <see cref="K5SerialPort"/>)
    /// and connects.
    /// </summary>
    public static async Task<K5Radio> OpenSerialAsync(string portName, K5RadioOptions? options = null, CancellationToken cancellationToken = default)
    {
        Stream stream = K5SerialPort.Open(portName);
        try
        {
            return await ConnectAsync(stream, options, ownsStream: true, cancellationToken).ConfigureAwait(false);
        }
        catch
        {
            await stream.DisposeAsync().ConfigureAwait(false);
            throw;
        }
    }

    /// <summary>
    /// Connects over an open duplex stream: says hello, identifies the firmware and, on v2, reads
    /// GET_INFO. Throws <see cref="K5TimeoutException"/> if the radio does not answer.
    /// </summary>
    /// <param name="stream">A duplex byte stream to the radio (a serial port, a loopback to a simulator, a socket).</param>
    /// <param name="options">Options.</param>
    /// <param name="ownsStream">Dispose the stream with the radio.</param>
    /// <param name="cancellationToken">Cancellation.</param>
    public static async Task<K5Radio> ConnectAsync(Stream stream, K5RadioOptions? options = null, bool ownsStream = false, CancellationToken cancellationToken = default)
    {
        var radio = new K5Radio(stream, ownsStream, options ?? new K5RadioOptions());
        try
        {
            await radio.IdentifyAsync(cancellationToken).ConfigureAwait(false);
            return radio;
        }
        catch
        {
            await radio.DisposeAsync().ConfigureAwait(false);
            throw;
        }
    }

    /// <summary>
    /// Waits until a PTT press would be honoured at once. Does not stop other tasks sending
    /// commands afterwards: prefer <see cref="BeginTransmitAsync"/>.
    /// </summary>
    public async Task WaitUntilSafeToKeyAsync(CancellationToken cancellationToken = default)
    {
        while (true)
        {
            TimeSpan wait = TimeUntilSafeToKey;
            if (wait <= TimeSpan.Zero)
            {
                return;
            }

            await Task.Delay(wait, _time, cancellationToken).ConfigureAwait(false);
        }
    }

    /// <summary>
    /// Waits for any command in flight and for the serial PTT lock to run out, then holds the port
    /// quiet until the returned window is disposed. Key PTT after this returns; release PTT, then
    /// dispose the window.
    /// </summary>
    /// <remarks>
    /// Commands from other tasks wait while the window is open. A command from inside the window's
    /// own async flow throws <see cref="K5TransmitInProgressException"/>. On v1 the wait can be up
    /// to 2 s after the last command, on stock 6.5 s.
    /// </remarks>
    public Task<TransmitWindow> BeginTransmitAsync(CancellationToken cancellationToken = default)
    {
        // Set synchronously so the marker flows back to the caller's async flow.
        WindowBox? box = _flowWindow.Value;
        if (box is null)
        {
            box = new WindowBox();
            _flowWindow.Value = box;
        }

        if (box.Window is { IsOpen: true })
        {
            throw new K5TransmitInProgressException("a transmit window is already open in this flow");
        }

        return BeginTransmitCoreAsync(box, cancellationToken);
    }

    /// <summary>Subscribes a buffered reader to events. Each call gets its own copy of every event from now on.</summary>
    public async IAsyncEnumerable<RadioEvent> ReadEventsAsync([EnumeratorCancellation] CancellationToken cancellationToken = default)
    {
        var channel = Channel.CreateBounded<RadioEvent>(new BoundedChannelOptions(_options.EventBufferCapacity)
        {
            FullMode = BoundedChannelFullMode.DropOldest,
            SingleReader = true,
            SingleWriter = true,
        });
        lock (_gate)
        {
            _readers.Add(channel);
        }

        try
        {
            await foreach (var e in channel.Reader.ReadAllAsync(cancellationToken).ConfigureAwait(false))
            {
                yield return e;
            }
        }
        finally
        {
            lock (_gate)
            {
                _readers.Remove(channel);
            }
        }
    }

    /// <summary>Stops the reader and, if owned, closes the stream. Sends nothing to the radio.</summary>
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
            // A stream we do not own may not honour cancellation of a pending read; do not hang on it.
            await _readerTask.WaitAsync(TimeSpan.FromSeconds(1)).ConfigureAwait(false);
        }
        catch (Exception)
        {
            // the reader ends with the stream
        }

        FailPending(new ObjectDisposedException(nameof(K5Radio)));
        lock (_gate)
        {
            foreach (var r in _readers)
            {
                r.Writer.TryComplete();
            }
        }

        _stopping.Dispose();
    }

    // ------------------------------------------------------------------ transmit windows

    private async Task<TransmitWindow> BeginTransmitCoreAsync(WindowBox box, CancellationToken ct)
    {
        long start = _time.GetTimestamp();
        await _wire.WaitAsync(ct).ConfigureAwait(false);
        try
        {
            while (true)
            {
                TimeSpan wait = _lock.Remaining;
                if (wait <= TimeSpan.Zero)
                {
                    break;
                }

                await Task.Delay(wait, _time, ct).ConfigureAwait(false);
            }
        }
        catch
        {
            _wire.Release();
            throw;
        }

        var window = new TransmitWindow(CloseWindow, _time.GetUtcNow(), _time.GetElapsedTime(start), ChannelBusy);
        box.Window = window;
        Volatile.Write(ref _openWindow, window);
        return window;
    }

    private void CloseWindow(TransmitWindow window)
    {
        // The radio must see PTT released (release debounce) before the next byte, or the byte
        // itself ends the transmission and TX_END says SERIAL rather than RELEASED.
        long guard = _time.GetTimestamp() + Ticks(_pttRelease + TimeSpan.FromMilliseconds(2));
        Interlocked.Exchange(ref _writesAllowedAt, guard);
        Interlocked.CompareExchange(ref _openWindow, null, window);
        _wire.Release();
        ScheduleRecoveryIfNeeded();
    }

    // ------------------------------------------------------------------ exchange plumbing

    private sealed class WindowBox
    {
        public TransmitWindow? Window;
    }

    private sealed class Pending(Func<K5Frame, bool> match)
    {
        public Func<K5Frame, bool> Match { get; } = match;

        public TaskCompletionSource<K5Frame> Reply { get; } = new(TaskCreationOptions.RunContinuationsAsynchronously);
    }

    private void ThrowIfUnusable(string operation)
    {
        ObjectDisposedException.ThrowIf(_stopping.IsCancellationRequested, this);
        if (_flowWindow.Value?.Window is { IsOpen: true })
        {
            throw new K5TransmitInProgressException(
                $"{operation} would send a frame while this flow holds a transmit window: on the AIOC any serial write releases PTT and ends the transmission. Dispose the window (after unkeying) first.");
        }

        if (_linkError is not null)
        {
            throw new K5Exception("the link to the radio has closed", _linkError);
        }

        if (_inBootloader)
        {
            throw new K5Exception("the radio is in its bootloader (beacons seen): use K5Bootloader, or power-cycle it normally");
        }
    }

    /// <summary>
    /// Sends one frame and, if <paramref name="match"/> is given, waits for the first frame it
    /// accepts. Holds the wire for the whole exchange. Returns null if no reply was expected.
    /// </summary>
    private async Task<K5Frame?> ExchangeAsync(string operation, ushort id, byte[] body, Func<K5Frame, bool>? match, TimeSpan timeout, CancellationToken ct)
    {
        ThrowIfUnusable(operation);
        if (id == MessageIds.ReservedSerialKey)
        {
            throw new K5SafetyException("serial keying (0x5020) is not part of this library");
        }

        await _wire.WaitAsync(ct).ConfigureAwait(false);
        try
        {
            ThrowIfUnusable(operation);
            await WaitForWriteGuardsAsync(ct).ConfigureAwait(false);
            byte[] frame = K5FrameCodec.Encode(id, body, _decoder.Obfuscated);
            Pending? pending = match is null ? null : new Pending(match);
            Volatile.Write(ref _pending, pending);
            try
            {
                _options.Trace?.Invoke($"TX {id:X4} {Convert.ToHexString(body)}");
                await _stream.WriteAsync(frame, ct).ConfigureAwait(false);
                await _stream.FlushAsync(ct).ConfigureAwait(false);
                Interlocked.Exchange(ref _lastWriteDone, _time.GetTimestamp());
                _lock.OnFrameWritten(frame.Length);
                if (pending is null)
                {
                    return null;
                }

                try
                {
                    return await pending.Reply.Task.WaitAsync(timeout, _time, ct).ConfigureAwait(false);
                }
                catch (TimeoutException)
                {
                    throw new K5TimeoutException($"{operation}: no reply within {timeout.TotalMilliseconds:F0} ms");
                }
            }
            finally
            {
                Interlocked.CompareExchange(ref _pending, null, pending);
            }
        }
        finally
        {
            _wire.Release();
        }
    }

    private async Task WaitForWriteGuardsAsync(CancellationToken ct)
    {
        while (true)
        {
            long now = _time.GetTimestamp();
            long until = Interlocked.Read(ref _writesAllowedAt);
            if (Firmware.Kind != FirmwareKind.PacketV2)
            {
                // v1 and stock handle one frame per 10 ms slice: keep frames more than 10 ms apart.
                until = Math.Max(until, Interlocked.Read(ref _lastWriteDone) + Ticks(LegacyFrameGap));
            }

            if (until <= now)
            {
                return;
            }

            await Task.Delay(_time.GetElapsedTime(now, until), _time, ct).ConfigureAwait(false);
        }
    }

    private long Ticks(TimeSpan t) => (long)(t.TotalSeconds * _time.TimestampFrequency);

    private static TimeSpan Max(TimeSpan a, TimeSpan b) => a > b ? a : b;

    /// <summary>A v2 command: tag, one retry where safe, status check. Returns the reply body after the 4-byte header.</summary>
    private async Task<byte[]> V2Async(string operation, ushort id, byte[] args, bool retryable, CancellationToken ct, TimeSpan? timeout = null)
    {
        RequireV2(operation);
        byte tag;
        lock (_gate)
        {
            tag = ++_tag;
        }

        var body = new byte[args.Length + 1];
        body[0] = tag;
        args.CopyTo(body, 1);
        ushort replyId = (ushort)(id + MessageIds.ReplyOffset);
        bool Match(K5Frame f) => f.Id == replyId && f.Body.Length >= 4 && f.Body.Span[0] == tag;

        int attempts = retryable ? 2 : 1;
        for (int attempt = 1; ; attempt++)
        {
            try
            {
                K5Frame reply = (await ExchangeAsync(operation, id, body, Match, timeout ?? _options.V2ReplyTimeout, ct).ConfigureAwait(false))!.Value;
                ReadOnlySpan<byte> b = reply.Body.Span;
                var status = (K5Status)b[1];
                if (status != K5Status.Ok)
                {
                    throw new K5CommandRejectedException(operation, status, b.Length > 4 ? b[4] : (byte)0);
                }

                return b[4..].ToArray();
            }
            catch (K5TimeoutException) when (attempt < attempts)
            {
                _options.Trace?.Invoke($"{operation}: timeout, retrying once");
            }
        }
    }

    private async Task<K5Frame> LegacyAsync(string operation, ushort id, byte[] body, Func<K5Frame, bool> match, CancellationToken ct, TimeSpan? timeout = null)
    {
        K5Frame? f = await ExchangeAsync(operation, id, body, match, timeout ?? _options.LegacyReplyTimeout, ct).ConfigureAwait(false);
        return f!.Value;
    }

    private void RequireV2(string operation)
    {
        if (Firmware.Kind != FirmwareKind.PacketV2)
        {
            throw new K5FirmwareNotSupportedException(operation, Firmware.Kind, "packet firmware with protocol v2");
        }
    }

    // ------------------------------------------------------------------ reader

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
                    throw new EndOfStreamException("the serial stream ended");
                }

                long now = _time.GetTimestamp();
                var frames = new List<K5Frame>();
                if (_decoder.HasPartialFrame && _lastRead != 0 && _time.GetElapsedTime(_lastRead, now) > LinkGapFlush)
                {
                    // A frame's bytes arrive back to back; after a long gap the partial frame was cut.
                    frames.AddRange(_decoder.DropPartialFrame(OnDiscarded));
                }

                _lastRead = now;
                frames.AddRange(_decoder.Feed(buf.AsSpan(0, n), OnDiscarded));
                foreach (var f in frames)
                {
                    Dispatch(f, now);
                }
            }
        }
        catch (OperationCanceledException) when (ct.IsCancellationRequested)
        {
        }
        catch (ObjectDisposedException) when (ct.IsCancellationRequested)
        {
        }
        catch (Exception e)
        {
            _linkError = e;
            lock (_gate)
            {
                _channelBusy = null;
            }

            FailPending(new K5Exception("the link to the radio has closed", e));
        }
    }

    private void FailPending(Exception e) => Volatile.Read(ref _pending)?.Reply.TrySetException(e);

    private void Dispatch(K5Frame f, long arrival)
    {
        _options.Trace?.Invoke($"RX {f.Id:X4} {Convert.ToHexString(f.Body.Span)} crc={f.Crc}");
        if (f.Crc == CrcStatus.Bad || (MessageIds.IsV2(f.Id) && f.Crc != CrcStatus.Valid))
        {
            return;
        }

        if (MessageIds.IsEvent(f.Id))
        {
            HandleEvent(f, arrival);
            return;
        }

        if (f.Id is MessageIds.BootBeacon or MessageIds.BootBeaconV5)
        {
            _inBootloader = true;
            FailPending(new K5Exception("the radio is in its bootloader (beacon 0x" + f.Id.ToString("X4", System.Globalization.CultureInfo.InvariantCulture) + " seen)"));
            return;
        }

        Pending? p = Volatile.Read(ref _pending);
        if (p is not null && p.Match(f))
        {
            if (MessageIds.IsV2(f.Id))
            {
                _lock.OnV2Reply(BinaryPrimitives.ReadUInt16LittleEndian(f.Body.Span[2..]));
            }
            else
            {
                _lock.OnLegacyReply();
            }

            p.Reply.TrySetResult(f);
        }
    }

    private void OnDiscarded(ReadOnlySpan<byte> bytes)
    {
        foreach (byte b in bytes)
        {
            if (b == '\n')
            {
                string line = _bannerLine.ToString().TrimEnd('\r');
                _bannerLine.Clear();
                if (line.StartsWith("UV-K5 packet firmware", StringComparison.Ordinal))
                {
                    OnRadioRebooted($"banner: {line}");
                }
            }
            else if (b is >= 0x20 and < 0x7F or (byte)'\r')
            {
                if (_bannerLine.Length < 120)
                {
                    _bannerLine.Append((char)b);
                }
            }
            else
            {
                _bannerLine.Clear();
            }
        }
    }

    private void HandleEvent(K5Frame f, long arrival)
    {
        RadioEvent e;
        try
        {
            e = EventParser.Parse(f.Id, f.Body.Span, _options.DeviationLaw, _time.GetUtcNow());
        }
        catch (K5ProtocolException ex)
        {
            _options.Trace?.Invoke($"event 0x{f.Id:X4} dropped: {ex.Message}");
            return;
        }

        var verdict = _sequencer.Accept(e);
        if (verdict == EventSequencer.Verdict.Duplicate)
        {
            return;
        }

        if (verdict == EventSequencer.Verdict.Rebooted)
        {
            OnRadioRebooted("event sequence and clock restarted");
        }

        // Clock: an exactly-timed frame left the radio within 1 ms of its time stamp.
        if (!e.IsReplay && (e.Flags & RadioEventFlags.TimeExact) != 0)
        {
            double t = e.RadioTimeMs + (e is HeartbeatEvent hb ? hb.Microseconds / 1000.0 : 0);
            long sentBy = arrival - Ticks(TimeSpan.FromMicroseconds(256 * f.WireLength));
            Clock.AddUpperBound(t, sentBy);
        }

        e = e with { EstimatedTime = Clock.ToHostTime(e.RadioTimeMs) };

        switch (e)
        {
            case BusyEvent be when !e.IsReplay:
                lock (_gate)
                {
                    _channelBusy = be.Busy;
                }

                break;
            case HeartbeatEvent hbe:
                lock (_gate)
                {
                    _lastHeartbeat = arrival;
                    if (_busySubscribed)
                    {
                        _channelBusy = (hbe.Status & StatusFlags.Busy) != 0;
                    }
                }

                break;
            case TxEndEvent when !e.IsReplay:
                // The release was seen: writing is safe again.
                Interlocked.Exchange(ref _writesAllowedAt, arrival);
                break;
            case BootEvent:
                if (verdict != EventSequencer.Verdict.Rebooted)
                {
                    OnRadioRebooted("BOOT event");
                }

                break;
        }

        Publish(e);
        if (e is BusyEvent busy && !e.IsReplay)
        {
            Raise(BusyChanged, busy, nameof(BusyChanged));
        }

        if (verdict == EventSequencer.Verdict.Gap)
        {
            ScheduleRecoveryIfNeeded();
        }
    }

    private void Publish(RadioEvent e)
    {
        Raise(EventReceived, e, nameof(EventReceived));

        lock (_gate)
        {
            foreach (var r in _readers)
            {
                r.Writer.TryWrite(e);
            }
        }
    }

    private bool HeartbeatStale()
    {
        if (_lastSubscription?.HeartbeatPeriod is not { } period || _lastHeartbeat == 0 || _openWindow is not null)
        {
            return false;
        }

        // Heartbeats are held while PTT is asserted, so allow for a long transmission after a window.
        TimeSpan allowed = period * 3 + TimeSpan.FromSeconds(2);
        return _time.GetElapsedTime(_lastHeartbeat) > allowed && _time.GetElapsedTime(Interlocked.Read(ref _writesAllowedAt)) > allowed;
    }

    private void OnRadioRebooted(string why)
    {
        _options.Trace?.Invoke($"radio rebooted ({why})");
        _sequencer.Reset();
        Clock.Reset();
        lock (_gate)
        {
            _channelBusy = null;
            _lastHeartbeat = 0;
        }

        // A rebooted radio is obfuscated again, whatever this session had chosen.
        _decoder.Obfuscated = true;
        Raise(Rebooted, EventArgs.Empty, nameof(Rebooted));

        if (_options.ResubscribeAfterReboot && Interlocked.Exchange(ref _resubscribing, 1) == 0)
        {
            _ = Task.Run(async () =>
            {
                try
                {
                    // Give the firmware time to finish booting.
                    await Task.Delay(TimeSpan.FromMilliseconds(500), _time, _stopping.Token).ConfigureAwait(false);
                    await IdentifyAsync(_stopping.Token).ConfigureAwait(false);
                    if (_lastSubscription is { } sub && Firmware.Kind == FirmwareKind.PacketV2)
                    {
                        await SubscribeAsync(sub, _stopping.Token).ConfigureAwait(false);
                    }
                }
                catch (Exception ex)
                {
                    _options.Trace?.Invoke($"resubscribe after reboot failed: {ex.Message}");
                }
                finally
                {
                    Interlocked.Exchange(ref _resubscribing, 0);
                }
            });
        }
    }

    private void ScheduleRecoveryIfNeeded()
    {
        if (!_options.RecoverMissedEvents || Firmware.Kind != FirmwareKind.PacketV2 || _sequencer.MissingFrom is null
            || Interlocked.Exchange(ref _recovering, 1) != 0)
        {
            return;
        }

        _ = Task.Run(async () =>
        {
            try
            {
                // Not while the host is transmitting: a frame would end the transmission.
                while (_openWindow is not null)
                {
                    await Task.Delay(TimeSpan.FromMilliseconds(20), _time, _stopping.Token).ConfigureAwait(false);
                }

                if (_sequencer.MissingFrom is { } from)
                {
                    await ReplayEventsAsync(from, _stopping.Token).ConfigureAwait(false);
                }
            }
            catch (Exception ex)
            {
                _options.Trace?.Invoke($"event recovery failed: {ex.Message}");
            }
            finally
            {
                Interlocked.Exchange(ref _recovering, 0);
            }
        });
    }

    /// <summary>Calls each subscriber in turn; one that throws must not starve the others or take the reader down.</summary>
    private void Raise<T>(EventHandler<T>? handlers, T args, string what)
    {
        if (handlers is null)
        {
            return;
        }

        foreach (var d in handlers.GetInvocationList())
        {
            try
            {
                ((EventHandler<T>)d)(this, args);
            }
            catch (Exception e)
            {
                _options.Trace?.Invoke($"{what} handler threw: {e.GetType().Name}: {e.Message}");
            }
        }
    }

    private void Raise(EventHandler? handlers, EventArgs args, string what)
    {
        if (handlers is null)
        {
            return;
        }

        foreach (var d in handlers.GetInvocationList())
        {
            try
            {
                ((EventHandler)d)(this, args);
            }
            catch (Exception e)
            {
                _options.Trace?.Invoke($"{what} handler threw: {e.GetType().Name}: {e.Message}");
            }
        }
    }
}
