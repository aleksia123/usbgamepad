// *** DO NOT BUILD THIS FILE IN THIS REPO - IT WILL NOT COMPILE HERE ***
// It needs ReflexX's types (IInputProvider, GamepadState, InputDevice,
// GamepadButton, ILogger<>), so it compiles only inside the ReflexX
// solution. To use it: COPY this file into the ReflexX project folder that
// holds XinputAppTransport.cs and change the `namespace` line below to that
// file's namespace. Never paste it into/over RawHidGamepadReader.cs - that
// is a different, standalone class which the hid-rate-probe project
// compiles for --decode; replacing it breaks the probe build.
//
// RawHidXInputProvider - IInputProvider that reads the physical pad's
// XInput-compatible HID collection (HID\VID_3537&PID_10C5&IG_00) directly,
// bypassing XInputGetState's ~125 Hz host-side cache (plan Step 2).
//
// Drop-in for ReflexX.Infrastructure/Input/. Written in the same lifecycle
// idiom as XinputAppTransport/Rp2350AppTransport (TryOpenStream, read loop,
// teardown-after-failure, capped-backoff reconnect) but INPUT-ONLY: the
// physical pad's IG_00 accepts no app output reports, so there is no
// IOutputController half here - processed state still goes out through the
// existing transport.
//
// Unlike the app transports this cannot use fixed report offsets: IG_00's
// report layout is whatever the descriptor declares (confirmed on this pad:
// X/Y/Rx/Ry 16-bit, ONE combined-trigger Z, 8-way hat, 10 buttons - and
// Windows returns degenerate 0..0 logical ranges for the axes, the signed
// 0xFFFF quirk). Decode therefore runs through HidSharp's report parser,
// driven by declared usages and ranges with a bit-width fallback.
//
// Decode uses the report layout verified ON THE WIRE with the probe's
// --map mode (see the table at DecodeVerified): every stick rail, both
// triggers, hat values and button A were observed directly, and the earlier
// DECODE SUMMARY confirmed the button press order 1..10 = A B X Y LB RB
// Back Start L3 R3. LT was verified as the HIGH side of the combined
// trigger. The descriptor-driven parser remains as fallback for reports
// that don't match the verified shape.
//
// Usings and namespaces match the real tree (ReflexX.Domain.*), taken from
// Rp2350AppTransport. Remaining in-tree fixes should be nil or cosmetic:
//   * TODO: GamepadButton member NAMES in ButtonMap/s_hat8 (order is
//     confirmed; only rename members if the enum spells them differently).
//   * If IInputProvider has members beyond these, stub them like
//     ExcludeXInputSlots below.

using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Threading;
using System.Threading.Tasks;
using HidSharp;
using HidSharp.Reports;
using HidSharp.Reports.Input;
using Microsoft.Extensions.Logging;
using ReflexX.Domain;
using ReflexX.Domain.Entities;
using ReflexX.Domain.Enums;
using ReflexX.Domain.Interfaces;
using ReflexX.Domain.ValueObjects;

namespace ReflexX.Infrastructure.Input;

public sealed class RawHidXInputProvider : IInputProvider, IDisposable
{
    public const ushort VendorId = 0x3537;   // physical pad (BIGBIG WON / GameSir family)
    public const ushort ProductId = 0x10C5;
    public const string DevicePathMustContain = "ig_00"; // the XInput-compatible HID collection
    public const string DeviceId = "raw_hid_xinput_pad";

    private readonly ILogger<RawHidXInputProvider> _logger;
    private readonly object _lifecycleLock = new();
    private HidStream? _stream;
    private DeviceItemInputParser? _parser;
    private IReadOnlyList<Report>? _inputReports;
    private byte[] _readBuffer = new byte[64];
    private CancellationTokenSource? _readCts;
    private Task? _readTask;
    private bool _connected;
    private InputDevice? _device;
    private volatile string? _layout;

    private readonly object _statsLock = new();
    private long _prevReportTicks;
    private int _statCount;
    private double _statSumMs, _statMinMs = double.MaxValue, _statMaxMs;
    private readonly List<double> _statSamples = new();

    public event Action<InputDevice>? DeviceConnected;
    public event Action<InputDevice>? DeviceDisconnected;
    public event Action<string, GamepadState>? StateUpdated;

    public bool IsConnected => _connected && _stream is { CanRead: true };
    public int? VirtualXInputSlot => null;

    /// <summary>Declared axis ranges/bit depths of the connected collection; null until first connect.</summary>
    public string? LayoutDescription => _layout;

    public RawHidXInputProvider(ILogger<RawHidXInputProvider> logger) => _logger = logger;

    public void Connect()
    {
        lock (_lifecycleLock)
        {
            if (IsConnected) return;
            if (!TryOpenStream(out var error))
                throw new InvalidOperationException(error);
        }
    }

    /// <summary>Finds and opens the pad's IG_00 HID collection and builds the
    /// descriptor-driven parser for it. Shared by <see cref="Connect"/> (throws
    /// on failure) and the read loop's reconnect path (returns false - the pad
    /// being unplugged mid-session is routine, not exceptional).</summary>
    private bool TryOpenStream(out string error)
    {
        error = string.Empty;
        var candidates = DeviceList.Local.GetHidDevices(VendorId, ProductId).ToList();

        // Path filter is essential: the pad is a composite and its vendor
        // collections (MI_01&COL0x) also match by VID/PID. Report lengths do
        // NOT identify the right node here - IG_00 has no output report at
        // all, and the vendor collections can have larger reports.
        var device = candidates.FirstOrDefault(candidate =>
            candidate.DevicePath.Contains(DevicePathMustContain, StringComparison.OrdinalIgnoreCase));

        if (device is null)
        {
            foreach (var candidate in candidates)
                _logger.LogInformation("Raw HID candidate (no '{Filter}' in path): {Path}",
                    DevicePathMustContain, candidate.DevicePath);
            error = $"Raw HID interface {VendorId:X4}:{ProductId:X4} ({DevicePathMustContain}) was not found. " +
                    "Is the pad plugged in directly and in XInput mode?";
            return false;
        }

        ReportDescriptor reportDescriptor;
        DeviceItem? deviceItem;
        try
        {
            reportDescriptor = device.GetReportDescriptor();
            deviceItem = SelectDeviceItem(reportDescriptor);
        }
        catch (Exception ex)
        {
            error = $"Raw HID descriptor unreadable: {ex.Message}";
            return false;
        }
        if (deviceItem is null)
        {
            error = "Raw HID collection declares no input items.";
            return false;
        }

        if (!device.TryOpen(out var stream))
        {
            error = "Raw HID open failed (held exclusively by another app?).";
            return false;
        }

        stream.ReadTimeout = 250;
        _stream = stream;
        _parser = deviceItem.CreateDeviceItemInputParser();
        _inputReports = deviceItem.Reports.Where(r => r.ReportType == ReportType.Input).ToList();
        _readBuffer = new byte[Math.Max(8, SafeInputReportLength(device))];
        _layout = DescribeLayout(deviceItem);
        _connected = true;
        lock (_statsLock) _prevReportTicks = 0;
        _device = new InputDevice
        {
            Id = DeviceId,
            Name = "Raw HID XInput Pad",
            Type = DeviceType.XInput, // or a more specific member if one exists
            PlayerIndex = 0,
            IsConnected = true,
            LastSeen = DateTime.UtcNow,
            VendorId = VendorId,
            ProductId = ProductId,
            DevicePath = device.DevicePath
        };

        _logger.LogInformation("Raw HID pad connected ({VendorId:X4}:{ProductId:X4}); layout: {Layout}",
            VendorId, ProductId, _layout);
        return true;
    }

    private static int SafeInputReportLength(HidDevice device)
    {
        try { return device.GetMaxInputReportLength(); }
        catch { return 64; }
    }

    public async Task StartAsync(CancellationToken ct)
    {
        Connect();
        lock (_lifecycleLock)
        {
            if (_readTask is { IsCompleted: false }) return;
            _readCts = CancellationTokenSource.CreateLinkedTokenSource(ct);
            var token = _readCts.Token;
            _readTask = Task.Run(() => ReadLoop(token), CancellationToken.None);
            if (_device is not null) DeviceConnected?.Invoke(_device);
        }
        await Task.CompletedTask;
    }

    public async Task StopAsync()
    {
        Task? readTask;
        lock (_lifecycleLock)
        {
            _readCts?.Cancel();
            readTask = _readTask;
        }

        if (readTask is not null)
        {
            try { await readTask.ConfigureAwait(false); }
            catch (OperationCanceledException) { }
        }

        lock (_lifecycleLock)
        {
            _readTask = null;
            _readCts?.Dispose();
            _readCts = null;
        }
    }

    public void Disconnect()
    {
        StopAsync().GetAwaiter().GetResult();
        lock (_lifecycleLock)
        {
            var device = _device;
            _device = null;
            _connected = false;
            _stream?.Dispose();
            _stream = null;
            _parser = null;
            _inputReports = null;
            if (device is not null)
            {
                device.IsConnected = false;
                DeviceDisconnected?.Invoke(device);
            }
            _logger.LogInformation("Raw HID pad disconnected");
        }
    }

    private void ReadLoop(CancellationToken ct)
    {
        while (!ct.IsCancellationRequested)
        {
            if (!IsConnected)
            {
                if (!TryReconnect(ct)) return;
                continue;
            }

            try
            {
                var stream = _stream;
                var parser = _parser;
                if (stream is null || parser is null) continue;

                var buffer = _readBuffer;
                var read = stream.Read(buffer, 0, buffer.Length);
                if (read <= 0) continue;

                GamepadState state;
                if (UseVerifiedFixedLayout && read >= 13 && buffer[0] == 0x00)
                {
                    state = DecodeVerified(buffer);
                }
                else
                {
                    var report = MatchReport(buffer);
                    if (report is null || !parser.TryParseReport(buffer, 0, report)) continue;
                    state = DecodeCurrent(parser);
                }

                RecordInterval(Stopwatch.GetTimestamp());
                if (_device is not null) _device.LastSeen = DateTime.UtcNow;
                StateUpdated?.Invoke(DeviceId, state);
            }
            catch (TimeoutException) { } // pad idle: HID only reports on change
            catch (Exception ex) when (ex is IOException or InvalidOperationException or ObjectDisposedException)
            {
                if (ct.IsCancellationRequested) return;
                _logger.LogWarning(ex, "Raw HID pad input stopped - reconnecting");
                TearDownAfterReadFailure();
            }
        }
    }

    private Report? MatchReport(byte[] buffer)
    {
        var reports = _inputReports;
        if (reports is null || reports.Count == 0) return null;
        if (reports.Count == 1) return reports[0];
        foreach (var report in reports)
            if (report.ReportID == buffer[0]) return report;
        return null;
    }

    /// <summary>Drops the dead stream and announces the device as gone so the UI
    /// reflects reality while <see cref="TryReconnect"/> retries. Does not touch
    /// <see cref="_readCts"/>/<see cref="_readTask"/> - the read loop calling
    /// this is still running and keeps ownership of them.</summary>
    private void TearDownAfterReadFailure()
    {
        InputDevice? lost;
        lock (_lifecycleLock)
        {
            lost = _device;
            _connected = false;
            _stream?.Dispose();
            _stream = null;
        }
        if (lost is not null)
        {
            lost.IsConnected = false;
            DeviceDisconnected?.Invoke(lost);
        }
    }

    internal const int InitialReconnectDelayMs = 250;
    internal const int MaxReconnectDelayMs = 5000;

    internal static int NextReconnectDelayMs(int currentDelayMs) =>
        Math.Min(currentDelayMs * 2, MaxReconnectDelayMs);

    private bool TryReconnect(CancellationToken ct)
    {
        var delayMs = InitialReconnectDelayMs;
        while (!ct.IsCancellationRequested)
        {
            bool opened;
            lock (_lifecycleLock) { opened = TryOpenStream(out _); }
            if (opened)
            {
                if (_device is not null) DeviceConnected?.Invoke(_device);
                return true;
            }

            try { Task.Delay(delayMs, ct).Wait(ct); }
            catch (OperationCanceledException) { return false; }
            delayMs = NextReconnectDelayMs(delayMs);
        }
        return false;
    }

    public IReadOnlyList<InputDevice> GetConnectedDevices() =>
        _device is { IsConnected: true } device ? [device] : [];

    public void ExcludeXInputSlots(int[] slots) { }

    // The raw IG_00 collection carries no output reports, so rumble cannot be
    // driven down this path; no-op (XInputSetState could, if ever wanted).
    public void PulseRumble(bool strong) { }

    public Task RestartAsync(CancellationToken ct = default) => RestartCoreAsync(ct);

    private async Task RestartCoreAsync(CancellationToken ct)
    {
        await StopAsync().ConfigureAwait(false);
        Disconnect();
        Connect();
        await StartAsync(ct).ConfigureAwait(false);
    }

    /// <summary>The achieved-rate readout: logs report timing since the last
    /// call (plan Step 3 - surface via the Logs panel or poll ~1/s).
    /// HID reports are delivered ON CHANGE only, so idle input stretches the
    /// average; the median ("sustained") is the endpoint's cadence while
    /// input is moving and is the number to compare against 1000 Hz.</summary>
    public void LogDiagnostics(string label)
    {
        int count;
        double min, med, avg, max;
        lock (_statsLock)
        {
            count = _statCount;
            min = count > 0 ? _statMinMs : 0;
            med = MedianOf(_statSamples);
            avg = count > 0 ? _statSumMs / count : 0;
            max = count > 0 ? _statMaxMs : 0;
            _statCount = 0; _statSumMs = 0; _statMinMs = double.MaxValue; _statMaxMs = 0;
            _statSamples.Clear();
        }
        _logger.LogInformation(
            "Raw HID pad ({Label}): {Status}; {Count} reports, min={Min:F3} med={Med:F3} avg={Avg:F3} max={Max:F3} ms (~{Hz:F0} Hz sustained; avg includes input pauses)",
            label, IsConnected ? "connected" : "disconnected", count, min, med, avg, max,
            med > 0 ? 1000.0 / med : 0);
    }

    private static double MedianOf(List<double> values)
    {
        if (values.Count == 0) return 0;
        var sorted = values.OrderBy(v => v).ToList();
        return sorted[sorted.Count / 2];
    }

    public void Dispose() => Disconnect();

    private void RecordInterval(long nowTicks)
    {
        lock (_statsLock)
        {
            if (_prevReportTicks != 0)
            {
                double ms = (nowTicks - _prevReportTicks) * 1000.0 / Stopwatch.Frequency;
                if (ms <= 250) // longer = input pause, not a polling interval
                {
                    _statCount++;
                    _statSumMs += ms;
                    if (ms < _statMinMs) _statMinMs = ms;
                    if (ms > _statMaxMs) _statMaxMs = ms;
                    if (_statSamples.Count < 8192) _statSamples.Add(ms);
                }
            }
            _prevReportTicks = nowTicks;
        }
    }

    // ------------------------------------------------------------------ //
    //  Verified fixed-layout decode (primary path for this pad)
    //
    //  Report layout of the IG_00 collection, mapped on the wire with the
    //  probe's --map mode (every rail/press observed directly):
    //    b00      report id (0x00)
    //    b01-b02  LX  u16 LE   0 = left, 65535 = right
    //    b03-b04  LY  u16 LE   0 = up,   65535 = down (HID orientation)
    //    b05-b06  RX  u16 LE   0 = left, 65535 = right
    //    b07-b08  RY  u16 LE   0 = up,   65535 = down
    //    b09-b10  combined trigger u16 LE: 32768 rest, ->65535 LT, ->0 RT
    //    b11      buttons 1-8 (A B X Y LB RB Back Start = bits 0-7)
    //    b12      bits 0-1 = buttons 9-10 (L3 R3); bits 2-5 = hat 0=idle 1=N..8=NW
    //    b13-b14  unused
    // ------------------------------------------------------------------ //

    private const bool UseVerifiedFixedLayout = true;

    private static GamepadState DecodeVerified(byte[] b)
    {
        static ushort U16(byte[] r, int o) => (ushort)(r[o] | (r[o + 1] << 8));

        GamepadButton buttons = default;
        int bits = b[11] | ((b[12] & 0x03) << 8);
        for (int i = 0; i < ButtonMap.Length; i++)
            if ((bits & (1 << i)) != 0) buttons |= ButtonMap[i];
        int hat = (b[12] >> 2) & 0x0F;
        if (hat is >= 1 and <= 8) buttons |= s_hat8[hat - 1];

        // Combined trigger, LT = high side (verified). Both triggers idle at
        // 0; both-held cancels toward zero - inherent to this collection.
        int delta = U16(b, 9) - 32768;

        return new GamepadState
        {
            Buttons = buttons,
            LeftTrigger = new TriggerValue((byte)(Math.Clamp(delta, 0, 32767) * 255 / 32767)),
            RightTrigger = new TriggerValue((byte)(Math.Clamp(-delta, 0, 32768) * 255 / 32768)),
            LeftStick = new StickPosition((short)(U16(b, 1) - 32768), FlipY(U16(b, 3))),
            RightStick = new StickPosition((short)(U16(b, 5) - 32768), FlipY(U16(b, 7))),
            TimestampTicks = (long)MonotonicClock.NowMs
        };
    }

    // HID: 0 = up. XInput: positive = up.
    private static short FlipY(int raw) => (short)Math.Clamp(32768 - raw, short.MinValue, short.MaxValue);

    // ------------------------------------------------------------------ //
    //  Descriptor-driven decode -> GamepadState (fallback path)
    // ------------------------------------------------------------------ //

    // TODO(map): HID button usage N (1-based) -> GamepadButton flag, in usage
    // order. Standard XInput-style ordering below; confirm with the probe's
    // DECODE SUMMARY press-order and rename members to the real enum.
    private static readonly GamepadButton[] ButtonMap =
    {
        GamepadButton.A, GamepadButton.B, GamepadButton.X, GamepadButton.Y,
        GamepadButton.LeftShoulder, GamepadButton.RightShoulder,
        GamepadButton.Back, GamepadButton.Start,
        GamepadButton.LeftThumb, GamepadButton.RightThumb,
    };

    // Combined-trigger Z (confirmed layout: lone Z, no Rz/Brake/Accelerator):
    // idles mid-scale, one trigger drives it up, the other down. Flip this if
    // testing shows LT/RT swapped. Both-held partially cancels - inherent to
    // this collection; if that matters, source triggers from XInputGetState
    // instead (125 Hz is plenty for triggers) and keep everything else raw.
    private const bool CombinedZLeftIsHigh = true;

    private static GamepadState DecodeCurrent(DeviceItemInputParser parser)
    {
        short lx = 0, lyHid = 0, rx = 0, ryHid = 0;
        int z = -1, rz = -1, brake = -1, accel = -1;
        GamepadButton buttons = default;

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
                if (v != 0 && id >= 1 && id <= ButtonMap.Length)
                    buttons |= ButtonMap[id - 1];
                continue;
            }

            if (page == 0x0001) // Generic Desktop
            {
                switch (id)
                {
                    case 0x30: lx = NormalizeSigned(v, di); break;      // X
                    case 0x31: lyHid = NormalizeSigned(v, di); break;   // Y
                    case 0x33: rx = NormalizeSigned(v, di); break;      // Rx
                    case 0x34: ryHid = NormalizeSigned(v, di); break;   // Ry
                    case 0x32: z = NormalizeUnsigned(v, di); break;     // Z (combined triggers here)
                    case 0x35: rz = NormalizeUnsigned(v, di); break;    // Rz (absent on this pad)
                    case 0x39: buttons |= DecodeHat(v, di); break;      // Hat -> dpad buttons
                }
            }
            else if (page == 0x0002) // Simulation Controls (absent on this pad; handled for generality)
            {
                switch (id)
                {
                    case 0xC5: brake = NormalizeUnsigned(v, di); break;
                    case 0xC4: accel = NormalizeUnsigned(v, di); break;
                }
            }
        }

        byte lt, rt;
        if (brake >= 0 || accel >= 0 || rz >= 0)
        {
            // Separate triggers declared: Brake/Z drive LT, Accelerator/Rz drive RT.
            lt = To255(brake >= 0 ? brake : Math.Max(z, 0));
            rt = To255(accel >= 0 ? accel : Math.Max(rz, 0));
        }
        else if (z >= 0)
        {
            SplitCombinedZ(z, out lt, out rt);
        }
        else
        {
            lt = 0; rt = 0;
        }

        return new GamepadState
        {
            Buttons = buttons,
            LeftTrigger = new TriggerValue(lt),
            RightTrigger = new TriggerValue(rt),
            // HID Y is down-positive; XInput convention is up-positive.
            LeftStick = new StickPosition(lx, InvertY(lyHid)),
            RightStick = new StickPosition(rx, InvertY(ryHid)),
            TimestampTicks = (long)MonotonicClock.NowMs
        };
    }

    private static void SplitCombinedZ(int z65535, out byte lt, out byte rt)
    {
        int delta = z65535 - 32768; // rest is mid-scale
        int high = Math.Clamp(delta, 0, 32767) * 255 / 32767;
        int low = Math.Clamp(-delta, 0, 32768) * 255 / 32768;
        lt = (byte)(CombinedZLeftIsHigh ? high : low);
        rt = (byte)(CombinedZLeftIsHigh ? low : high);
    }

    private static byte To255(int v65535) => (byte)Math.Clamp(v65535 * 255 / 65535, 0, 255);

    private static short InvertY(short v) => v == short.MinValue ? short.MaxValue : (short)-v;

    private static DeviceItem? SelectDeviceItem(ReportDescriptor reportDescriptor)
    {
        DeviceItem? best = null;
        int bestScore = -1;
        foreach (DeviceItem item in reportDescriptor.DeviceItems)
        {
            if (!item.Reports.Any(r => r.ReportType == ReportType.Input)) continue;
            var usages = item.Usages.GetAllValues().ToList();
            int score = usages.Contains(0x00010005u) ? 3   // GenericDesktop/Gamepad
                      : usages.Contains(0x00010004u) ? 2   // GenericDesktop/Joystick
                      : 0;
            if (score > bestScore) { best = item; bestScore = score; }
        }
        return best;
    }

    // Windows' preparsed-caps reconstruction can return a degenerate logical
    // range (0..0) for axes declared 0..0xFFFF (Logical Maximum is a signed
    // HID item, so 0xFFFF reads as -1 and HIDP drops the "invalid" range).
    // Confirmed on this pad's IG_00. Fall back to the field's bit width.
    private static (long Min, long Range, bool FromBits) EffectiveRange(DataItem di)
    {
        long range = (long)di.LogicalMaximum - di.LogicalMinimum;
        if (range > 0) return (di.LogicalMinimum, range, false);
        int bits = Math.Clamp(di.ElementBits, 1, 31);
        return (0, (1L << bits) - 1, true);
    }

    private static short NormalizeSigned(int v, DataItem di)
    {
        var (min, range, fromBits) = EffectiveRange(di);
        long uv = fromBits ? (v & range) : (v - min);
        long scaled = uv * 65535 / range - 32768;
        return (short)Math.Clamp(scaled, short.MinValue, short.MaxValue);
    }

    private static int NormalizeUnsigned(int v, DataItem di)
    {
        var (min, range, fromBits) = EffectiveRange(di);
        long uv = fromBits ? (v & range) : (v - min);
        return (int)Math.Clamp(uv * 65535 / range, 0, 65535);
    }

    // TODO(map): rename dpad members to the real GamepadButton names.
    private static readonly GamepadButton[] s_hat8 =
    {
        GamepadButton.DPadUp,
        GamepadButton.DPadUp | GamepadButton.DPadRight,
        GamepadButton.DPadRight,
        GamepadButton.DPadDown | GamepadButton.DPadRight,
        GamepadButton.DPadDown,
        GamepadButton.DPadDown | GamepadButton.DPadLeft,
        GamepadButton.DPadLeft,
        GamepadButton.DPadUp | GamepadButton.DPadLeft,
    };

    private static GamepadButton DecodeHat(int v, DataItem di)
    {
        if (v < di.LogicalMinimum || v > di.LogicalMaximum) return default; // null state
        int positions = di.LogicalMaximum - di.LogicalMinimum + 1;
        if (positions <= 0) return default;
        int idx = (v - di.LogicalMinimum) * 8 / positions;
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
                    var (_, range, fromBits) = EffectiveRange(di);
                    int bits = (int)Math.Ceiling(Math.Log2(range + 1d));
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
                    parts.Add(fromBits ? $"{name}[raw {bits}bit]" : $"{name}[{di.LogicalMinimum}..{di.LogicalMaximum}]={bits}bit");
                }
            }
        }
        if (buttons > 0) parts.Add($"{buttons} buttons");
        return string.Join("  ", parts);
    }
}
