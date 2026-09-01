using System.Runtime.InteropServices;

namespace DeadzoneTuner;

public enum DeadzoneShape
{
    Radial = 0,
    Axial = 1,
}

[StructLayout(LayoutKind.Sequential)]
public struct AxisConfig
{
    public int Deadzone;
    public int AntiDeadzone;
    public int MaxZone;
}

// P/Invoke over deadzone_native.dll (tools/deadzone_tuner/native), which
// wraps the exact same math as src/deadzone.h - see deadzone.hpp for the
// C++ port and native/deadzone_api.cpp for the exported C ABI.
public static class NativeDeadzone
{
    [DllImport("deadzone_native.dll", CallingConvention = CallingConvention.Cdecl)]
    private static extern void deadzone_apply_stick_native(
        ref short px, ref short py, int type,
        ref AxisConfig radial, ref AxisConfig xAxis, ref AxisConfig yAxis);

    public static (short x, short y) Apply(
        short x, short y, DeadzoneShape shape,
        AxisConfig radial, AxisConfig xAxis, AxisConfig yAxis)
    {
        deadzone_apply_stick_native(ref x, ref y, (int)shape, ref radial, ref xAxis, ref yAxis);
        return (x, y);
    }
}
