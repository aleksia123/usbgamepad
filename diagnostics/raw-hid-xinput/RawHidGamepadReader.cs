// RawHidGamepadReader - high-rate, descriptor-driven reader for an
// XInput-compatible HID game-controller collection (the IG_00 node),
// bypassing XInputGetState's ~125 Hz host-side cache.
//
// This is the ReflexX "plan Step 2" core, staged in this repo because the
// ReflexX sources aren't available here. It is deliberately self-contained
// (HidSharp only, no ReflexX types): drop the file into
// ReflexX.Infrastructure/Input/ and write a thin IInputProvider adapter
// around it - see README.md next to this file.
//
// Design notes:
//  * All decode is driven by the device's declared report descriptor
//    (usages + logical min/max), not fixed byte offsets, so it adapts to
//    whatever layout the pad really declares and preserves native
//    resolution. LayoutDescription tells you what that layout/resolution
//    actually is.
//  * Axis orientation is left exactly as the pad declares it (HID Y is
//    typically down-positive; XInput's sThumbLY is up-positive). Invert in
//    the adapter if the consumer expects XInput orientation.
//  * Reconnect is handled: the worker loops enumerate -> open -> read until
//    Stop(), announcing transitions via Status.

using System.Diagnostics;
using HidSharp;
using HidSharp.Reports;
using HidSharp.Reports.Input;

namespace RawHidXInput;

[Flags]
public enum RawDpad : byte
{
    None = 0,
    Up = 1,
    Right = 2,
    Down = 4,
    Left = 8,
}

/// <summary>One decoded input report, normalized but lossless in range.</summary>
public struct RawGamepadState
{
    // Normalized to the full signed 16-bit range from the declared logical
    // range, orientation as declared by the device.
    public short LeftX, LeftY, RightX, RightY;

    // Normalized to 0..65535 from the declared logical range. NOTE: if the
    // collection declares only a single Z axis (classic combined-trigger
    // XInput HID mapping), both triggers ride on LeftTrigger and idle near
    // mid-scale - check LayoutDescription / the --decode harness.
    public ushort LeftTrigger, RightTrigger;

    /// <summary>Bit n = HID button usage n+1 (button 1 -> bit 0).</summary>
    public uint Buttons;

    public RawDpad Dpad;

    /// <summary>Stopwatch.GetTimestamp() taken when the report was parsed.</summary>
    public long TimestampTicks;

    public bool GetButton(int usageNumber) =>
        usageNumber is >= 1 and <= 32 && (Buttons & (1u << (usageNumber - 1))) != 0;
}

/// <summary>Snapshot of achieved report timing since the previous snapshot.</summary>
public sealed class RateStats
{
    public int Samples;
    public double MinMs, AvgMs, MaxMs, LastMs;
    public long TotalReports;

    public override string ToString() => Samples == 0
        ? "no reports"
        : $"{Samples} reports  min={MinMs:F3}  avg={AvgMs:F3}  max={MaxMs:F3} ms  (~{1000.0 / Math.Max(AvgMs, 1e-9):F0} Hz)";
}

public sealed class RawHidGamepadReader : IDisposable
{
    private readonly int _vendorId;
    private readonly int _productId;
    private readonly string _pathFilter;

    private Thread? _thread;
    private volatile bool _stop;
    private volatile bool _connected;
    private volatile string? _layout;

    private readonly object _stateLock = new();
    private RawGamepadState _lastState;

    private readonly object _statsLock = new();
    private int _cnt;
    private double _sum, _min = double.MaxValue, _max, _lastMs;
    private long _total;

    public RawHidGamepadReader(int vendorId = 0x3537, int productId = 0x10C5, string devicePathFilter = "ig_00")
    {
        _vendorId = vendorId;
        _productId = productId;
        _pathFilter = devicePathFilter;
    }

    /// <summary>Fires on the reader thread for every input report. Keep handlers fast.</summary>
    public event Action<RawGamepadState>? StateUpdated;

    /// <summary>Connect/disconnect/diagnostic messages, for a log panel.</summary>
    public event Action<string>? Status;

    public bool IsConnected => _connected;

    /// <summary>Human-readable summary of the declared layout (axes, bit depths, buttons); null until first connect.</summary>
    public string? LayoutDescription => _layout;

    public RawGamepadState LastState
    {
        get { lock (_stateLock) return _lastState; }
    }

    /// <summary>Returns timing of reports since the last call and resets the window (poll ~1/s for a readout).</summary>
    public RateStats GetRateStats()
    {
        lock (_statsLock)
        {
            var s = new RateStats
            {
                Samples = _cnt,
                MinMs = _cnt > 0 ? _min : 0,
                AvgMs = _cnt > 0 ? _sum / _cnt : 0,
                MaxMs = _cnt > 0 ? _max : 0,
                LastMs = _lastMs,
                TotalReports = _total,
            };
            _cnt = 0; _sum = 0; _min = double.MaxValue; _max = 0;
            return s;
        }
    }

    public void Start()
    {
        if (_thread is { IsAlive: true }) return;
        _stop = false;
        _thread = new Thread(Run)
        {
            IsBackground = true,
            Name = nameof(RawHidGamepadReader),
            Priority = ThreadPriority.AboveNormal,
        };
        _thread.Start();
    }

    public void Stop()
    {
        _stop = true;
        _thread?.Join(2000);
        _thread = null;
        _connected = false;
    }

    public void Dispose() => Stop();

    // ------------------------------------------------------------------ //

    private void Run()
    {
        while (!_stop)
        {
            HidDevice? device = FindDevice();
            if (device == null)
            {
                SleepStep(1000);
                continue;
            }

            try
            {
                RunDevice(device);
            }
            catch (Exception ex)
            {
                Announce($"device error: {ex.Message}");
            }

            if (_connected)
            {
                _connected = false;
                Announce("disconnected");
            }
            if (!_stop) SleepStep(1000);
        }
        _connected = false;
    }

    private HidDevice? FindDevice()
    {
        List<HidDevice> matches;
        try
        {
            matches = DeviceList.Local.GetHidDevices(_vendorId, _productId)
                .Where(d => _pathFilter.Length == 0 ||
                            d.DevicePath.Contains(_pathFilter, StringComparison.OrdinalIgnoreCase))
                .ToList();
        }
        catch (Exception ex)
        {
            Announce($"enumeration failed: {ex.Message}");
            return null;
        }

        if (matches.Count > 1)
            Announce($"{matches.Count} nodes match {_vendorId:X4}:{_productId:X4} \"{_pathFilter}\"; using the first");
        return matches.FirstOrDefault();
    }

    private void RunDevice(HidDevice device)
    {
        ReportDescriptor reportDescriptor = device.GetReportDescriptor();
        DeviceItem? deviceItem = SelectDeviceItem(reportDescriptor);
        if (deviceItem == null)
        {
            Announce("no input-capable device item in the report descriptor");
            SleepStep(2000);
            return;
        }

        _layout = DescribeLayout(deviceItem);

        if (!device.TryOpen(out HidStream? stream))
        {
            Announce("open failed (held exclusively by another app?)");
            SleepStep(2000);
            return;
        }

        using (stream)
        {
            HidDeviceInputReceiver receiver = reportDescriptor.CreateHidDeviceInputReceiver();
            DeviceItemInputParser parser = deviceItem.CreateDeviceItemInputParser();
            receiver.Start(stream);

            _connected = true;
            Announce($"connected: {device.DevicePath}");
            Announce($"layout: {_layout}");

            var buf = new byte[Math.Max(8, device.GetMaxInputReportLength())];
            long prevT = 0;
            bool havePrev = false;

            while (!_stop && receiver.IsRunning)
            {
                if (!receiver.WaitHandle.WaitOne(250)) continue;

                while (receiver.TryRead(buf, 0, out Report? report))
                {
                    if (!parser.TryParseReport(buf, 0, report)) continue;

                    long t = Stopwatch.GetTimestamp();
                    RawGamepadState state = BuildState(parser, t);

                    if (havePrev)
                    {
                        double ms = (t - prevT) * 1000.0 / Stopwatch.Frequency;
                        if (ms <= 250) // longer = input pause, not a polling interval
                        {
                            lock (_statsLock)
                            {
                                _cnt++;
                                _sum += ms;
                                if (ms < _min) _min = ms;
                                if (ms > _max) _max = ms;
                                _lastMs = ms;
                                _total++;
                            }
                        }
                    }
                    prevT = t;
                    havePrev = true;

                    lock (_stateLock) _lastState = state;
                    StateUpdated?.Invoke(state);
                }
            }
        }
    }

    private static DeviceItem? SelectDeviceItem(ReportDescriptor reportDescriptor)
    {
        DeviceItem? best = null;
        int bestScore = -1;
        foreach (DeviceItem item in reportDescriptor.DeviceItems)
        {
            if (!item.Reports.Any(r => r.ReportType == ReportType.Input)) continue;

            var usages = item.Usages.GetAllValues().ToList();
            int score = usages.Contains(0x00010005u) ? 3   // GenericDesktop / Gamepad
                      : usages.Contains(0x00010004u) ? 2   // GenericDesktop / Joystick
                      : 0;
            if (score > bestScore) { best = item; bestScore = score; }
        }
        return best;
    }

    private static RawGamepadState BuildState(DeviceItemInputParser parser, long timestamp)
    {
        var s = new RawGamepadState { TimestampTicks = timestamp };

        int count = parser.ValueCount;
        for (int i = 0; i < count; i++)
        {
            DataValue dv = parser.GetValue(i);
            uint usage = dv.Usages.FirstOrDefault();
            int v = dv.GetLogicalValue();
            DataItem di = dv.DataItem;
            uint page = usage >> 16, id = usage & 0xFFFF;

            if (page == 0x0009) // Button page
            {
                if (id is >= 1 and <= 32 && v != 0)
                    s.Buttons |= 1u << ((int)id - 1);
                continue;
            }

            if (page == 0x0001) // Generic Desktop
            {
                switch (id)
                {
                    case 0x30: s.LeftX = NormalizeSigned(v, di); break;   // X
                    case 0x31: s.LeftY = NormalizeSigned(v, di); break;   // Y
                    case 0x33: s.RightX = NormalizeSigned(v, di); break;  // Rx
                    case 0x34: s.RightY = NormalizeSigned(v, di); break;  // Ry
                    case 0x32: s.LeftTrigger = NormalizeUnsigned(v, di); break;  // Z  (may be combined triggers)
                    case 0x35: s.RightTrigger = NormalizeUnsigned(v, di); break; // Rz
                    case 0x39: s.Dpad = DecodeHat(v, di); break;          // Hat switch
                }
            }
            else if (page == 0x0002) // Simulation Controls: some pads put triggers here
            {
                switch (id)
                {
                    case 0xC5: s.LeftTrigger = NormalizeUnsigned(v, di); break;  // Brake
                    case 0xC4: s.RightTrigger = NormalizeUnsigned(v, di); break; // Accelerator
                }
            }
        }
        return s;
    }

    private static short NormalizeSigned(int v, DataItem di)
    {
        long range = (long)di.LogicalMaximum - di.LogicalMinimum;
        if (range <= 0) return 0;
        long scaled = (v - (long)di.LogicalMinimum) * 65535 / range - 32768;
        return (short)Math.Clamp(scaled, short.MinValue, short.MaxValue);
    }

    private static ushort NormalizeUnsigned(int v, DataItem di)
    {
        long range = (long)di.LogicalMaximum - di.LogicalMinimum;
        if (range <= 0) return 0;
        long scaled = (v - (long)di.LogicalMinimum) * 65535 / range;
        return (ushort)Math.Clamp(scaled, ushort.MinValue, ushort.MaxValue);
    }

    private static readonly RawDpad[] s_hat8 =
    {
        RawDpad.Up, RawDpad.Up | RawDpad.Right, RawDpad.Right, RawDpad.Down | RawDpad.Right,
        RawDpad.Down, RawDpad.Down | RawDpad.Left, RawDpad.Left, RawDpad.Up | RawDpad.Left,
    };

    private static RawDpad DecodeHat(int v, DataItem di)
    {
        if (v < di.LogicalMinimum || v > di.LogicalMaximum) return RawDpad.None; // null state
        int positions = di.LogicalMaximum - di.LogicalMinimum + 1;
        if (positions <= 0) return RawDpad.None;
        int idx = (v - di.LogicalMinimum) * 8 / positions; // 8-way hats hit this 1:1
        return s_hat8[idx & 7];
    }

    private static string DescribeLayout(DeviceItem deviceItem)
    {
        var parts = new List<string>();
        int buttons = 0;
        foreach (Report report in deviceItem.Reports.Where(r => r.ReportType == ReportType.Input))
        {
            foreach (DataItem di in report.DataItems)
            {
                foreach (uint usage in di.Usages.GetAllValues())
                {
                    uint page = usage >> 16, id = usage & 0xFFFF;
                    if (page == 0x0009) { buttons++; continue; }

                    long range = (long)di.LogicalMaximum - di.LogicalMinimum;
                    int bits = range > 0 ? (int)Math.Ceiling(Math.Log2(range + 1)) : 0;
                    string name = (page, id) switch
                    {
                        (0x0001, 0x30) => "X",
                        (0x0001, 0x31) => "Y",
                        (0x0001, 0x32) => "Z",
                        (0x0001, 0x33) => "Rx",
                        (0x0001, 0x34) => "Ry",
                        (0x0001, 0x35) => "Rz",
                        (0x0001, 0x39) => "Hat",
                        (0x0002, 0xC4) => "Accelerator",
                        (0x0002, 0xC5) => "Brake",
                        _ => $"0x{page:X2}:{id:X2}",
                    };
                    parts.Add($"{name}[{di.LogicalMinimum}..{di.LogicalMaximum}]={bits}bit");
                }
            }
        }
        if (buttons > 0) parts.Add($"{buttons} buttons");
        return string.Join("  ", parts);
    }

    private void SleepStep(int totalMs)
    {
        for (int waited = 0; waited < totalMs && !_stop; waited += 100)
            Thread.Sleep(100);
    }

    private void Announce(string message)
    {
        try { Status?.Invoke(message); }
        catch { /* never let a log sink kill the reader thread */ }
    }
}
