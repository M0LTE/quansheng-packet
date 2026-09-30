using System.Collections;

namespace M0LTE.Uvk5.Client;

/// <summary>
/// Tracks event sequence numbers (protocol v2 section 4.4): finds gaps, drops duplicates (a replay
/// of something already delivered), and notices a radio reboot. Thread-safe.
/// </summary>
internal sealed class EventSequencer
{
    private readonly Lock _gate = new();
    private readonly BitArray _seen = new(65536);
    private ushort? _expected;
    private uint _lastRadioTime;
    private ushort? _missingFrom;
    private ushort _missingTo;

    public enum Verdict
    {
        New,
        Gap,
        Duplicate,
        Rebooted,
    }

    /// <summary>First seq known to be missing, or null.</summary>
    public ushort? MissingFrom
    {
        get
        {
            lock (_gate)
            {
                return _missingFrom;
            }
        }
    }

    /// <summary>The seq the next stored event should have, or null before any event.</summary>
    public ushort? Expected
    {
        get
        {
            lock (_gate)
            {
                return _expected;
            }
        }
    }

    public void Reset()
    {
        lock (_gate)
        {
            _seen.SetAll(false);
            _expected = null;
            _missingFrom = null;
            _lastRadioTime = 0;
        }
    }

    /// <summary>Starts tracking from what a SUBSCRIBE or GET_STATUS reply says the next seq is.</summary>
    public void Prime(ushort nextSeq)
    {
        lock (_gate)
        {
            _expected ??= nextSeq;
        }
    }

    /// <summary>A replay was done: whatever is still missing is gone for good (EVENTS_LOST says so).</summary>
    public void ReplayDone()
    {
        lock (_gate)
        {
            _missingFrom = null;
        }
    }

    public Verdict Accept(RadioEvent e)
    {
        lock (_gate)
        {
            ushort seq = e.Sequence;
            bool ephemeral = e.IsEphemeral;
            bool replay = e.IsReplay;
            if (!replay && _expected is not null && e.RadioTimeMs + 1000 < _lastRadioTime)
            {
                _seen.SetAll(false);
                _expected = null;
                _missingFrom = null;
                _lastRadioTime = 0;
                First(e);
                return Verdict.Rebooted;
            }

            if (_expected is not { } expected)
            {
                if (replay)
                {
                    Mark(seq);
                    return Verdict.New;
                }

                First(e);
                return Verdict.New;
            }

            if (!replay)
            {
                _lastRadioTime = Math.Max(_lastRadioTime, e.RadioTimeMs);
            }

            if (replay)
            {
                if (_seen[seq])
                {
                    return Verdict.Duplicate;
                }

                Mark(seq);
                AdvanceMissing();
                return Verdict.New;
            }

            int d = (ushort)(seq - expected);
            if (ephemeral)
            {
                if (d == 0 || d >= 32768)
                {
                    return Verdict.New;
                }

                NoteMissing(expected, seq);
                _expected = seq;
                return Verdict.Gap;
            }

            if (_seen[seq])
            {
                return Verdict.Duplicate;
            }

            Mark(seq);
            if (d == 0)
            {
                _expected = (ushort)(seq + 1);
                return Verdict.New;
            }

            if (d < 32768)
            {
                NoteMissing(expected, seq);
                _expected = (ushort)(seq + 1);
                return Verdict.Gap;
            }

            return Verdict.New;
        }
    }

    private void First(RadioEvent e)
    {
        _lastRadioTime = e.RadioTimeMs;
        if (e.IsEphemeral)
        {
            _expected = e.Sequence;
        }
        else
        {
            Mark(e.Sequence);
            _expected = (ushort)(e.Sequence + 1);
        }
    }

    private void NoteMissing(ushort from, ushort to)
    {
        _missingFrom ??= from;
        _missingTo = to;
        AdvanceMissing();
    }

    private void AdvanceMissing()
    {
        while (_missingFrom is { } m && (_seen[m] || m == _missingTo))
        {
            _missingFrom = m == _missingTo ? null : (ushort)(m + 1);
            if (_missingFrom == _missingTo)
            {
                _missingFrom = null;
            }
        }
    }

    private void Mark(ushort seq)
    {
        _seen[seq] = true;
        _seen[(ushort)(seq + 32768)] = false;
    }
}
