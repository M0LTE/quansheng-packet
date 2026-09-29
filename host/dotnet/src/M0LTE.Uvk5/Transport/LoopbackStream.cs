namespace M0LTE.Uvk5;

/// <summary>
/// An in-memory duplex byte pipe: what one end writes, the other reads. No pty, no OS resources,
/// so tests and simulators run anywhere. Thread-safe.
/// </summary>
public static class LoopbackStream
{
    /// <summary>Creates two connected streams. Disposing either ends both (reads return 0).</summary>
    public static (Stream A, Stream B) CreatePair()
    {
        var ab = new ByteQueue();
        var ba = new ByteQueue();
        return (new End(ba, ab), new End(ab, ba));
    }

    internal sealed class End(ByteQueue incoming, ByteQueue outgoing) : Stream
    {
        public override bool CanRead => true;

        public override bool CanSeek => false;

        public override bool CanWrite => true;

        public override long Length => throw new NotSupportedException();

        public override long Position { get => throw new NotSupportedException(); set => throw new NotSupportedException(); }

        public override void Flush()
        {
        }

        public override Task FlushAsync(CancellationToken cancellationToken) => Task.CompletedTask;

        public override int Read(byte[] buffer, int offset, int count) =>
            ReadAsync(buffer.AsMemory(offset, count)).AsTask().GetAwaiter().GetResult();

        public override ValueTask<int> ReadAsync(Memory<byte> buffer, CancellationToken cancellationToken = default) =>
            incoming.ReadAsync(buffer, cancellationToken);

        public override Task<int> ReadAsync(byte[] buffer, int offset, int count, CancellationToken cancellationToken) =>
            ReadAsync(buffer.AsMemory(offset, count), cancellationToken).AsTask();

        public override void Write(byte[] buffer, int offset, int count) => outgoing.Write(buffer.AsSpan(offset, count));

        public override ValueTask WriteAsync(ReadOnlyMemory<byte> buffer, CancellationToken cancellationToken = default)
        {
            outgoing.Write(buffer.Span);
            return ValueTask.CompletedTask;
        }

        public override Task WriteAsync(byte[] buffer, int offset, int count, CancellationToken cancellationToken)
        {
            outgoing.Write(buffer.AsSpan(offset, count));
            return Task.CompletedTask;
        }

        public override long Seek(long offset, SeekOrigin origin) => throw new NotSupportedException();

        public override void SetLength(long value) => throw new NotSupportedException();

        protected override void Dispose(bool disposing)
        {
            if (disposing)
            {
                incoming.Complete();
                outgoing.Complete();
            }

            base.Dispose(disposing);
        }
    }
}

/// <summary>A byte FIFO with an async reader. Thread-safe.</summary>
internal sealed class ByteQueue
{
    private readonly Lock _gate = new();
    private readonly Queue<byte[]> _chunks = new();
    private int _offset;
    private bool _completed;
    private TaskCompletionSource? _waiter;

    public void Write(ReadOnlySpan<byte> data)
    {
        if (data.IsEmpty)
        {
            return;
        }

        TaskCompletionSource? wake;
        lock (_gate)
        {
            if (_completed)
            {
                throw new ObjectDisposedException(nameof(LoopbackStream), "the other end is closed");
            }

            _chunks.Enqueue(data.ToArray());
            wake = _waiter;
            _waiter = null;
        }

        wake?.TrySetResult();
    }

    public void Complete()
    {
        TaskCompletionSource? wake;
        lock (_gate)
        {
            _completed = true;
            wake = _waiter;
            _waiter = null;
        }

        wake?.TrySetResult();
    }

    public async ValueTask<int> ReadAsync(Memory<byte> buffer, CancellationToken ct)
    {
        while (true)
        {
            Task wait;
            lock (_gate)
            {
                if (_chunks.Count > 0)
                {
                    int n = 0;
                    while (n < buffer.Length && _chunks.Count > 0)
                    {
                        byte[] c = _chunks.Peek();
                        int take = Math.Min(buffer.Length - n, c.Length - _offset);
                        c.AsSpan(_offset, take).CopyTo(buffer.Span[n..]);
                        n += take;
                        _offset += take;
                        if (_offset == c.Length)
                        {
                            _chunks.Dequeue();
                            _offset = 0;
                        }
                    }

                    return n;
                }

                if (_completed)
                {
                    return 0;
                }

                _waiter ??= new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
                wait = _waiter.Task;
            }

            await wait.WaitAsync(ct).ConfigureAwait(false);
        }
    }
}
