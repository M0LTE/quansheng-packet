namespace M0LTE.Uvk5.Protocol;

/// <summary>Message ids on the wire. Internal: the public API is typed.</summary>
internal static class MessageIds
{
    // legacy firmware protocol
    public const ushort Hello = 0x0514;
    public const ushort HelloReply = 0x0515;
    public const ushort HelloAlt = 0x052F;
    public const ushort EepromRead = 0x051B;
    public const ushort EepromReadReply = 0x051C;
    public const ushort EepromWrite = 0x051D;
    public const ushort EepromWriteReply = 0x051E;
    public const ushort Rssi = 0x0527;
    public const ushort RssiReply = 0x0528;
    public const ushort Battery = 0x0529;
    public const ushort BatteryReply = 0x052A;
    public const ushort Reboot = 0x05DD;
    public const ushort RegRead = 0x0601;      // reply id is also 0x0601
    public const ushort RegWrite = 0x0602;     // no reply

    // bootloader
    public const ushort BootBeacon = 0x0518;
    public const ushort BootBeaconV5 = 0x057A;
    public const ushort BootVersion = 0x0530;
    public const ushort BootWrite = 0x0519;
    public const ushort BootWriteAck = 0x051A;

    // protocol v2
    public const ushort V2First = 0x5000;
    public const ushort V2Last = 0x50FF;
    public const ushort V2RequestLast = 0x507F;
    public const ushort ReplyOffset = 0x80;
    public const ushort EventFirst = 0x50C0;
    public const ushort EventLast = 0x50DF;

    public const ushort GetInfo = 0x5000;
    public const ushort GetStatus = 0x5001;
    public const ushort Subscribe = 0x5002;
    public const ushort TimeSync = 0x5003;
    public const ushort GetParams = 0x5004;
    public const ushort SetParams = 0x5005;
    public const ushort SaveParams = 0x5006;
    public const ushort LevelTone = 0x5007;
    public const ushort RegReadV2 = 0x5008;
    public const ushort RegWriteV2 = 0x5009;
    public const ushort RegOverride = 0x500A;
    public const ushort EventReplay = 0x500B;
    public const ushort GetCounters = 0x500C;

    /// <summary>Reserved for serial keying. This library never sends it.</summary>
    public const ushort ReservedSerialKey = 0x5020;

    public const uint Pkt2Magic = 0x32544B50;   // "PKT2"

    public static bool IsV2(ushort id) => id is >= V2First and <= V2Last;

    public static bool IsEvent(ushort id) => id is >= EventFirst and <= EventLast;
}
