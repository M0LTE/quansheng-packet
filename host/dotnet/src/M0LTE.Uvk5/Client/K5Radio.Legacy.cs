using System.Buffers.Binary;
using M0LTE.Uvk5.Protocol;

namespace M0LTE.Uvk5;

public sealed partial class K5Radio
{
    private const int EepromReadChunk = 0x80;
    private const int EepromWriteChunk = 0x40;

    /// <summary>
    /// RSSI, noise and glitch now (legacy 0x0527, every firmware). Each call is a frame and so
    /// costs a serial lock (1.5 s on v1): on v2 prefer busy events and burst reports.
    /// </summary>
    public async Task<RssiReading> ReadRssiAsync(CancellationToken cancellationToken = default)
    {
        K5Frame f = await LegacyAsync("RSSI", MessageIds.Rssi, [], x => x.Id == MessageIds.RssiReply && x.Body.Length >= 4, cancellationToken).ConfigureAwait(false);
        var r = new WireReader(f.Body.Span, "RSSI reply");
        return new RssiReading(new Rssi((ushort)(r.U16() & 0x1FF)), r.U8() & 0x7F, r.U8());
    }

    /// <summary>
    /// Battery voltage. v2: from GET_STATUS. Older firmware: the raw ADC reading (0x0529) scaled by
    /// the radio's battery calibration (EEPROM 0x1F40, read once; never written).
    /// </summary>
    public async Task<BatteryReading> ReadBatteryAsync(CancellationToken cancellationToken = default)
    {
        if (Firmware.Kind == FirmwareKind.PacketV2)
        {
            var s = await GetStatusAsync(cancellationToken).ConfigureAwait(false);
            return new BatteryReading { Volts = s.BatteryVolts, Level = s.BatteryLevel };
        }

        K5Frame f = await LegacyAsync("battery", MessageIds.Battery, [], x => x.Id == MessageIds.BatteryReply && x.Body.Length >= 2, cancellationToken).ConfigureAwait(false);
        int raw = BinaryPrimitives.ReadUInt16LittleEndian(f.Body.Span);
        if (_batteryCalibration is null)
        {
            byte[] cal = await ReadEepromAsync(0x1F40, 12, cancellationToken).ConfigureAwait(false);
            _batteryCalibration = BinaryPrimitives.ReadUInt16LittleEndian(cal.AsSpan(6));
        }

        ushort c = _batteryCalibration.Value;
        return new BatteryReading { RawAdc = raw, Volts = c is 0 or 0xFFFF ? null : raw * 7.6 / c };
    }

    /// <summary>Reads EEPROM (legacy 0x051B, in 128-byte chunks). Reading is always safe, the calibration area included.</summary>
    public async Task<byte[]> ReadEepromAsync(int address, int length, CancellationToken cancellationToken = default, IProgress<double>? progress = null)
    {
        if (address < 0 || length < 0 || address + length > K5Safety.EepromSize)
        {
            throw new ArgumentOutOfRangeException(nameof(address), $"read 0x{address:X4}+{length} is outside the EEPROM");
        }

        var result = new byte[length];
        int done = 0;
        while (done < length)
        {
            int n = Math.Min(EepromReadChunk, length - done);
            int a = address + done;
            var body = new WireWriter().U16(a).U8(n).U8(0).U32(_sessionId).ToArray();
            K5Frame f = await LegacyAsync($"EEPROM read 0x{a:X4}", MessageIds.EepromRead, body,
                x => x.Id == MessageIds.EepromReadReply && x.Body.Length >= 4 && BinaryPrimitives.ReadUInt16LittleEndian(x.Body.Span) == a,
                cancellationToken).ConfigureAwait(false);
            ReadOnlySpan<byte> b = f.Body.Span;
            int size = b[2];
            if (size != n || b.Length < 4 + n)
            {
                throw new K5ProtocolException($"EEPROM read at 0x{a:X4} returned {Math.Min(size, b.Length - 4)} bytes, wanted {n}");
            }

            b.Slice(4, n).CopyTo(result.AsSpan(done));
            done += n;
            progress?.Report((double)done / length);
        }

        return result;
    }

    /// <summary>
    /// Reads the whole EEPROM twice, requires both reads to match and the image not to be blank,
    /// and returns it as a backup. Refused while the radio shows its lock screen. The backup also
    /// authorises EEPROM writes for this session (it is, by construction, of this radio). Save it
    /// with <see cref="EepromBackup.SaveAsync"/>.
    /// </summary>
    public async Task<EepromBackup> BackupEepromAsync(IProgress<double>? progress = null, CancellationToken cancellationToken = default)
    {
        if (Firmware.InLockScreen)
        {
            throw new K5SafetyException("the radio reports its lock screen: unlock it before taking a backup");
        }

        var p1 = progress is null ? null : new Progress<double>(x => progress.Report(x / 2));
        var p2 = progress is null ? null : new Progress<double>(x => progress.Report(0.5 + x / 2));
        byte[] first = await ReadEepromAsync(0, K5Safety.EepromSize, cancellationToken, p1).ConfigureAwait(false);
        byte[] second = await ReadEepromAsync(0, K5Safety.EepromSize, cancellationToken, p2).ConfigureAwait(false);
        int diff = first.AsSpan().CommonPrefixLength(second);
        if (diff != first.Length)
        {
            throw new K5Exception($"the two EEPROM reads differ (first at 0x{diff:X4}); no backup made");
        }

        var backup = EepromBackup.FromImage(first, Firmware.Version, _time.GetUtcNow());
        _authorizedBackup = backup;
        return backup;
    }

    /// <summary>
    /// Allows EEPROM writes for this session, given a verified backup of this radio: its
    /// calibration area (0x1E00 to 0x1FFF) must match a fresh read, so a backup of another radio is
    /// refused with <see cref="K5SafetyException"/>.
    /// </summary>
    public async Task AuthorizeEepromWritesAsync(EepromBackup backup, CancellationToken cancellationToken = default)
    {
        ArgumentNullException.ThrowIfNull(backup);
        byte[] live = await ReadEepromAsync(K5Safety.CalibrationStart, K5Safety.EepromSize - K5Safety.CalibrationStart, cancellationToken).ConfigureAwait(false);
        int same = live.AsSpan().CommonPrefixLength(backup.Calibration.Span);
        if (same != live.Length)
        {
            throw new K5SafetyException(
                $"the backup's calibration area differs from this radio's (first at 0x{K5Safety.CalibrationStart + same:X4}): it is a backup of another radio, refused");
        }

        _authorizedBackup = backup;
    }

    /// <summary>
    /// Writes EEPROM (legacy 0x051D, 64-byte frames) and reads it back. Needs
    /// <see cref="AuthorizeEepromWritesAsync"/> (or <see cref="BackupEepromAsync"/>) first, 8-byte
    /// alignment, and never touches 0x1E00 and up: refused with <see cref="K5SafetyException"/>
    /// before anything is sent. Every block write is reported to <see cref="K5RadioOptions.Audit"/>
    /// whatever the answer. Packet firmware applies settings-area changes about 1 to 1.5 s after the
    /// serial session goes quiet.
    /// </summary>
    public async Task WriteEepromAsync(int address, ReadOnlyMemory<byte> data, CancellationToken cancellationToken = default)
    {
        K5Safety.CheckEepromWrite(address, data.Length);
        EepromBackup backup = _authorizedBackup ?? throw new K5SafetyException(
            "EEPROM writes need a verified backup of this radio first: BackupEepromAsync or AuthorizeEepromWritesAsync");

        for (int pos = 0; pos < data.Length; pos += EepromWriteChunk)
        {
            int n = Math.Min(EepromWriteChunk, data.Length - pos);
            int a = address + pos;
            K5Safety.CheckEepromWrite(a, n);
            ReadOnlyMemory<byte> chunk = data.Slice(pos, n);
            var body = new WireWriter().U16(a).U8(n).U8(0).U32(_sessionId).Bytes(chunk.Span).ToArray();
            string outcome = "timeout";
            try
            {
                await LegacyAsync($"EEPROM write 0x{a:X4}", MessageIds.EepromWrite, body,
                    x => x.Id == MessageIds.EepromWriteReply && x.Body.Length >= 2 && BinaryPrimitives.ReadUInt16LittleEndian(x.Body.Span) == a,
                    cancellationToken).ConfigureAwait(false);
                outcome = "ok";
            }
            catch (Exception e) when (e is not OperationCanceledException)
            {
                outcome = e is K5TimeoutException ? "timeout (packet firmware refuses a bad write with no reply)" : e.Message;
                throw;
            }
            finally
            {
                Audit(K5AuditKind.EepromWrite, $"EEPROM write 0x{a:X4}, {n} bytes", outcome, new Dictionary<string, string>
                {
                    ["addr"] = $"0x{a:X4}",
                    ["data"] = Convert.ToHexStringLower(chunk.Span),
                    ["backup_had"] = Convert.ToHexStringLower(backup.Data.Span.Slice(a, n)),
                    ["backup_sha256"] = backup.Sha256,
                });
            }
        }

        byte[] back = await ReadEepromAsync(address, data.Length, cancellationToken).ConfigureAwait(false);
        int same = back.AsSpan().CommonPrefixLength(data.Span);
        if (same != data.Length)
        {
            throw new K5Exception($"EEPROM read-back differs at 0x{address + same:X4} after writing");
        }
    }

    /// <summary>Reboots the radio (0x05DD; packet firmware de-keys first). No reply; the library notices the power-on banner.</summary>
    public async Task RebootAsync(CancellationToken cancellationToken = default) =>
        await ExchangeAsync("reboot", MessageIds.Reboot, [], null, TimeSpan.Zero, cancellationToken).ConfigureAwait(false);

    /// <summary>Reads one BK4819 register (v2 REG_READ, else legacy 0x0601). 0x5F (the FSK FIFO) is refused: reading it pops data.</summary>
    public async Task<ushort> ReadRegisterAsync(int register, CancellationToken cancellationToken = default)
    {
        if (register == 0x5F)
        {
            throw new ArgumentException("0x5F is the FSK RX FIFO: reading it pops data. Use ReadRegistersAsync with includeFifo if you mean it.", nameof(register));
        }

        return (await ReadRegistersAsync(register, 1, false, cancellationToken).ConfigureAwait(false))[0];
    }

    /// <summary>
    /// Reads <paramref name="count"/> consecutive BK4819 registers from <paramref name="first"/>.
    /// 0x5F reads as 0 unless <paramref name="includeFifo"/>. v2: two requests dump the chip; older
    /// firmware: one 0x0601 per register (about 2 s for all 128 on v1).
    /// </summary>
    public async Task<ushort[]> ReadRegistersAsync(int first, int count, bool includeFifo = false, CancellationToken cancellationToken = default)
    {
        if (first < 0 || count < 1 || first + count > 0x80)
        {
            throw new ArgumentOutOfRangeException(nameof(count), "registers 0x00 to 0x7F");
        }

        var values = new ushort[count];
        if (Firmware.Kind == FirmwareKind.PacketV2)
        {
            for (int done = 0; done < count;)
            {
                int n = Math.Min(64, count - done);
                byte[] b = await V2Async("REG_READ", MessageIds.RegReadV2, [(byte)(first + done), (byte)n, includeFifo ? (byte)1 : (byte)0], true, cancellationToken).ConfigureAwait(false);
                var r = new WireReader(b, "REG_READ reply");
                r.U8();
                int got = r.U8();
                if (got != n)
                {
                    throw new K5ProtocolException($"REG_READ returned {got} registers, wanted {n}");
                }

                for (int i = 0; i < n; i++)
                {
                    values[done + i] = r.U16();
                }

                done += n;
            }

            return values;
        }

        for (int i = 0; i < count; i++)
        {
            int reg = first + i;
            if (reg == 0x5F && !includeFifo)
            {
                continue;
            }

            values[i] = await LegacyRegisterReadAsync(reg, cancellationToken).ConfigureAwait(false);
        }

        return values;
    }

    /// <summary>
    /// Writes BK4819 registers in order and returns the read-back values. Every register is checked
    /// against the refusal list first (<see cref="K5Safety.RefusedRegisters"/>), on every firmware.
    /// v2: one REG_WRITE, never retried (re-read instead). Older firmware: 0x0602 per register (no
    /// reply) then a 0x0601 read-back. Registers the firmware manages (7D, 40, 47, 48, 7E, 2B, 43,
    /// 31, squelch) are rewritten at its next set-up: use settings or overrides for those.
    /// </summary>
    public async Task<ushort[]> WriteRegistersAsync(IReadOnlyList<(int Register, ushort Value)> writes, CancellationToken cancellationToken = default)
    {
        ArgumentNullException.ThrowIfNull(writes);
        if (writes.Count is 0 or > 16)
        {
            throw new ArgumentOutOfRangeException(nameof(writes), writes.Count, "1 to 16 register writes");
        }

        foreach (var (reg, _) in writes)
        {
            K5Safety.CheckRegisterWrite(reg);
        }

        var readBack = new ushort[writes.Count];
        string outcome = "timeout";
        try
        {
            if (Firmware.Kind == FirmwareKind.PacketV2)
            {
                var w = new WireWriter().U8(writes.Count);
                foreach (var (reg, value) in writes)
                {
                    w.U8(reg).U16(value);
                }

                byte[] b = await V2Async("REG_WRITE", MessageIds.RegWriteV2, w.ToArray(), false, cancellationToken).ConfigureAwait(false);
                var r = new WireReader(b, "REG_WRITE reply");
                int n = r.U8();
                for (int i = 0; i < n && i < readBack.Length; i++)
                {
                    r.U8();
                    readBack[i] = r.U16();
                }
            }
            else
            {
                for (int i = 0; i < writes.Count; i++)
                {
                    var (reg, value) = writes[i];
                    await ExchangeAsync($"register write 0x{reg:X2}", MessageIds.RegWrite, new WireWriter().U8(reg).U16(value).ToArray(), null, TimeSpan.Zero, cancellationToken).ConfigureAwait(false);
                    readBack[i] = await LegacyRegisterReadAsync(reg, cancellationToken).ConfigureAwait(false);
                }
            }

            outcome = "ok";
            return readBack;
        }
        catch (Exception e) when (e is not OperationCanceledException)
        {
            outcome = e.Message;
            throw;
        }
        finally
        {
            Audit(K5AuditKind.RegisterWrite, $"register write {string.Join(", ", writes.Select(x => $"0x{x.Register:X2}=0x{x.Value:X4}"))}", outcome,
                new Dictionary<string, string> { ["readback"] = string.Join(" ", readBack.Select(v => $"0x{v:X4}")) });
        }
    }

    private async Task<ushort> LegacyRegisterReadAsync(int reg, CancellationToken ct)
    {
        try
        {
            K5Frame f = await LegacyAsync($"register read 0x{reg:X2}", MessageIds.RegRead, [(byte)reg],
                x => x.Id == MessageIds.RegRead && x.Body.Length >= 3 && x.Body.Span[0] == reg, ct).ConfigureAwait(false);
            return BinaryPrimitives.ReadUInt16LittleEndian(f.Body.Span[1..]);
        }
        catch (K5TimeoutException) when (Firmware.Kind == FirmwareKind.Stock)
        {
            throw new K5FirmwareNotSupportedException("register access", Firmware.Kind, "a firmware built with the BK4819 register commands (0x0601/0x0602), as the packet firmware always is");
        }
    }

    private async Task<(byte[] Block, byte[] Timing)> ReadV1BlocksAsync(CancellationToken ct)
    {
        byte[] block = await ReadEepromAsync(SettingsBlock.Address, SettingsBlock.Length, ct).ConfigureAwait(false);
        byte[] timing = await ReadEepromAsync(SettingsBlock.TimingAddress, SettingsBlock.TimingLength, ct).ConfigureAwait(false);
        Firmware = Firmware with { SettingsBlockLayout = SettingsBlock.IsValid(block) ? block[0] : 0 };
        return (block, timing);
    }

    private async Task<RadioSettings> ReadV1SettingsAsync(CancellationToken ct)
    {
        var (block, timing) = await ReadV1BlocksAsync(ct).ConfigureAwait(false);
        if (!SettingsBlock.IsValid(block))
        {
            // The firmware runs on its defaults; report them.
            block = [SettingsBlock.Layout, .. Enumerable.Repeat((byte)0xFF, SettingsBlock.Length - 1)];
            timing = [.. Enumerable.Repeat((byte)0xFF, SettingsBlock.TimingLength)];
        }

        var s = SettingsBlock.Decode(block, timing, _options.DeviationLaw);
        if (s.PttReleaseDebounce is { } rel)
        {
            _pttRelease = rel;
        }

        return s;
    }

    private async Task<SetSettingsResult> WriteV1SettingsAsync(RadioSettings changes, CancellationToken ct)
    {
        if (_authorizedBackup is null)
        {
            throw new K5SafetyException("on packet firmware v1 settings live in EEPROM: call BackupEepromAsync or AuthorizeEepromWritesAsync first");
        }

        var (block, timing) = await ReadV1BlocksAsync(ct).ConfigureAwait(false);
        if (!SettingsBlock.IsValid(block))
        {
            throw new K5Exception("the settings block at 0x1D00 is not valid (the firmware runs on defaults); store any setting from the radio's menu once to create it");
        }

        var (newBlock, newTiming) = SettingsBlock.Encode(block, timing, changes);
        for (int page = 0; page < SettingsBlock.Length; page += 8)
        {
            if (!newBlock.AsSpan(page, 8).SequenceEqual(block.AsSpan(page, 8)))
            {
                await WriteEepromAsync(SettingsBlock.Address + page, newBlock.AsMemory(page, 8), ct).ConfigureAwait(false);
            }
        }

        if (!newTiming.AsSpan().SequenceEqual(timing))
        {
            await WriteEepromAsync(SettingsBlock.TimingAddress, newTiming, ct).ConfigureAwait(false);
        }

        Audit(K5AuditKind.SettingsChange, $"v1 settings block {string.Join(", ", changes.SetIds)}", "ok", NoDetails);
        var applied = SettingsBlock.Decode(newBlock, newTiming, _options.DeviationLaw);
        var only = new RadioSettings();
        foreach (var id in changes.SetIds)
        {
            only = ParameterCodec.Merge(only, Pick(applied, id));
        }

        return new SetSettingsResult { Applied = only, AppliesAfterQuiet = TimeSpan.FromSeconds(1.5) };

        static RadioSettings Pick(RadioSettings s, RadioParameterId id)
        {
            var w = ParameterCodec.ToWire(s).FirstOrDefault(x => x.Id == id);
            return w.Id == id ? ParameterCodec.Apply(new RadioSettings(), id, w.Value, s.DeviationWide?.Law ?? DeviationLaw.AiocWithBenchEq) : new RadioSettings();
        }
    }
}
