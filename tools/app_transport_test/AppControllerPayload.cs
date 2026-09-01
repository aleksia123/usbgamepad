using System.Runtime.InteropServices;

namespace AppTransportTest;

/// <summary>
/// Mirrors app_controller_payload_t from src/app_transport.h exactly:
/// packed, little-endian, 12 bytes. Report ID is NOT part of this struct -
/// it is prepended separately when reading/writing HID reports.
/// </summary>
[StructLayout(LayoutKind.Sequential, Pack = 1)]
public struct AppControllerPayload
{
    public const int Size = 12;

    public ushort Buttons;
    public byte LeftTrigger;
    public byte RightTrigger;
    public short ThumbLX;
    public short ThumbLY;
    public short ThumbRX;
    public short ThumbRY;

    // Bit values match XINPUT_GAMEPAD_* in src/xinput_host.h.
    public const ushort DPAD_UP = 0x0001;
    public const ushort DPAD_DOWN = 0x0002;
    public const ushort DPAD_LEFT = 0x0004;
    public const ushort DPAD_RIGHT = 0x0008;
    public const ushort START = 0x0010;
    public const ushort BACK = 0x0020;
    public const ushort LEFT_THUMB = 0x0040;
    public const ushort RIGHT_THUMB = 0x0080;
    public const ushort LEFT_SHOULDER = 0x0100;
    public const ushort RIGHT_SHOULDER = 0x0200;
    public const ushort GUIDE = 0x0400;
    public const ushort SHARE = 0x0800;
    public const ushort A = 0x1000;
    public const ushort B = 0x2000;
    public const ushort X = 0x4000;
    public const ushort Y = 0x8000;

    public byte[] ToBytes()
    {
        var bytes = new byte[Size];
        int handleSize = Marshal.SizeOf<AppControllerPayload>();
        IntPtr ptr = Marshal.AllocHGlobal(handleSize);
        try
        {
            Marshal.StructureToPtr(this, ptr, false);
            Marshal.Copy(ptr, bytes, 0, Size);
        }
        finally
        {
            Marshal.FreeHGlobal(ptr);
        }
        return bytes;
    }

    public static AppControllerPayload FromBytes(ReadOnlySpan<byte> bytes)
    {
        IntPtr ptr = Marshal.AllocHGlobal(Size);
        try
        {
            Marshal.Copy(bytes.ToArray(), 0, ptr, Size);
            return Marshal.PtrToStructure<AppControllerPayload>(ptr);
        }
        finally
        {
            Marshal.FreeHGlobal(ptr);
        }
    }

    public override string ToString() =>
        $"buttons=0x{Buttons:X4} lt={LeftTrigger,3} rt={RightTrigger,3} " +
        $"lx={ThumbLX,6} ly={ThumbLY,6} rx={ThumbRX,6} ry={ThumbRY,6}";
}
