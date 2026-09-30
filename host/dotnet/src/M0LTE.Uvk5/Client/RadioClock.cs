using System.Globalization;

namespace M0LTE.Uvk5;

/// <summary>
/// Maps the radio's millisecond clock to the host clock, from TIME_SYNC exchanges and from
/// heartbeats and other exactly-timed event frames. Thread-safe.
/// </summary>
/// <remarks>
/// <para>The radio clock runs from the MCU's internal RC oscillator, so it drifts; the estimate
/// fits a rate (skew) as well as an offset once samples span at least 10 s.</para>
/// <para>Every sample is an upper bound on the true offset (host time minus radio time): a frame
/// can only arrive late, never early. The estimate is a line under the lowest samples in each
/// time bucket, which removes USB and scheduling jitter; what remains is the minimum latency of
/// the path, about 1 ms.</para>
/// </remarks>
public sealed class RadioClock
{
    private const int MaxSamples = 256;
    private const int Buckets = 8;

    private readonly TimeProvider _time;
    private readonly long _monoAnchor;
    private readonly DateTimeOffset _utcAnchor;
    private readonly Lock _gate = new();
    private readonly List<(double RadioMs, double OffsetMs)> _samples = [];
    private double _offsetMs;       // at _refRadioMs
    private double _refRadioMs;
    private double _skew;           // host ms per radio ms, minus 1
    private double _residualMs;
    private bool _estimated;

    internal RadioClock(TimeProvider time)
    {
        _time = time;
        _monoAnchor = time.GetTimestamp();
        _utcAnchor = time.GetUtcNow();
    }

    /// <summary>True once at least one sample has been taken.</summary>
    public bool IsEstimated
    {
        get
        {
            lock (_gate)
            {
                return _estimated;
            }
        }
    }

    /// <summary>Samples in the estimate.</summary>
    public int SampleCount
    {
        get
        {
            lock (_gate)
            {
                return _samples.Count;
            }
        }
    }

    /// <summary>Estimated rate error of the radio clock, parts per million (positive: the radio runs slow), null until samples span 10 s.</summary>
    public double? SkewPpm
    {
        get
        {
            lock (_gate)
            {
                return SpanMs() >= 10_000 ? _skew * 1e6 : null;
            }
        }
    }

    /// <summary>Scatter of the bucket minima around the fit, a rough uncertainty of the mapping.</summary>
    public TimeSpan Uncertainty
    {
        get
        {
            lock (_gate)
            {
                return TimeSpan.FromMilliseconds(_residualMs + 1.0);
            }
        }
    }

    /// <summary>The host time of radio time <paramref name="radioMs"/>, null before any sample.</summary>
    public DateTimeOffset? ToHostTime(double radioMs)
    {
        lock (_gate)
        {
            if (!_estimated)
            {
                return null;
            }

            double hostMs = radioMs + _offsetMs + _skew * (radioMs - _refRadioMs);
            return _utcAnchor + TimeSpan.FromMilliseconds(hostMs);
        }
    }

    /// <summary>Radio time now, estimated, null before any sample.</summary>
    public double? RadioNowMs
    {
        get
        {
            double host = HostMs(_time.GetTimestamp());
            lock (_gate)
            {
                if (!_estimated)
                {
                    return null;
                }

                // host = r + off + skew (r - ref)  =>  r = (host - off + skew ref) / (1 + skew)
                return (host - _offsetMs + _skew * _refRadioMs) / (1 + _skew);
            }
        }
    }

    /// <inheritdoc/>
    public override string ToString()
    {
        lock (_gate)
        {
            if (!_estimated)
            {
                return "not estimated";
            }

            string skew = SpanMs() >= 10_000 ? string.Create(CultureInfo.InvariantCulture, $", skew {_skew * 1e6:F0} ppm") : string.Empty;
            return string.Create(CultureInfo.InvariantCulture, $"{_samples.Count} samples, +/-{_residualMs + 1.0:F1} ms{skew}");
        }
    }

    internal double HostMs(long timestamp) => _time.GetElapsedTime(_monoAnchor, timestamp).TotalMilliseconds;

    /// <summary>Adds a sample: radio time <paramref name="radioMs"/> happened no later than host timestamp <paramref name="hostTimestamp"/>.</summary>
    internal void AddUpperBound(double radioMs, long hostTimestamp) => Add(radioMs, HostMs(hostTimestamp) - radioMs);

    /// <summary>Adds a symmetric TIME_SYNC estimate (host ms minus radio ms) with its half-width.</summary>
    internal void AddSymmetric(double radioMs, double offsetMs, double halfWidthMs) => Add(radioMs, offsetMs + Math.Max(0, halfWidthMs));

    /// <summary>The radio rebooted: its clock restarted.</summary>
    internal void Reset()
    {
        lock (_gate)
        {
            _samples.Clear();
            _estimated = false;
            _skew = 0;
            _residualMs = 0;
        }
    }

    private void Add(double radioMs, double offsetMs)
    {
        lock (_gate)
        {
            if (_samples.Count > 0 && radioMs + 1000 < _samples[^1].RadioMs)
            {
                _samples.Clear();       // the radio clock went backwards: reboot
            }

            _samples.Add((radioMs, offsetMs));
            if (_samples.Count > MaxSamples)
            {
                _samples.RemoveAt(0);
            }

            Fit();
            _estimated = true;
        }
    }

    private double SpanMs() => _samples.Count < 2 ? 0 : _samples[^1].RadioMs - _samples[0].RadioMs;

    private void Fit()
    {
        double span = SpanMs();
        if (span < 10_000)
        {
            // Not long enough for a rate: the lowest recent sample.
            double min = double.MaxValue;
            double at = 0;
            foreach (var (r, o) in _samples)
            {
                if (o < min)
                {
                    min = o;
                    at = r;
                }
            }

            _offsetMs = min;
            _refRadioMs = at;
            _skew = 0;
            _residualMs = 0;
            return;
        }

        double first = _samples[0].RadioMs;
        var minima = new (double R, double O)?[Buckets];
        foreach (var (r, o) in _samples)
        {
            int b = Math.Min(Buckets - 1, (int)((r - first) / span * Buckets));
            if (minima[b] is not { } m || o < m.O)
            {
                minima[b] = (r, o);
            }
        }

        var pts = minima.Where(m => m.HasValue).Select(m => m!.Value).ToArray();
        double mr = pts.Average(p => p.R);
        double mo = pts.Average(p => p.O);
        double sxx = pts.Sum(p => (p.R - mr) * (p.R - mr));
        double sxy = pts.Sum(p => (p.R - mr) * (p.O - mo));
        double slope = sxx > 0 ? sxy / sxx : 0;
        _skew = slope;
        _refRadioMs = mr;

        // A lower envelope: shift the line down to the lowest point.
        double shift = pts.Min(p => p.O - (mo + slope * (p.R - mr)));
        _offsetMs = mo + shift;
        _residualMs = pts.Max(p => p.O - (_offsetMs + slope * (p.R - mr)));
    }
}
