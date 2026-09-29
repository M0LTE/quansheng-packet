namespace M0LTE.Uvk5;

/// <summary>Options for <see cref="K5Radio"/>.</summary>
public sealed record K5RadioOptions
{
    /// <summary>Clock for timeouts, the PTT lock and event times. Tests pass a fake one.</summary>
    public TimeProvider TimeProvider { get; init; } = TimeProvider.System;

    /// <summary>How deviation registers are shown in kHz (default: the bench AIOC with its TX EQ).</summary>
    public DeviationLaw DeviationLaw { get; init; } = DeviationLaw.AiocWithBenchEq;

    /// <summary>
    /// Use the obfuscated framing (the radio's power-on mode, and what every other tool uses).
    /// False switches the session to plain frames with the hello.
    /// </summary>
    public bool Obfuscate { get; init; } = true;

    /// <summary>Reply timeout for v2 commands (spec: 100 ms; retried once where safe).</summary>
    public TimeSpan V2ReplyTimeout { get; init; } = TimeSpan.FromMilliseconds(100);

    /// <summary>
    /// Reply timeout for legacy commands. v1 answers in 10 to 22 ms (one command per 10 ms slice);
    /// a 128-byte EEPROM read adds 35 ms on the wire.
    /// </summary>
    public TimeSpan LegacyReplyTimeout { get; init; } = TimeSpan.FromMilliseconds(250);

    /// <summary>
    /// How long after the last frame has left the wire a press is safe on packet firmware v1. The
    /// radio's lock is 1.0 to 1.5 s and a press inside it keys late, eating the start of the frame,
    /// so this must be at least 1.5 s.
    /// </summary>
    public TimeSpan V1KeyAllowance { get; init; } = TimeSpan.FromSeconds(2);

    /// <summary>The same for stock and unidentified firmware (upstream: 6.0 to 6.5 s after a hello or EEPROM command).</summary>
    public TimeSpan StockKeyAllowance { get; init; } = TimeSpan.FromSeconds(6.5);

    /// <summary>
    /// When a gap in event sequence numbers shows events were missed (a frame cut by PTT, say),
    /// fetch them with EVENT_REPLAY once the host is not transmitting. Replayed events are delivered
    /// once, marked <see cref="RadioEventFlags.Replay"/>. Each replay costs one command (and so one
    /// serial lock, 20 ms by default).
    /// </summary>
    public bool RecoverMissedEvents { get; init; } = true;

    /// <summary>
    /// When the radio is seen to reboot (its power-on banner, or its clock and sequence numbers
    /// restarting), say hello again and restore the last subscription, which the radio keeps in RAM
    /// only.
    /// </summary>
    public bool ResubscribeAfterReboot { get; init; } = true;

    /// <summary>Called for every change sent to the radio (EEPROM, registers, overrides, settings, flashing).</summary>
    public Action<K5AuditEntry>? Audit { get; init; }

    /// <summary>Called with a one-line trace of every frame sent and received (hex), for debugging.</summary>
    public Action<string>? Trace { get; init; }

    /// <summary>Events buffered per <see cref="K5Radio.ReadEventsAsync"/> reader before the oldest are dropped.</summary>
    public int EventBufferCapacity { get; init; } = 4096;

    /// <summary>Session id sent in the hello and every EEPROM command; random if null.</summary>
    public uint? SessionId { get; init; }
}

/// <summary>An event subscription (SUBSCRIBE). The radio keeps it in RAM; a hello resets it.</summary>
public sealed record EventSubscription
{
    /// <summary>Which events.</summary>
    public RadioEvents Events { get; init; }

    /// <summary>
    /// Heartbeat period (100 ms to 60 s), or null for none. Needs <see cref="RadioEvents.Heartbeat"/>
    /// in <see cref="Events"/>; the library adds it when a period is given.
    /// </summary>
    public TimeSpan? HeartbeatPeriod { get; init; }

    /// <summary>RSSI stream sample period (5 to 250 ms), or null for none. Adds <see cref="RadioEvents.RssiStream"/>.</summary>
    public TimeSpan? RssiStreamPeriod { get; init; }

    /// <summary>RSSI stream samples per frame, 1 to 20.</summary>
    public int RssiStreamBatch { get; init; } = 10;

    /// <summary>Burst sample period (2 to 50 ms), or null for the firmware default (5 ms).</summary>
    public TimeSpan? BurstSamplePeriod { get; init; }

    /// <summary>
    /// Ask the radio to send events during transmissions too (LIVE_TX). Only set this after the
    /// AIOC's RXIGNPTT has been turned off (see <see cref="Aioc.AiocHid.AllowRadioOutputWhileKeyed"/>);
    /// otherwise the AIOC drops everything the radio sends while PTT is held.
    /// </summary>
    public bool LiveTx { get; init; }

    /// <summary>
    /// Store the mask, options and heartbeat period as the radio's power-on default. The radio then
    /// sends frames unprompted from power-on, which may confuse CHIRP and similar tools: only for
    /// dedicated stations.
    /// </summary>
    public bool Persist { get; init; }

    /// <summary>
    /// The subscription a TNC wants: busy edges, burst reports, TX start/end/refused, battery,
    /// parameter changes, lost events, tone end, override expiry, and a 1 s heartbeat for clock
    /// tracking and link supervision.
    /// </summary>
    public static EventSubscription ForTnc { get; } = new()
    {
        Events = RadioEvents.Tnc | RadioEvents.Heartbeat,
        HeartbeatPeriod = TimeSpan.FromSeconds(1),
    };
}

/// <summary>What the radio said to a SUBSCRIBE.</summary>
/// <param name="NextSequence">Seq the next stored event will get.</param>
/// <param name="OldestSequence">Oldest seq still in the ring (equal to <paramref name="NextSequence"/> if empty).</param>
/// <param name="RadioTimeMs">Radio clock when the reply was queued.</param>
public readonly record struct SubscriptionInfo(ushort NextSequence, ushort OldestSequence, uint RadioTimeMs);

/// <summary>Result of a clock synchronisation.</summary>
/// <param name="RadioBootTime">Host time at which the radio clock read zero (so an event at radio time t happened at about <c>RadioBootTime + t ms</c>; <see cref="RadioClock.ToHostTime"/> also corrects for drift).</param>
/// <param name="Uncertainty">Half-width of the best exchange.</param>
/// <param name="Exchanges">Exchanges done.</param>
public readonly record struct TimeSyncResult(DateTimeOffset RadioBootTime, TimeSpan Uncertainty, int Exchanges);
