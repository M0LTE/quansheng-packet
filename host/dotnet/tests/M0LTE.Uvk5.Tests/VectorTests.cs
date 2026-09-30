using System.Buffers.Binary;
using System.Text.Json;
using M0LTE.Uvk5.Protocol;

namespace M0LTE.Uvk5.Tests;

/// <summary>
/// The firmware's own golden vectors (tests/vectors/protocol-v2.json, produced by the real firmware
/// code compiled on the host): the codec must decode every response exactly, and the client must
/// build the same requests for the same calls and understand every reply and event.
/// </summary>
public class VectorTests
{
    private static CancellationToken Ct => TestContext.Current.CancellationToken;

    internal sealed record Frame(ushort Id, string Crc, byte[] Body);

    internal sealed record Vector(string Name, string State, bool RequestObfuscated, byte[] Request, bool ResponseObfuscated, byte[] Response, Frame[] Frames);

    /// <summary>The repo's tests/vectors copy if this checkout has one (it follows the firmware), else the copy in this project.</summary>
    internal static readonly Lazy<Vector[]> All = new(() =>
    {
        string? path = null;
        for (var dir = new DirectoryInfo(AppContext.BaseDirectory); dir is not null; dir = dir.Parent)
        {
            string candidate = Path.Combine(dir.FullName, "tests", "vectors", "protocol-v2.json");
            if (File.Exists(candidate) && File.Exists(Path.Combine(dir.FullName, "docs", "protocol-v2.md")))
            {
                path = candidate;
                break;
            }
        }

        path ??= Path.Combine(AppContext.BaseDirectory, "Vectors", "protocol-v2.json");
        using var doc = JsonDocument.Parse(File.ReadAllBytes(path));
        return [.. doc.RootElement.GetProperty("vectors").EnumerateArray().Select(v => new Vector(
            v.GetProperty("name").GetString()!,
            v.GetProperty("state").GetString()!,
            v.GetProperty("request_mode").GetString() == "obfuscated",
            Convert.FromHexString(v.GetProperty("request").GetString()!),
            v.GetProperty("response_mode").GetString() == "obfuscated",
            Convert.FromHexString(v.GetProperty("response").GetString()!),
            [.. v.GetProperty("frames").EnumerateArray().Select(f => new Frame(
                Convert.ToUInt16(f.GetProperty("id").GetString(), 16),
                f.GetProperty("crc").GetString()!,
                Convert.FromHexString(f.GetProperty("body").GetString()!)))]))];
    });

    public static TheoryData<string> Names => [.. All.Value.Select(v => v.Name)];

    private static Vector Get(string name) => All.Value.Single(v => v.Name == name);

    [Theory]
    [MemberData(nameof(Names))]
    public void Response_decodes_to_the_listed_frames(string name)
    {
        var v = Get(name);
        var frames = new K5FrameDecoder(v.ResponseObfuscated).Feed(v.Response);
        Assert.Equal(v.Frames.Length, frames.Count);
        for (int i = 0; i < frames.Count; i++)
        {
            Assert.Equal(v.Frames[i].Id, frames[i].Id);
            Assert.Equal(v.Frames[i].Crc == "real" ? CrcStatus.Valid : CrcStatus.Absent, frames[i].Crc);
            Assert.Equal(v.Frames[i].Body, frames[i].Body.ToArray());
            Assert.Equal(frames[i].Body.Length, frames[i].DeclaredBodyLength);
        }
    }

    [Theory]
    [MemberData(nameof(Names))]
    public void Request_reencodes_identically(string name)
    {
        var v = Get(name);
        if (v.Request.Length == 0)
        {
            return;     // unsolicited events
        }

        var f = Assert.Single(new K5FrameDecoder(v.RequestObfuscated, maxPayload: 248).Feed(v.Request));
        Assert.Equal(CrcStatus.Valid, f.Crc);
        Assert.Equal(v.Request, K5FrameCodec.EncodePayload(f.Payload.Span, v.RequestObfuscated));
    }

    [Theory]
    [MemberData(nameof(Names))]
    public void Every_event_parses_with_its_whole_body(string name)
    {
        foreach (var f in Get(name).Frames.Where(f => MessageIds.IsEvent(f.Id)))
        {
            var e = EventParser.Parse(f.Id, f.Body, DeviationLaw.AiocWithBenchEq, default);
            Assert.IsNotType<UnknownEvent>(e);
        }
    }

    [Fact]
    public void Event_fields_match_the_states_described()
    {
        var law = DeviationLaw.AiocWithBenchEq;
        RadioEvent P(string name, int i = 0)
        {
            var f = Get(name).Frames.Where(x => MessageIds.IsEvent(x.Id)).ElementAt(i);
            return EventParser.Parse(f.Id, f.Body, law, default);
        }

        var open = (BusyEvent)P("event_cd_open");
        Assert.True(open.Busy);
        Assert.Equal(1031u, open.RadioTimeMs);
        Assert.Equal(200, open.Rssi.Raw);
        Assert.Equal(0x10, open.Noise);
        Assert.Equal(0x20, open.Glitch);
        Assert.Equal(RadioEventFlags.TimeExact, open.Flags);

        var burst = (RxBurstEvent)P("event_cd_close_and_rx_burst", 1);
        Assert.Equal(4, burst.Samples);
        Assert.Equal(202, burst.RssiMean.Raw);        // floor((200 + 220 + 180 + 210) / 4)
        Assert.Equal(220, burst.RssiMax.Raw);
        Assert.Equal(180, burst.RssiMin.Raw);
        Assert.Equal(TimeSpan.FromMilliseconds(16), burst.Duration);
        Assert.Equal(0x480, burst.AfAmplitudeMean);
        Assert.Equal(0x600, burst.AfAmplitudeMax);
        Assert.Null(burst.FrequencyErrorHz);

        var start = (TxStartEvent)P("event_tx_start_and_end");
        Assert.True(start.LateKey && start.BusyAtPress && start.IsDeferred);
        Assert.Equal(TimeSpan.FromMilliseconds(12), start.LockDelay);
        Assert.Equal(144_800_000, start.FrequencyHz);
        var end = (TxEndEvent)P("event_tx_start_and_end", 1);
        Assert.Equal(TxEndReason.Released, end.Reason);
        Assert.True(end.IsDeferred);

        var refused = (TxRefusedEvent)P("event_tx_refused");
        Assert.Equal(TxRefusedReason.Lock, refused.Reason);
        Assert.Equal(TimeSpan.FromMilliseconds(1234), refused.LockRemaining);

        var hb = (HeartbeatEvent)P("event_heartbeat");
        Assert.True(hb.IsEphemeral);
        Assert.Equal(250, hb.Microseconds);
        Assert.Equal(0x77, hb.Rssi.Raw);
        Assert.Equal(7800, hb.BatteryMillivolts);

        var stream = (RssiStreamEvent)P("event_rssi_stream");
        Assert.Equal(3, stream.Samples.Count);
        Assert.Equal(TimeSpan.FromMilliseconds(10), stream.Period);
        Assert.Equal(stream.RadioTimeMs - 20, stream.Samples[0].RadioTimeMs);

        var boot = (BootEvent)P("event_boot");
        Assert.Equal(new Version(2, 0), boot.ProtocolVersion);
    }

    /// <summary>
    /// Plays the vectors back through the real client: the fake radio checks each request the
    /// client builds against the vector's request (tag aside) and answers with the vector's
    /// response, tag patched.
    /// </summary>
    [Fact]
    public async Task Client_builds_the_same_requests_and_understands_the_replies()
    {
        var fake = new VectorRadio();
        await using var radio = await K5Radio.ConnectAsync(fake.HostStream,
            new K5RadioOptions { Obfuscate = false, SessionId = 0x12345678, RecoverMissedEvents = false, ResubscribeAfterReboot = false }, cancellationToken: Ct);

        // hello and GET_INFO matched byte for byte (the client's first tag is 1, as in the vector)
        Assert.Equal([Get("hello_plain").Request, Get("get_info").Request], fake.RawRequests);
        var fw = radio.Firmware;
        Assert.Equal(FirmwareKind.PacketV2, fw.Kind);
        Assert.Equal("PKTFW test", fw.Version);
        Assert.Equal(TimeSpan.FromMilliseconds(20), fw.SerialLock);
        Assert.Equal(20, fw.EventRingCapacity);
        Assert.Equal(0, fw.SettingsBlockLayout);
        Assert.Equal(24, fw.SupportedParameters.Count);
        Assert.Equal(RadioCapabilities.LiveTx | RadioCapabilities.RssiBusyDetector | RadioCapabilities.LevelToneRaw
            | RadioCapabilities.RamRegisterOverrides | RadioCapabilities.Persistence | RadioCapabilities.ExactTimeSync, fw.Capabilities);

        var s = await radio.GetStatusAsync(Ct);
        Assert.Equal(144_800_000, s.FrequencyHz);
        Assert.Equal(0x123, s.Rssi.Raw);
        Assert.Equal(0x49, s.Noise);
        Assert.Equal(0x5B, s.Glitch);
        Assert.True(s.Agc.IsFixed);
        Assert.Equal(3, s.Agc.Index);
        Assert.Equal(5, s.BatteryLevel);
        Assert.Equal(7800, s.BatteryMillivolts);
        Assert.Equal(0xFF, s.Channel);   // one operating channel, no memory channels
        Assert.Equal(TimeSpan.FromMilliseconds(1000), s.BusyAge);
        Assert.Null(s.TxTimeLeft);

        var sub = await radio.SubscribeAsync(new EventSubscription { Events = RadioEvents.Busy | RadioEvents.RxBurst }, Ct);
        Assert.Equal(1000u, sub.RadioTimeMs);

        var all = await radio.GetSettingsAsync(cancellationToken: Ct);
        Assert.Equal(24, all.SetIds.Count);
        Assert.Equal(1, all.BusySquelchLevel);
        Assert.Equal(45, all.RxGain);
        Assert.Equal(0x856, all.DeviationWide!.Value.Register);
        Assert.Equal(TimeSpan.FromMilliseconds(20), all.SerialLock);
        Assert.Equal(new Rssi(110), all.BusyRssiOpen);

        var stored = await radio.GetSettingsAsync(true, [RadioParameterId.BusySquelchRaw, RadioParameterId.BusySquelchLevel, RadioParameterId.RxGain], Ct);
        Assert.Null(stored.BusySquelchThresholds);
        Assert.Equal(1, stored.BusySquelchLevel);
        Assert.Equal(45, stored.RxGain);

        var retired = await Assert.ThrowsAsync<K5CommandRejectedException>(() =>
            radio.GetSettingsAsync(false, [RadioParameterId.MicGain, (RadioParameterId)0x07], Ct));
        Assert.Equal(K5Status.Unsupported, retired.Status);
        Assert.Equal(0x07, retired.Detail);
        Assert.Contains("no squelch", retired.Message);

        var set = await radio.SetSettingsAsync(new RadioSettings { FrequencyHz = 433_500_000, BusySquelchLevel = 3, DeviationWide = new Deviation(0x800) }, cancellationToken: Ct);
        Assert.True(set.TxAllowed);
        Assert.True(set.Retuned);
        Assert.Equal(433_500_000, set.Applied.FrequencyHz);

        Assert.Equal([RadioParameterId.MicGain, RadioParameterId.Afc], await radio.RevertSettingsAsync(Ct));

        await radio.SyncClockAsync(1, Ct);
        Assert.True(radio.Clock.IsEstimated);

        Assert.Equal([0x105C, 0x105D, 0x105E, 0], await radio.ReadRegistersAsync(0x5C, 4, cancellationToken: Ct));
        Assert.Equal([0x6240, 0x8000], await radio.WriteRegistersAsync([(0x47, 0x6240), (0x70, 0x8000)], Ct));

        var ov = await radio.AddOverridesAsync([new RegisterOverride(OverridePhase.Rx, 0x2B, 0xFFF8, 0)], TimeSpan.FromSeconds(2), null, Ct);
        Assert.Equal(TimeSpan.FromSeconds(2), ov.ExpiresIn);
        Assert.Null(ov.KeyUpsLeft);
        Assert.Equal(new RegisterOverride(OverridePhase.Rx, 0x2B, 0xFFF8, 0), Assert.Single(ov.Ram));

        var tone = await radio.StartLevelToneRawAsync(1000, 64, TimeSpan.FromMilliseconds(500), Ct);
        Assert.Equal(new LevelToneResult(64, 0x2854), tone);
        var uncal = await Assert.ThrowsAsync<K5CommandRejectedException>(() => radio.StartLevelToneAsync(1000, 3000, TimeSpan.FromSeconds(1), Ct));
        Assert.Equal(K5Status.Unsupported, uncal.Status);

        var c = await radio.GetCountersAsync(cancellationToken: Ct);
        Assert.Equal(4u, c.FramesAccepted);
        Assert.Equal(1u, c.ErrorReplies);

        var events = new List<RadioEvent>();
        radio.EventReceived += (_, e) => { lock (events) { events.Add(e); } };

        // unsolicited events, pushed as the radio would (seq 0, 1, 2, then 5, 6, 7: 3 and 4 missing)
        foreach (string name in new[] { "event_cd_open", "event_cd_close_and_rx_burst", "event_tx_start_and_end", "event_tx_refused", "event_heartbeat", "event_rssi_stream" })
        {
            fake.Push(Get(name));
        }

        await Rig.Until(() => { lock (events) { return events.Count == 8; } });
        lock (events)
        {
            Assert.Equal(
                [RadioEvents.Busy, RadioEvents.Busy, RadioEvents.RxBurst, RadioEvents.TxStart, RadioEvents.TxEnd, RadioEvents.TxRefused, RadioEvents.Heartbeat, RadioEvents.RssiStream],
                events.Select(e => e.Kind));
        }

        // The replay vector re-sends seq 2 to 5: only 3 and 4 are new to the client.
        var replay = await radio.ReplayEventsAsync(2, Ct);
        Assert.Equal(new ReplayResult(2, 4, 0, 6), replay);
        await Task.Delay(50, Ct);
        lock (events)
        {
            Assert.Equal([3, 4], events.Skip(8).Select(e => (int)e.Sequence));
            Assert.All(events.Skip(8), e => Assert.True(e.IsReplay && e is TxRefusedEvent));
        }

        Assert.False(radio.ChannelBusy);
        Assert.Empty(fake.Mismatches);
    }

    /// <summary>
    /// The simulator must answer the state-independent vectors byte for byte as the firmware does
    /// (after the same plain hello, with the same version string and register contents).
    /// </summary>
    [Theory]
    [InlineData("hello_plain")]
    [InlineData("unknown_command")]
    [InlineData("bad_length")]
    [InlineData("reg_read")]
    [InlineData("reg_write")]
    [InlineData("level_tone_raw")]
    [InlineData("level_tone_uncalibrated")]
    [InlineData("set_params_range")]
    [InlineData("get_params_squelch_retired")]
    public async Task Simulator_answers_like_the_firmware(string name)
    {
        using var sim = new Simulation.SimulatedRadio(new Simulation.SimulatedRadioOptions { Version = "PKTFW test", TimeProvider = new Microsoft.Extensions.Time.Testing.FakeTimeProvider() });
        var host = sim.HostStream;
        await host.WriteAsync(Get("hello_obfuscated").Request, Ct);
        await host.WriteAsync(Get("hello_plain").Request, Ct);
        byte[] helloReply = await ReadAvailableAsync(host);
        if (name == "hello_plain")
        {
            Assert.Equal(Get("hello_obfuscated").Response.Concat(Get("hello_plain").Response), helloReply);
            return;
        }

        var v = Get(name);
        await host.WriteAsync(v.Request, Ct);
        Assert.Equal(Convert.ToHexString(v.Response), Convert.ToHexString(await ReadAvailableAsync(host)));
    }

    private static async Task<byte[]> ReadAvailableAsync(Stream s)
    {
        var got = new List<byte>();
        var buf = new byte[1024];
        using var cts = CancellationTokenSource.CreateLinkedTokenSource(Ct);
        cts.CancelAfter(100);
        try
        {
            while (true)
            {
                int n = await s.ReadAsync(buf, cts.Token);
                got.AddRange(buf.AsSpan(0, n).ToArray());
            }
        }
        catch (OperationCanceledException) when (!Ct.IsCancellationRequested)
        {
        }

        return [.. got];
    }

    /// <summary>A fake radio that answers from the vectors.</summary>
    private sealed class VectorRadio
    {
        private readonly (Stream Host, Stream Radio) _pair = LoopbackStream.CreatePair();
        private readonly K5FrameDecoder _decoder = new(obfuscated: false);

        public VectorRadio() => _ = Task.Run(PumpAsync);

        public Stream HostStream => _pair.Host;

        public List<byte[]> RawRequests { get; } = [];

        public List<string> Mismatches { get; } = [];

        public void Push(Vector v)
        {
            foreach (var f in v.Frames)
            {
                Send(f.Id, f.Body, f.Crc == "real");
            }
        }

        private async Task PumpAsync()
        {
            var buf = new byte[1024];
            var raw = new List<byte>();
            while (true)
            {
                int n = await _pair.Radio.ReadAsync(buf);
                if (n == 0)
                {
                    return;
                }

                raw.AddRange(buf.AsSpan(0, n).ToArray());
                foreach (var f in _decoder.Feed(buf.AsSpan(0, n)))
                {
                    RawRequests.Add(K5FrameCodec.EncodePayload(f.Payload.Span, false));
                    Answer(f);
                }
            }
        }

        private void Answer(K5Frame f)
        {
            byte[] body = f.Body.ToArray();
            Vector v = f.Id switch
            {
                MessageIds.Hello => Get("hello_plain"),
                MessageIds.GetInfo => Get("get_info"),
                MessageIds.GetStatus => Get("get_status"),
                MessageIds.Subscribe => Get("subscribe"),
                MessageIds.GetParams => body[1] == 1 ? Get("get_params_stored") : body.Length == 2 ? Get("get_params_all") : Get("get_params_squelch_retired"),
                MessageIds.SetParams => Get("set_params"),
                MessageIds.SaveParams => Get("save_params_revert"),
                MessageIds.TimeSync => Get("time_sync"),
                MessageIds.RegReadV2 => Get("reg_read"),
                MessageIds.RegWriteV2 => Get("reg_write"),
                MessageIds.RegOverride => Get("reg_override_add"),
                MessageIds.LevelTone => body[3] == 1 ? Get("level_tone_raw") : Get("level_tone_uncalibrated"),
                MessageIds.GetCounters => Get("get_counters"),
                MessageIds.EventReplay => Get("event_replay"),
                _ => throw new InvalidOperationException($"no vector for 0x{f.Id:X4}"),
            };

            var expected = Assert.Single(new K5FrameDecoder(v.RequestObfuscated).Feed(v.Request));
            if (!SameRequest(f, expected))
            {
                Mismatches.Add($"{v.Name}: client sent {Convert.ToHexString(body)}, vector has {Convert.ToHexString(expected.Body.Span)}");
            }

            foreach (var r in v.Frames)
            {
                byte[] rb = (byte[])r.Body.Clone();
                if (MessageIds.IsV2(r.Id) && !MessageIds.IsEvent(r.Id))
                {
                    rb[0] = body[0];        // the client's tag
                }

                Send(r.Id, rb, r.Crc == "real");
            }
        }

        private static bool SameRequest(K5Frame got, K5Frame want)
        {
            if (got.Id != want.Id)
            {
                return false;
            }

            ReadOnlySpan<byte> a = got.Body.Span;
            ReadOnlySpan<byte> b = want.Body.Span;
            if (!MessageIds.IsV2(got.Id))
            {
                return a.SequenceEqual(b);
            }

            a = a[1..];     // tags are arbitrary
            b = b[1..];
            return got.Id switch
            {
                MessageIds.TimeSync => a.Length == b.Length,                     // host_ref is the host's own
                MessageIds.SetParams => a[0] == b[0] && Records(a[1..]).SetEquals(Records(b[1..])),   // record order is free
                _ => a.SequenceEqual(b),
            };
        }

        private static HashSet<string> Records(ReadOnlySpan<byte> tlv)
        {
            var set = new HashSet<string>();
            int i = 0;
            while (i < tlv.Length)
            {
                int size = ParameterCodec.SizeOf((RadioParameterId)tlv[i]);
                set.Add(Convert.ToHexString(tlv.Slice(i, 1 + size)));
                i += 1 + size;
            }

            return set;
        }

        private void Send(ushort id, byte[] body, bool realCrc)
        {
            byte[] payload = K5FrameCodec.BuildPayload(id, body);
            _pair.Radio.Write(realCrc ? K5FrameCodec.EncodePayload(payload, false) : K5FrameCodec.EncodeWithoutCrc(payload, false));
        }
    }
}
