using M0LTE.Uvk5.Client;
using M0LTE.Uvk5.Protocol;
using M0LTE.Uvk5.Simulation;
using Microsoft.Extensions.Time.Testing;

namespace M0LTE.Uvk5.Tests;

/// <summary>Event dispatch: busy edges, burst reports, gaps, replay, loss, heartbeats and the clock.</summary>
public class EventTests
{
    private static CancellationToken Ct => TestContext.Current.CancellationToken;

    private sealed class Collector
    {
        private readonly List<RadioEvent> _events = [];

        public Collector(K5Radio radio) => radio.EventReceived += (_, e) => { lock (_events) { _events.Add(e); } };

        public List<RadioEvent> Snapshot()
        {
            lock (_events)
            {
                return [.. _events];
            }
        }

        public Task WaitFor(Func<List<RadioEvent>, bool> cond) => Rig.Until(() => cond(Snapshot()));
    }

    [Fact]
    public async Task Busy_edges_drive_channel_busy_and_burst_reports()
    {
        await using var rig = await Rig.StartAsync();
        Assert.Null(rig.Radio.ChannelBusy);                   // not subscribed yet: unknown
        var edges = new List<BusyEvent>();
        rig.Radio.BusyChanged += (_, e) => { lock (edges) { edges.Add(e); } };
        var c = new Collector(rig.Radio);
        await rig.Radio.SubscribeAsync(EventSubscription.ForTnc, Ct);
        Assert.False(rig.Radio.ChannelBusy);                  // read from GET_STATUS at once

        rig.Sim.StartCarrier(Rssi.FromDbm(-80), 8, 1);
        await Rig.Until(() => rig.Radio.ChannelBusy == true);
        await Task.Delay(40, Ct);
        rig.Sim.StopCarrier();
        await c.WaitFor(l => l.OfType<RxBurstEvent>().Any());
        Assert.False(rig.Radio.ChannelBusy);

        lock (edges)
        {
            Assert.Equal([true, false], edges.Select(e => e.Busy));
            Assert.Equal(-80, edges[0].Rssi.Dbm, 1);
            Assert.True(edges[0].Cause.HasFlag(BusyCause.SquelchEdge));
        }

        var burst = c.Snapshot().OfType<RxBurstEvent>().Single();
        Assert.Equal(-80, burst.RssiMean.Dbm, 1);
        Assert.True(burst.Duration >= TimeSpan.FromMilliseconds(30));
        Assert.Null(burst.FrequencyErrorHz);
        Assert.Null(burst.AfAmplitudeMean);
        Assert.Equal(edges[0].RadioTimeMs, burst.OpenedAtMs);
        Assert.NotNull(burst.EstimatedTime);                  // exactly-timed frames feed the clock
    }

    [Fact]
    public async Task A_throwing_handler_does_not_stop_the_reader()
    {
        await using var rig = await Rig.StartAsync();
        rig.Radio.EventReceived += (_, _) => throw new InvalidOperationException("subscriber bug");
        rig.Radio.BusyChanged += (_, _) => throw new InvalidOperationException("subscriber bug");
        var c = new Collector(rig.Radio);
        await rig.Radio.SubscribeAsync(new EventSubscription { Events = RadioEvents.Busy }, Ct);
        rig.Sim.StartCarrier(new Rssi(200));
        rig.Sim.StopCarrier();
        await c.WaitFor(l => l.Count == 2);
        Assert.Equal(144_800_000, (await rig.Radio.GetStatusAsync(Ct)).FrequencyHz);
    }

    [Fact]
    public async Task Read_events_async_delivers_in_order()
    {
        await using var rig = await Rig.StartAsync();
        await rig.Radio.SubscribeAsync(new EventSubscription { Events = RadioEvents.Busy }, Ct);
        using var cts = CancellationTokenSource.CreateLinkedTokenSource(Ct);
        var got = new List<RadioEvent>();
        var reader = Task.Run(async () =>
        {
            await foreach (var e in rig.Radio.ReadEventsAsync(cts.Token))
            {
                got.Add(e);
                if (got.Count == 4)
                {
                    break;
                }
            }
        }, Ct);
        await Task.Delay(50, Ct);
        for (int i = 0; i < 2; i++)
        {
            rig.Sim.StartCarrier(new Rssi(200));
            rig.Sim.StopCarrier();
        }

        await reader.WaitAsync(TimeSpan.FromSeconds(3), Ct);
        Assert.Equal([true, false, true, false], got.Cast<BusyEvent>().Select(e => e.Busy));
        Assert.Equal(got.Select(e => (int)e.Sequence), Enumerable.Range(got[0].Sequence, 4));
    }

    [Fact]
    public async Task A_cut_frame_is_recovered_by_replay_without_duplicates()
    {
        await using var rig = await Rig.StartAsync();
        var c = new Collector(rig.Radio);
        await rig.Radio.SubscribeAsync(new EventSubscription { Events = RadioEvents.Busy | RadioEvents.RxBurst }, Ct);
        rig.Sim.CutNextFrame(12);                           // the CD open is truncated on the wire
        rig.Sim.StartCarrier(new Rssi(200));
        rig.Sim.StopCarrier();                              // CD close and RX_BURST arrive: a gap
        await c.WaitFor(l => l.Count >= 3);
        await Task.Delay(100, Ct);
        var events = c.Snapshot();
        Assert.Equal(3, events.Count);
        var replayed = Assert.Single(events, e => e.IsReplay);
        Assert.True(((BusyEvent)replayed).Busy);
        Assert.Equal(events.Select(e => e.Sequence).Order(), events.Select(e => e.Sequence).Distinct().Order());
        Assert.Contains((ushort)0x500B, rig.Sim.ReceivedIds);
    }

    [Fact]
    public async Task Explicit_replay_does_not_deliver_twice()
    {
        await using var rig = await Rig.StartAsync();
        var c = new Collector(rig.Radio);
        var info = await rig.Radio.SubscribeAsync(new EventSubscription { Events = RadioEvents.Busy }, Ct);
        rig.Sim.StartCarrier(new Rssi(200));
        rig.Sim.StopCarrier();
        await c.WaitFor(l => l.Count == 2);
        var r = await rig.Radio.ReplayEventsAsync(info.NextSequence, Ct);
        Assert.Equal(2, r.CountSent);
        await Task.Delay(50, Ct);
        Assert.Equal(2, c.Snapshot().Count);
    }

    [Fact]
    public async Task Events_lost_while_deferred_are_reported()
    {
        await using var rig = await Rig.StartAsync(simOptions: new SimulatedRadioOptions { EventRingCapacity = 4, TxAllowed = _ => false, DeferralResumeDelay = TimeSpan.Zero });
        var c = new Collector(rig.Radio);
        await rig.Radio.SubscribeAsync(new EventSubscription { Events = RadioEvents.Tnc }, Ct);
        await Task.Delay(30, Ct);
        rig.Sim.PressPtt();                                  // refused (TX band): TX_REFUSED stored, deferred
        for (int i = 0; i < 6; i++)
        {
            rig.Sim.ChangeFromKeypad(new RadioSettings { Backlight = i });
        }

        rig.Sim.ReleasePtt();
        await c.WaitFor(l => l.OfType<EventsLostEvent>().Any());
        await Task.Delay(100, Ct);
        var events = c.Snapshot();
        var lost = events.OfType<EventsLostEvent>().Single();
        Assert.Equal(3, lost.Count);
        Assert.Equal(0, lost.FirstLost);
        Assert.All(events.OfType<ParamsChangedEvent>(), e => Assert.True(e.IsDeferred));
        Assert.Equal(4, events.OfType<ParamsChangedEvent>().Count());
        Assert.Empty(events.OfType<TxRefusedEvent>());      // it was the first overwritten
    }

    [Fact]
    public async Task Live_tx_events_arrive_while_keyed()
    {
        await using var rig = await Rig.StartAsync(simOptions: new SimulatedRadioOptions { AiocDropsRadioOutputWhileKeyed = false });
        var c = new Collector(rig.Radio);
        await rig.Radio.SubscribeAsync(new EventSubscription { Events = RadioEvents.TxStart | RadioEvents.TxEnd, LiveTx = true }, Ct);
        await Task.Delay(30, Ct);
        rig.Sim.PressPtt();
        await c.WaitFor(l => l.OfType<TxStartEvent>().Any());
        Assert.True(rig.Sim.IsTransmitting);
        Assert.False(c.Snapshot().OfType<TxStartEvent>().Single().IsDeferred);
        rig.Sim.ReleasePtt();
        await c.WaitFor(l => l.OfType<TxEndEvent>().Any());
    }

    [Fact]
    public async Task Params_changed_from_the_keypad_are_seen()
    {
        await using var rig = await Rig.StartAsync();
        var c = new Collector(rig.Radio);
        await rig.Radio.SubscribeAsync(new EventSubscription { Events = RadioEvents.ParamsChanged }, Ct);
        rig.Sim.ChangeFromKeypad(new RadioSettings { FrequencyHz = 145_500_000, Power = TxPower.Mid });
        await c.WaitFor(l => l.Count == 1);
        var e = (ParamsChangedEvent)c.Snapshot()[0];
        Assert.Equal(ParamsChangeSource.Keypad, e.Source);
        Assert.Equal([RadioParameterId.FrequencyHz, RadioParameterId.Power], e.Parameters);
    }

    [Fact]
    public async Task Battery_and_override_expiry_events()
    {
        var time = new FakeTimeProvider();
        await using var rig = await Rig.StartAsync(time: time);
        var c = new Collector(rig.Radio);
        await rig.Radio.SubscribeAsync(new EventSubscription { Events = RadioEvents.Battery | RadioEvents.OverrideExpired }, Ct);
        rig.Sim.SetBattery(6100);
        await rig.Radio.AddOverridesAsync([new RegisterOverride(OverridePhase.Tx, 0x2B, 0xFFF8, 0)], TimeSpan.FromSeconds(5), null, Ct);
        time.Advance(TimeSpan.FromSeconds(5));
        await c.WaitFor(l => l.Count == 2);
        var b = c.Snapshot().OfType<BatteryEvent>().Single();
        Assert.Equal(BatteryClass.Empty, b.Class);
        Assert.Equal(0, b.Level);
        var o = c.Snapshot().OfType<OverrideExpiredEvent>().Single();
        Assert.False(o.ByKeyUps);
        Assert.Equal(1, o.EntriesReverted);
        Assert.Empty(rig.Sim.RamOverrides);
    }

    [Fact]
    public async Task Overrides_expire_after_key_ups()
    {
        var time = new FakeTimeProvider();
        await using var rig = await Rig.StartAsync(time: time);
        await rig.Radio.AddOverridesAsync([new RegisterOverride(OverridePhase.Tx, 0x2B, 0xFFF8, 0)], null, 2, Ct);
        time.Advance(TimeSpan.FromMilliseconds(30));
        for (int i = 0; i < 2; i++)
        {
            rig.Sim.PressPtt();
            time.Advance(TimeSpan.FromMilliseconds(100));
            rig.Sim.ReleasePtt();
            time.Advance(TimeSpan.FromMilliseconds(10));
        }

        Assert.Empty(rig.Sim.RamOverrides);
    }

    [Fact]
    public async Task Heartbeats_estimate_the_clock_and_skew()
    {
        var time = new FakeTimeProvider();
        await using var rig = await Rig.StartAsync(time: time);
        var c = new Collector(rig.Radio);
        await rig.Radio.SubscribeAsync(new EventSubscription { Events = RadioEvents.Heartbeat, HeartbeatPeriod = TimeSpan.FromMilliseconds(500) }, Ct);
        for (int i = 0; i < 30; i++)
        {
            time.Advance(TimeSpan.FromMilliseconds(500));
            await c.WaitFor(l => l.Count == i + 1);
        }

        Assert.True(rig.Radio.Clock.IsEstimated);
        Assert.NotNull(rig.Radio.Clock.SkewPpm);
        Assert.InRange(rig.Radio.Clock.SkewPpm!.Value, -1000, 1000);
        var now = time.GetUtcNow();
        var mapped = rig.Radio.Clock.ToHostTime(rig.Sim.RadioTimeMs)!.Value;

        // The simulator delivers frames instantly, but the estimate (rightly) allows 256 us per
        // byte for the heartbeat to cross the wire: 31 bytes, 7.9 ms early here.
        Assert.InRange((mapped - now).TotalMilliseconds, -8.5, 1);
        var hb = c.Snapshot().OfType<HeartbeatEvent>().Last();
        Assert.True(hb.IsEphemeral);
        Assert.Equal(RadioState.Idle, hb.State);
    }

    [Fact]
    public async Task Time_sync_maps_radio_time_to_host_time()
    {
        var time = new FakeTimeProvider();
        await using var rig = await Rig.StartAsync(time: time);
        time.Advance(TimeSpan.FromSeconds(3));
        var r = await rig.Radio.SyncClockAsync(4, Ct);
        Assert.Equal(4, r.Exchanges);
        var mapped = rig.Radio.Clock.ToHostTime(rig.Sim.RadioTimeMs)!.Value;
        Assert.InRange((mapped - time.GetUtcNow()).TotalMilliseconds, -15, 15);
    }

    [Fact]
    public async Task Heartbeat_silence_makes_busy_unknown()
    {
        var time = new FakeTimeProvider();
        await using var rig = await Rig.StartAsync(time: time);
        await rig.Radio.SubscribeAsync(EventSubscription.ForTnc, Ct);
        Assert.False(rig.Radio.ChannelBusy);
        rig.Sim.Dispose();                                   // the radio goes silent (cable pulled)
        time.Advance(TimeSpan.FromSeconds(6));
        Assert.Null(rig.Radio.ChannelBusy);
    }

    [Fact]
    public void Sequencer_finds_gaps_duplicates_and_reboots()
    {
        var s = new EventSequencer();
        RadioEvent E(ushort seq, uint t, RadioEventFlags f = RadioEventFlags.None) => new BusyEvent { Sequence = seq, RadioTimeMs = t, Flags = f };
        Assert.Equal(EventSequencer.Verdict.New, s.Accept(E(5, 1000)));
        Assert.Equal(EventSequencer.Verdict.New, s.Accept(E(6, 1010)));
        Assert.Equal(EventSequencer.Verdict.Duplicate, s.Accept(E(6, 1010)));
        Assert.Equal(EventSequencer.Verdict.Gap, s.Accept(E(9, 1100)));
        Assert.Equal((ushort)7, s.MissingFrom);
        Assert.Equal(EventSequencer.Verdict.New, s.Accept(E(7, 1050, RadioEventFlags.Replay)));
        Assert.Equal((ushort)8, s.MissingFrom);
        Assert.Equal(EventSequencer.Verdict.Duplicate, s.Accept(E(9, 1100, RadioEventFlags.Replay)));
        Assert.Equal(EventSequencer.Verdict.New, s.Accept(E(8, 1060, RadioEventFlags.Replay)));
        Assert.Null(s.MissingFrom);

        // an ephemeral event carries the next stored seq: a jump means stored events were missed
        Assert.Equal(EventSequencer.Verdict.New, s.Accept(E(10, 1200, RadioEventFlags.Ephemeral)));
        Assert.Equal(EventSequencer.Verdict.Gap, s.Accept(E(12, 1300, RadioEventFlags.Ephemeral)));
        s.ReplayDone();
        Assert.Null(s.MissingFrom);

        // clock and seq restart
        Assert.Equal(EventSequencer.Verdict.Rebooted, s.Accept(E(0, 20)));
        Assert.Equal(EventSequencer.Verdict.New, s.Accept(E(1, 25)));
    }

    [Fact]
    public void Wraparound_is_not_a_gap()
    {
        var s = new EventSequencer();
        RadioEvent E(ushort seq) => new BusyEvent { Sequence = seq, RadioTimeMs = 5000 };
        Assert.Equal(EventSequencer.Verdict.New, s.Accept(E(65535)));
        Assert.Equal(EventSequencer.Verdict.New, s.Accept(E(0)));
        Assert.Equal(EventSequencer.Verdict.New, s.Accept(E(1)));
    }

    [Fact]
    public void Every_event_layout_parses()
    {
        var law = DeviationLaw.AiocWithBenchEq;
        byte[] Header(ushort seq) => new WireWriter().U16(seq).U32(1234).U8(0x10).ToArray();
        var ts = (TxStartEvent)EventParser.Parse(0x50C2, [.. Header(1), .. new WireWriter().U32(1200).U32(145_000_000).U8(2).U8(1).U16(0x756).U16(12).U8(3).ToArray()], law, default);
        Assert.Equal(34, 8 + 4 + 7 + 15);
        Assert.Equal(TimeSpan.FromMilliseconds(34), ts.KeyUpLatency);
        Assert.True(ts.BusyAtPress && ts.LateKey);
        Assert.Equal(Bandwidth.Narrow, ts.Bandwidth);
        var te = (TxEndEvent)EventParser.Parse(0x50C3, [.. Header(2), .. new WireWriter().U32(1000).U32(1225).U32(1240).U8(0).U16(300).U16(900).U16(22).ToArray()], law, default);
        Assert.Equal(TimeSpan.FromMilliseconds(234), te.OnAir);
        Assert.Equal(TimeSpan.FromMilliseconds(9), te.KeyDownLatency);
        Assert.Equal(TimeSpan.FromMilliseconds(15), te.TurnaroundToReceive);
        Assert.Equal(300, te.MicAmplitudeMean);
        var rs = (RssiStreamEvent)EventParser.Parse(0x50C5, [.. Header(3), 10, 3, .. new WireWriter().U16(100).U8(1).U8(2).U16(110).U8(1).U8(2).U16(120).U8(1).U8(2).ToArray()], law, default);
        Assert.Equal([1214u, 1224u, 1234u], rs.Samples.Select(x => x.RadioTimeMs));
        var boot = (BootEvent)EventParser.Parse(0x50CC, [.. Header(0), 0x00, 0x02, 1], law, default);
        Assert.Equal(new Version(2, 0), boot.ProtocolVersion);
        var unknown = (UnknownEvent)EventParser.Parse(0x50D5, [.. Header(4), 9, 9], law, default);
        Assert.Equal(2, unknown.Body.Length);
        Assert.Throws<K5ProtocolException>(() => EventParser.Parse(0x50C0, Header(5), law, default));
    }
}
