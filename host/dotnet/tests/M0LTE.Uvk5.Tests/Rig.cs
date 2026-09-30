using M0LTE.Uvk5.Simulation;

namespace M0LTE.Uvk5.Tests;

/// <summary>A simulated radio and a client connected to it.</summary>
internal sealed class Rig : IAsyncDisposable
{
    private Rig(SimulatedRadio sim, K5Radio radio)
    {
        Sim = sim;
        Radio = radio;
    }

    public SimulatedRadio Sim { get; }

    public K5Radio Radio { get; }

    public List<string> Trace { get; } = [];

    public static async Task<Rig> StartAsync(
        FirmwareKind kind = FirmwareKind.PacketV2,
        TimeProvider? time = null,
        K5RadioOptions? options = null,
        SimulatedRadioOptions? simOptions = null)
    {
        time ??= TimeProvider.System;
        var sim = new SimulatedRadio((simOptions ?? new SimulatedRadioOptions()) with { Firmware = kind, TimeProvider = time });
        var o = (options ?? new K5RadioOptions()) with { TimeProvider = time };
        var radio = await K5Radio.ConnectAsync(sim.HostStream, o, ownsStream: false, TestContext.Current.CancellationToken);
        return new Rig(sim, radio);
    }

    public async ValueTask DisposeAsync()
    {
        await Radio.DisposeAsync();
        Sim.Dispose();
    }

    /// <summary>Waits (real time) until <paramref name="condition"/> holds.</summary>
    public static async Task Until(Func<bool> condition, int timeoutMs = 3000)
    {
        var start = DateTime.UtcNow;
        while (!condition())
        {
            if ((DateTime.UtcNow - start).TotalMilliseconds > timeoutMs)
            {
                throw new TimeoutException("condition not met");
            }

            await Task.Delay(5);
        }
    }
}
