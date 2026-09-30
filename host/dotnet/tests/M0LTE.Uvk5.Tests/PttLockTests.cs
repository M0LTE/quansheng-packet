using M0LTE.Uvk5.Client;
using M0LTE.Uvk5.Simulation;
using Microsoft.Extensions.Time.Testing;

namespace M0LTE.Uvk5.Tests;

/// <summary>When the host may key, and the transmit window that enforces it.</summary>
public class PttLockTests
{
    private static CancellationToken Ct => TestContext.Current.CancellationToken;

    private static PttLockTracker Tracker(FakeTimeProvider t) => new(t, TimeSpan.FromSeconds(2), TimeSpan.FromSeconds(6.5));

    private static double Ms(TimeSpan t) => t.TotalMilliseconds;

    [Fact]
    public void V2_reply_lock_is_authoritative()
    {
        var t = new FakeTimeProvider();
        var k = Tracker(t);
        k.SetFirmware(FirmwareKind.PacketV2, TimeSpan.FromMilliseconds(20));
        k.OnFrameWritten(13);
        Assert.Equal(13 * 0.27 + 12 + 20, Ms(k.Remaining), 3);   // the no-reply formula until the reply comes
        t.Advance(TimeSpan.FromMilliseconds(4));
        k.OnV2Reply(18);
        Assert.Equal(18, Ms(k.Remaining), 3);
        t.Advance(TimeSpan.FromMilliseconds(18));
        Assert.Equal(TimeSpan.Zero, k.Remaining);
    }

    [Fact]
    public void V2_legacy_reply_adds_two_ms()
    {
        var t = new FakeTimeProvider();
        var k = Tracker(t);
        k.SetFirmware(FirmwareKind.PacketV2, TimeSpan.FromMilliseconds(20));
        k.OnFrameWritten(12);
        k.OnLegacyReply();
        Assert.Equal(22, Ms(k.Remaining), 3);
    }

    [Fact]
    public void V1_waits_the_allowance_after_the_frame_left_the_wire()
    {
        var t = new FakeTimeProvider();
        var k = Tracker(t);
        k.SetFirmware(FirmwareKind.PacketV1, null);
        k.OnFrameWritten(16);
        k.OnLegacyReply();                  // no effect on v1
        Assert.Equal(2000 + 16 * 0.27, Ms(k.Remaining), 3);
    }

    [Fact]
    public void Unknown_firmware_is_treated_as_stock_until_identified()
    {
        var t = new FakeTimeProvider();
        var k = Tracker(t);
        k.OnFrameWritten(16);
        Assert.Equal(6500 + 16 * 0.27, Ms(k.Remaining), 3);
        k.SetFirmware(FirmwareKind.PacketV1, null);
        Assert.Equal(2000 + 16 * 0.27, Ms(k.Remaining), 3);
    }

    [Fact]
    public async Task After_a_v2_command_the_client_knows_the_lock()
    {
        var time = new FakeTimeProvider();
        await using var rig = await Rig.StartAsync(time: time);
        Assert.Equal(20, Ms(rig.Radio.TimeUntilSafeToKey), 3);
        Assert.False(rig.Radio.IsSafeToKey);
        Assert.Equal(20, Ms(rig.Sim.LockRemaining), 3);
        time.Advance(TimeSpan.FromMilliseconds(20));
        Assert.True(rig.Radio.IsSafeToKey);
        rig.Sim.PressPtt();
        var tx = Assert.Single(rig.Sim.Transmissions);
        Assert.False(tx.LateKey);
    }

    [Fact]
    public async Task A_press_inside_the_lock_keys_late_and_says_so()
    {
        var time = new FakeTimeProvider();
        await using var rig = await Rig.StartAsync(time: time, simOptions: new SimulatedRadioOptions { DeferralResumeDelay = TimeSpan.Zero });
        await rig.Radio.SubscribeAsync(new EventSubscription { Events = RadioEvents.TxStart | RadioEvents.TxEnd }, Ct);
        var events = new List<RadioEvent>();
        rig.Radio.EventReceived += (_, e) => events.Add(e);
        time.Advance(TimeSpan.FromMilliseconds(8));
        rig.Sim.PressPtt();                       // 12 ms of lock left: a late key
        Assert.Empty(rig.Sim.Transmissions);
        time.Advance(TimeSpan.FromMilliseconds(12));
        Assert.True(Assert.Single(rig.Sim.Transmissions).LateKey);
        rig.Sim.ReleasePtt();
        await Rig.Until(() => events.OfType<TxEndEvent>().Any());
        var start = events.OfType<TxStartEvent>().Single();
        Assert.True(start.LateKey);
        Assert.Equal(12, start.LockDelay.TotalMilliseconds);
        Assert.True(start.IsDeferred);
    }

    [Fact]
    public async Task A_press_with_a_long_lock_is_refused()
    {
        var time = new FakeTimeProvider();
        await using var rig = await Rig.StartAsync(time: time, simOptions: new SimulatedRadioOptions { DeferralResumeDelay = TimeSpan.Zero });
        await rig.Radio.SetSettingsAsync(new RadioSettings { SerialLock = TimeSpan.FromMilliseconds(500) }, cancellationToken: Ct);
        await rig.Radio.SubscribeAsync(new EventSubscription { Events = RadioEvents.TxRefused }, Ct);
        Assert.Equal(500, Ms(rig.Radio.TimeUntilSafeToKey), 3);
        var refused = new TaskCompletionSource<TxRefusedEvent>();
        rig.Radio.EventReceived += (_, e) => { if (e is TxRefusedEvent r) { refused.TrySetResult(r); } };
        rig.Sim.PressPtt();
        time.Advance(TimeSpan.FromSeconds(1));
        Assert.Empty(rig.Sim.Transmissions);      // latched: no late key after the lock either
        rig.Sim.ReleasePtt();
        var ev = await refused.Task.WaitAsync(TimeSpan.FromSeconds(3), Ct);
        Assert.Equal(TxRefusedReason.Lock, ev.Reason);
        Assert.Equal(500, ev.LockRemaining.TotalMilliseconds);
    }

    [Fact]
    public async Task On_v1_a_press_inside_the_lock_keys_late_silently()
    {
        var time = new FakeTimeProvider();
        await using var rig = await Rig.StartAsync(FirmwareKind.PacketV1, time);
        Assert.True(rig.Radio.TimeUntilSafeToKey >= TimeSpan.FromSeconds(2));
        rig.Sim.PressPtt();
        time.Advance(TimeSpan.FromMilliseconds(1600));
        var tx = Assert.Single(rig.Sim.Transmissions);
        Assert.True(tx.LateKey);                    // what the bench saw: the start of the frame eaten
        rig.Sim.ReleasePtt();
        time.Advance(TimeSpan.FromMilliseconds(500));
        Assert.True(rig.Radio.IsSafeToKey);
    }

    [Fact]
    public async Task Stock_allows_six_and_a_half_seconds()
    {
        var time = new FakeTimeProvider();
        await using var rig = await Rig.StartAsync(FirmwareKind.Stock, time);
        Assert.True(rig.Radio.TimeUntilSafeToKey >= TimeSpan.FromSeconds(6.5));
    }

    [Fact]
    public async Task Transmit_window_waits_for_the_lock_and_holds_the_port_quiet()
    {
        var time = new FakeTimeProvider();
        await using var rig = await Rig.StartAsync(time: time, simOptions: new SimulatedRadioOptions { DeferralResumeDelay = TimeSpan.Zero });
        await rig.Radio.SubscribeAsync(new EventSubscription { Events = RadioEvents.TxStart | RadioEvents.TxEnd }, Ct);
        var events = new List<RadioEvent>();
        rig.Radio.EventReceived += (_, e) => { lock (events) { events.Add(e); } };

        // Another part of the application, started before the window, polls the status.
        var go = new TaskCompletionSource();
        var poll = Task.Run(async () =>
        {
            await go.Task;
            return await rig.Radio.GetStatusAsync(Ct);
        }, Ct);

        var opening = rig.Radio.BeginTransmitAsync(Ct);
        Assert.False(opening.IsCompleted);          // 20 ms of lock left
        await Pump(time, () => opening.IsCompleted);
        await using (var window = await opening)
        {
            Assert.True(rig.Radio.IsSafeToKey);
            Assert.True(rig.Radio.IsTransmitWindowOpen);
            Assert.Equal(20, window.WaitedForLock.TotalMilliseconds, 0);
            go.SetResult();
            await Task.Delay(50, Ct);
            Assert.False(poll.IsCompleted);          // waits for the window

            rig.Sim.PressPtt();
            Assert.True(rig.Sim.IsTransmitting);
            await Assert.ThrowsAsync<K5TransmitInProgressException>(() => rig.Radio.GetStatusAsync(Ct));
            await Assert.ThrowsAsync<K5TransmitInProgressException>(() => rig.Radio.BeginTransmitAsync(Ct));
            time.Advance(TimeSpan.FromMilliseconds(300));
            rig.Sim.ReleasePtt();
        }

        Assert.False(rig.Radio.IsTransmitWindowOpen);
        await Pump(time, () => poll.IsCompleted);
        await poll;
        Assert.Empty(rig.Sim.Violations);
        var tx = Assert.Single(rig.Sim.Transmissions);
        Assert.Equal(TxEndReason.Released, tx.EndReason);
        await Rig.Until(() => { lock (events) { return events.OfType<TxEndEvent>().Any(); } });
        TxEndEvent end;
        TxStartEvent start;
        lock (events)
        {
            end = events.OfType<TxEndEvent>().Single();
            start = events.OfType<TxStartEvent>().Single();
        }

        Assert.Equal(TxEndReason.Released, end.Reason);
        Assert.True(start.IsDeferred);        // held while PTT was down
        Assert.True(end.IsDeferred);          // as the firmware's vector: both go after the release
    }

    [Fact]
    public async Task Writing_while_keyed_ends_the_transmission()
    {
        // What the window prevents: the AIOC releases PTT on any serial write.
        await using var rig = await Rig.StartAsync();
        await Task.Delay(30, Ct);
        rig.Sim.PressPtt();
        Assert.True(rig.Sim.IsTransmitting);
        await rig.Radio.GetStatusAsync(Ct);
        Assert.False(rig.Sim.IsTransmitting);
        Assert.Equal(TxEndReason.SerialFrame, rig.Sim.Transmissions[0].EndReason);
        Assert.Single(rig.Sim.Violations);
    }

    [Fact]
    public async Task Wait_until_safe_to_key()
    {
        await using var rig = await Rig.StartAsync(FirmwareKind.PacketV2);
        await rig.Radio.WaitUntilSafeToKeyAsync(Ct);
        Assert.True(rig.Radio.IsSafeToKey);
        Assert.True(rig.Radio.ReadyToKeyAt <= DateTimeOffset.UtcNow.AddMilliseconds(1));
    }

    internal static async Task Pump(FakeTimeProvider time, Func<bool> done, int maxSteps = 20000)
    {
        for (int i = 0; i < maxSteps && !done(); i++)
        {
            time.Advance(TimeSpan.FromMilliseconds(1));
            await Task.Delay(1);
        }

        Assert.True(done());
    }
}
