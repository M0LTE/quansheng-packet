namespace M0LTE.Uvk5;

/// <summary>
/// A period during which the host may hold PTT. While it is open the library sends nothing to the
/// radio: on the AIOC any byte written to the serial port releases PTT at once (and it stays
/// released until the next HID report), so a frame sent while keyed ends the transmission.
/// </summary>
/// <remarks>
/// <para>Get one from <see cref="K5Radio.BeginTransmitAsync"/>, which returns once the radio's
/// serial PTT lock has run out and no frame is on the wire. Key PTT (AIOC HID) only after that,
/// unkey before disposing, and dispose as soon as PTT is released. Commands called from other
/// tasks meanwhile wait until the window closes (plus the radio's release debounce, so the release
/// is seen as a release); a command called from inside the window's own async flow throws
/// <see cref="K5TransmitInProgressException"/> instead of deadlocking or cutting the
/// transmission.</para>
/// </remarks>
public sealed class TransmitWindow : IAsyncDisposable, IDisposable
{
    private readonly Action<TransmitWindow> _close;
    private int _closed;

    internal TransmitWindow(Action<TransmitWindow> close, DateTimeOffset openedAt, TimeSpan waited, bool? channelBusy)
    {
        _close = close;
        OpenedAt = openedAt;
        WaitedForLock = waited;
        ChannelBusyAtOpen = channelBusy;
    }

    /// <summary>When the window opened (the earliest safe press).</summary>
    public DateTimeOffset OpenedAt { get; }

    /// <summary>How long opening waited for the serial lock and any command in flight.</summary>
    public TimeSpan WaitedForLock { get; }

    /// <summary>The radio's busy state when the window opened (null if unknown). The library does not do CSMA; this is for the caller's.</summary>
    public bool? ChannelBusyAtOpen { get; }

    /// <summary>True until disposed.</summary>
    public bool IsOpen => Volatile.Read(ref _closed) == 0;

    /// <summary>Closes the window: call after PTT has been released.</summary>
    public void Dispose()
    {
        if (Interlocked.Exchange(ref _closed, 1) == 0)
        {
            _close(this);
        }
    }

    /// <inheritdoc cref="Dispose"/>
    public ValueTask DisposeAsync()
    {
        Dispose();
        return ValueTask.CompletedTask;
    }
}
