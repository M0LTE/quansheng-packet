using System.IO.Ports;

namespace M0LTE.Uvk5;

/// <summary>
/// Opens a UV-K5 serial port (an AIOC's CDC port, typically) the only safe way: 38400 8N1, no
/// handshake, DTR and RTS low, and never a BREAK.
/// </summary>
/// <remarks>
/// <para>The rate is fixed on purpose. On the K1 plug the host-to-radio data line is also PTT, and
/// the radio only rejects serial data as a press because at 38400 baud no byte holds the line low
/// for a whole 280 us window. Measured on the bench: runs of zero bytes at 9600 baud keyed the
/// radio for 0.46 s, at 19200 for 0.12 s, at 38400 not at all. A BREAK holds the line low and
/// keys it too.</para>
/// <para>DTR and RTS are set low before the port opens. The AIOC's default serial PTT is DTR=1 with
/// RTS=0; Linux raises both on open (not keyed), and the port then ends with both low without
/// passing through DTR=1, RTS=0.</para>
/// </remarks>
public static class K5SerialPort
{
    /// <summary>The only baud rate this library uses.</summary>
    public const int BaudRate = 38400;

    /// <summary>Opens <paramref name="portName"/> and returns a duplex stream that closes the port when disposed.</summary>
    public static Stream Open(string portName)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(portName);
        var port = new SerialPort(portName, BaudRate, Parity.None, 8, StopBits.One)
        {
            Handshake = Handshake.None,
            DtrEnable = false,
            RtsEnable = false,
            ReadTimeout = SerialPort.InfiniteTimeout,
            WriteTimeout = 2000,
            ReadBufferSize = 8192,
        };
        port.Open();
        port.DiscardInBuffer();
        return new PortStream(port);
    }

    /// <summary>Lists serial ports the OS knows about.</summary>
    public static string[] ListPorts() => SerialPort.GetPortNames();

    private sealed class PortStream(SerialPort port) : Stream
    {
        private readonly Stream _inner = port.BaseStream;

        public override bool CanRead => true;

        public override bool CanSeek => false;

        public override bool CanWrite => true;

        public override long Length => throw new NotSupportedException();

        public override long Position { get => throw new NotSupportedException(); set => throw new NotSupportedException(); }

        public override void Flush() => _inner.Flush();

        public override Task FlushAsync(CancellationToken cancellationToken) => _inner.FlushAsync(cancellationToken);

        public override int Read(byte[] buffer, int offset, int count) => _inner.Read(buffer, offset, count);

        public override ValueTask<int> ReadAsync(Memory<byte> buffer, CancellationToken cancellationToken = default) => _inner.ReadAsync(buffer, cancellationToken);

        public override Task<int> ReadAsync(byte[] buffer, int offset, int count, CancellationToken cancellationToken) => _inner.ReadAsync(buffer, offset, count, cancellationToken);

        public override void Write(byte[] buffer, int offset, int count) => _inner.Write(buffer, offset, count);

        public override ValueTask WriteAsync(ReadOnlyMemory<byte> buffer, CancellationToken cancellationToken = default) => _inner.WriteAsync(buffer, cancellationToken);

        public override Task WriteAsync(byte[] buffer, int offset, int count, CancellationToken cancellationToken) => _inner.WriteAsync(buffer, offset, count, cancellationToken);

        public override long Seek(long offset, SeekOrigin origin) => throw new NotSupportedException();

        public override void SetLength(long value) => throw new NotSupportedException();

        protected override void Dispose(bool disposing)
        {
            if (disposing)
            {
                try
                {
                    port.Close();
                }
                finally
                {
                    port.Dispose();
                }
            }

            base.Dispose(disposing);
        }
    }
}
