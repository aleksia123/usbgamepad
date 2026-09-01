using System.Runtime.InteropServices;

namespace XInputTest;

/// <summary>
/// Polls Windows' native XInput API (not our custom HID app-transport
/// interface) to confirm the RP2350's XInput device interface (interface 0)
/// is actually bound and working. XInput devices are deliberately excluded
/// from the legacy joystick panel (joy.cpl) - this is the correct way to
/// check them, no game required.
/// </summary>
internal static class Program
{
    private const int ButtonCount = 14;

    [StructLayout(LayoutKind.Sequential)]
    private struct XInputGamepad
    {
        public ushort wButtons;
        public byte bLeftTrigger;
        public byte bRightTrigger;
        public short sThumbLX;
        public short sThumbLY;
        public short sThumbRX;
        public short sThumbRY;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct XInputState
    {
        public uint dwPacketNumber;
        public XInputGamepad Gamepad;
    }

    [DllImport("xinput1_4.dll", EntryPoint = "XInputGetState")]
    private static extern int XInputGetState14(uint dwUserIndex, out XInputState pState);

    [DllImport("xinput9_1_0.dll", EntryPoint = "XInputGetState")]
    private static extern int XInputGetState910(uint dwUserIndex, out XInputState pState);

    private const int ERROR_SUCCESS = 0;

    private static readonly (ushort mask, string name)[] Buttons =
    {
        (0x0001, "UP"), (0x0002, "DOWN"), (0x0004, "LEFT"), (0x0008, "RIGHT"),
        (0x0010, "START"), (0x0020, "BACK"), (0x0040, "L3"), (0x0080, "R3"),
        (0x0100, "LB"), (0x0200, "RB"), (0x1000, "A"), (0x2000, "B"),
        (0x4000, "X"), (0x8000, "Y"),
    };

    private static int GetState(uint index, out XInputState state)
    {
        try
        {
            return XInputGetState14(index, out state);
        }
        catch (DllNotFoundException)
        {
            return XInputGetState910(index, out state);
        }
    }

    private static int Main()
    {
        Console.WriteLine("Polling XInput slots 0-3 (Ctrl+C to exit)...");
        Console.WriteLine("A connected RP2350 running the composite firmware should show up as one of these.");
        Console.WriteLine();

        bool[] wasConnected = new bool[4];

        while (true)
        {
            for (uint i = 0; i < 4; i++)
            {
                int result = GetState(i, out XInputState state);
                bool connected = result == ERROR_SUCCESS;

                if (connected && !wasConnected[i])
                    Console.WriteLine($"[slot {i}] connected");
                else if (!connected && wasConnected[i])
                    Console.WriteLine($"[slot {i}] disconnected");

                wasConnected[i] = connected;

                if (connected)
                {
                    var g = state.Gamepad;
                    string pressed = string.Join(' ', Buttons.Where(b => (g.wButtons & b.mask) != 0).Select(b => b.name));
                    Console.Write($"\r[slot {i}] pkt={state.dwPacketNumber,6} " +
                                  $"lt={g.bLeftTrigger,3} rt={g.bRightTrigger,3} " +
                                  $"lx={g.sThumbLX,6} ly={g.sThumbLY,6} rx={g.sThumbRX,6} ry={g.sThumbRY,6} " +
                                  $"[{pressed}]".PadRight(40));
                }
            }

            Thread.Sleep(16);
        }
    }
}
