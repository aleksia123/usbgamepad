// HidRateProbe - throwaway diagnostic for the "125 Hz ceiling" question.
//
// Measures how fast input reports actually arrive from a HID device node,
// by blocking-reading it directly (HidSharp -> ReadFile) and timestamping
// every returned report. Run it against the physical pad's XInput HID
// collection (HID\VID_3537&PID_10C5&IG_00) while wiggling a stick
// continuously, and compare against the XInputGetState path (--xinput).
//
// See README.md next to this file for how to interpret the numbers.
//
// Windows-only in practice (IG_00 / xinput1_4.dll are Windows concepts),
// though the raw HID modes build and run anywhere HidSharp supports.

using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text;
using HidSharp;
using RawHidXInput;

internal static class Program
{
    // Defaults: BIGBIG WON Rainbow 2 Pro (XInput mode), XInput-compatible HID collection.
    private const int DefaultVid = 0x3537;
    private const int DefaultPid = 0x10C5;
    private const string DefaultPathFilter = "ig_00";

    private static volatile bool s_stop;

    private static int Main(string[] args)
    {
        int vid = DefaultVid;
        int pid = DefaultPid;
        string filter = DefaultPathFilter;
        int? index = null;
        double seconds = 8.0;
        bool secondsSet = false;
        bool list = false, dump = false, xinput = false, noFilter = false, decode = false, map = false;
        uint xinputIndex = 0;

        try
        {
            for (int i = 0; i < args.Length; i++)
            {
                switch (args[i])
                {
                    case "--vid": vid = ParseId(args[++i]); break;
                    case "--pid": pid = ParseId(args[++i]); break;
                    case "--filter": filter = args[++i]; break;
                    case "--no-filter": noFilter = true; break;
                    case "--index": index = int.Parse(args[++i]); break;
                    case "--seconds": seconds = double.Parse(args[++i], System.Globalization.CultureInfo.InvariantCulture); secondsSet = true; break;
                    case "--list": list = true; break;
                    case "--dump": dump = true; break;
                    case "--decode": decode = true; break;
                    case "--map": map = true; break;
                    case "--xinput":
                        xinput = true;
                        if (i + 1 < args.Length && uint.TryParse(args[i + 1], out uint xi)) { xinputIndex = xi; i++; }
                        break;
                    case "--help": case "-h": case "/?":
                        PrintUsage();
                        return 0;
                    default:
                        Console.Error.WriteLine($"Unknown argument: {args[i]}");
                        PrintUsage();
                        return 1;
                }
            }
        }
        catch (Exception ex) when (ex is IndexOutOfRangeException or FormatException or OverflowException)
        {
            Console.Error.WriteLine("Bad or missing argument value.");
            PrintUsage();
            return 1;
        }

        Console.CancelKeyPress += (_, e) => { e.Cancel = true; s_stop = true; };

        if (xinput)
            return ProbeXInput(xinputIndex, seconds);

        if (decode)
            return DecodeLive(vid, pid, noFilter ? "" : filter, secondsSet ? seconds : 0);

        var candidates = DeviceList.Local.GetHidDevices(vid, pid).ToList();
        if (candidates.Count == 0)
        {
            Console.Error.WriteLine($"No HID devices found for VID=0x{vid:X4} PID=0x{pid:X4}.");
            Console.Error.WriteLine("Is the pad plugged in DIRECTLY (not through the Pico) and in the expected mode?");
            return 2;
        }

        if (list)
        {
            Console.WriteLine($"HID device nodes for VID=0x{vid:X4} PID=0x{pid:X4}:");
            ListDevices(candidates);
            return 0;
        }

        var filtered = noFilter || filter.Length == 0
            ? candidates
            : candidates.Where(d => d.DevicePath.Contains(filter, StringComparison.OrdinalIgnoreCase)).ToList();

        if (filtered.Count == 0)
        {
            Console.Error.WriteLine($"No device path contains \"{filter}\". All nodes for VID=0x{vid:X4} PID=0x{pid:X4}:");
            ListDevices(candidates);
            Console.Error.WriteLine("Pick one with --filter <substring> or --index <n> (with --no-filter).");
            return 2;
        }

        HidDevice device;
        if (index is int idx)
        {
            if (idx < 0 || idx >= filtered.Count)
            {
                Console.Error.WriteLine($"--index {idx} out of range; matching nodes:");
                ListDevices(filtered);
                return 2;
            }
            device = filtered[idx];
        }
        else if (filtered.Count == 1)
        {
            device = filtered[0];
        }
        else
        {
            Console.Error.WriteLine("More than one node matches; choose with --index <n>:");
            ListDevices(filtered);
            return 2;
        }

        if (map) return MapReports(device);
        // --dump runs until Ctrl+C unless a window was requested explicitly.
        return dump ? DumpReports(device, secondsSet ? seconds : 0) : ProbeRate(device, seconds);
    }

    private static void PrintUsage()
    {
        Console.WriteLine("""
            HidRateProbe - measure real inter-report intervals of a HID device node.

            Modes (default: rate probe of the raw HID node):
              --list           list matching HID nodes and exit
              --dump           hex-dump reports, marking bytes that changed (mapping harness)
              --map            guided per-control byte mapper: hold one control per prompt,
                               prints a paste-ready MAP SUMMARY of true byte offsets
              --decode         live-decode via the descriptor-driven reader; prints a
                               paste-ready DECODE SUMMARY on exit (Step 2 validation)
              --xinput [n]     probe via XInputGetState(n) instead of raw HID (default pad 0)

            Selection (raw HID modes):
              --vid <hex>      vendor id  (default 3537)
              --pid <hex>      product id (default 10C5)
              --filter <s>     substring the device path must contain (default "ig_00")
              --no-filter      ignore the path filter
              --index <n>      pick the n-th matching node when several match

            Other:
              --seconds <n>    measurement window (default 8; --dump default: until Ctrl+C)

            Wiggle a stick CONTINUOUSLY while a probe runs: HID interrupt reports are
            only delivered when the report content changes.
            """);
    }

    private static int ParseId(string s)
    {
        s = s.Trim();
        if (s.StartsWith("0x", StringComparison.OrdinalIgnoreCase)) s = s[2..];
        return int.Parse(s, System.Globalization.NumberStyles.HexNumber);
    }

    private static void ListDevices(IReadOnlyList<HidDevice> devices)
    {
        for (int i = 0; i < devices.Count; i++)
        {
            var d = devices[i];
            string name, usage;
            try { name = d.GetProductName(); } catch { name = "?"; }
            try
            {
                int maxIn = d.GetMaxInputReportLength();
                usage = $"maxInputReport={maxIn}B";
            }
            catch { usage = "maxInputReport=?"; }

            string tlc = DescribeTopLevelUsage(d);
            Console.WriteLine($"  [{i}] {usage}{tlc}  product=\"{name}\"");
            Console.WriteLine($"      {d.DevicePath}");
        }
    }

    private static string DescribeTopLevelUsage(HidDevice d)
    {
        try
        {
            var usages = d.GetReportDescriptor().DeviceItems
                .SelectMany(item => item.Usages.GetAllValues())
                .Distinct()
                .Select(u =>
                {
                    uint page = u >> 16, id = u & 0xFFFF;
                    string label = (page, id) switch
                    {
                        (0x0001, 0x0004) => "GenericDesktop/Joystick",
                        (0x0001, 0x0005) => "GenericDesktop/Gamepad",
                        (0x0001, 0x0008) => "GenericDesktop/MultiAxis",
                        (0x000C, 0x0001) => "Consumer/ConsumerControl",
                        _ when page >= 0xFF00 => $"Vendor(0x{page:X4}/0x{id:X4})",
                        _ => $"0x{page:X4}/0x{id:X4}",
                    };
                    return label;
                });
            string s = string.Join(", ", usages);
            return s.Length > 0 ? $"  usage={s}" : "";
        }
        catch
        {
            return ""; // descriptor not readable; not fatal for the probe
        }
    }

    // ------------------------------------------------------------------ //
    //  Raw HID rate probe
    // ------------------------------------------------------------------ //

    private static int ProbeRate(HidDevice device, double seconds)
    {
        Console.WriteLine($"Probing: {device.DevicePath}");
        if (!TryOpen(device, out var stream)) return 3;

        using (stream)
        {
            stream.ReadTimeout = 2000;
            int bufLen = SafeMaxInputLength(device);
            var buf = new byte[bufLen];

            Console.WriteLine($"Max input report length: {bufLen} bytes (byte 0 is the report ID).");
            Console.WriteLine($"Reading for {seconds:0.#} s - WIGGLE A STICK CONTINUOUSLY the whole time...");
            Console.WriteLine();

            long freq = Stopwatch.Frequency;
            long tStart = Stopwatch.GetTimestamp();
            long tWarmupEnd = tStart + (long)(0.5 * freq);
            long tEnd = tWarmupEnd + (long)(seconds * freq);

            var deltas = new List<double>(1 << 16);
            long tPrev = 0;
            bool havePrev = false, firstShown = false;
            int timeouts = 0, gaps = 0;

            while (!s_stop)
            {
                long now = Stopwatch.GetTimestamp();
                if (now >= tEnd) break;

                int n;
                try { n = stream.Read(buf, 0, buf.Length); }
                catch (TimeoutException)
                {
                    timeouts++;
                    havePrev = false;
                    Console.WriteLine("  ...no report for 2 s (wiggle the stick; make sure this is the right node)");
                    continue;
                }
                catch (IOException ex)
                {
                    Console.Error.WriteLine($"Read failed (device unplugged?): {ex.Message}");
                    break;
                }

                long t = Stopwatch.GetTimestamp();
                if (!firstShown)
                {
                    firstShown = true;
                    Console.WriteLine($"First report ({n} bytes): {Hex(buf, n, 32)}");
                    Console.WriteLine();
                }

                if (t < tWarmupEnd) { tPrev = t; havePrev = true; continue; } // warm-up: discard

                if (havePrev)
                {
                    double ms = (t - tPrev) * 1000.0 / freq;
                    if (ms <= 100.0) deltas.Add(ms);
                    else gaps++; // pause in input, not a polling interval
                }
                tPrev = t;
                havePrev = true;
            }

            PrintStats("raw HID reads", deltas, timeouts, gaps);

            if (deltas.Count >= 20)
            {
                double median = Percentile(deltas, 50);
                Console.WriteLine();
                if (median <= 2.0)
                    Console.WriteLine(
                        "VERDICT: the endpoint/collection is FAST. The ~125 Hz seen through\n" +
                        "XInputGetState is host-side software throttling, not the device.\n" +
                        "=> A raw-HID reader on this node already gets the fast path (plan Step 2).");
                else if (median >= 6.0)
                    Console.WriteLine(
                        "VERDICT: the ~125 Hz cap is REAL at this interface - reading it raw\n" +
                        "does not go faster. The fast mode, if any, lives on another\n" +
                        "collection (e.g. the vendor-defined MI_01&COL04) (plan Step 4).");
                else
                    Console.WriteLine(
                        "VERDICT: intermediate rate - faster than the 125 Hz XInput path but\n" +
                        "not 1 kHz. Raw reads still help (Step 2); compare with --xinput and\n" +
                        "consider probing the vendor collection too (Step 4).");
            }
            else
            {
                Console.WriteLine();
                Console.WriteLine("Too few samples for a verdict. Wiggle continuously for the whole run,");
                Console.WriteLine("and check --list output to confirm the node (pad in XInput mode?).");
            }
        }
        return 0;
    }

    // ------------------------------------------------------------------ //
    //  XInputGetState comparison probe
    // ------------------------------------------------------------------ //

    [StructLayout(LayoutKind.Sequential)]
    private struct XINPUT_GAMEPAD
    {
        public ushort wButtons;
        public byte bLeftTrigger, bRightTrigger;
        public short sThumbLX, sThumbLY, sThumbRX, sThumbRY;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct XINPUT_STATE
    {
        public uint dwPacketNumber;
        public XINPUT_GAMEPAD Gamepad;
    }

    [DllImport("xinput1_4.dll")]
    private static extern uint XInputGetState(uint dwUserIndex, out XINPUT_STATE state);

    private const uint ERROR_DEVICE_NOT_CONNECTED = 1167;

    private static int ProbeXInput(uint padIndex, double seconds)
    {
        Console.WriteLine($"Probing XInputGetState(pad {padIndex}) - spinning as fast as possible,");
        Console.WriteLine("timestamping every dwPacketNumber change.");
        Console.WriteLine($"Reading for {seconds:0.#} s - WIGGLE A STICK CONTINUOUSLY the whole time...");
        Console.WriteLine();

        try
        {
            if (XInputGetState(padIndex, out _) == ERROR_DEVICE_NOT_CONNECTED)
            {
                Console.Error.WriteLine($"XInput pad {padIndex} is not connected.");
                return 2;
            }
        }
        catch (DllNotFoundException)
        {
            Console.Error.WriteLine("xinput1_4.dll not found - this mode only works on Windows.");
            return 3;
        }

        long freq = Stopwatch.Frequency;
        long tStart = Stopwatch.GetTimestamp();
        long tWarmupEnd = tStart + (long)(0.5 * freq);
        long tEnd = tWarmupEnd + (long)(seconds * freq);

        var deltas = new List<double>(1 << 16);
        uint lastPacket = 0;
        long tPrev = 0;
        bool havePrev = false;
        int gaps = 0;
        long polls = 0;

        while (!s_stop)
        {
            long now = Stopwatch.GetTimestamp();
            if (now >= tEnd) break;

            polls++;
            if (XInputGetState(padIndex, out var st) != 0) { havePrev = false; continue; }
            if (havePrev && st.dwPacketNumber == lastPacket) continue;

            long t = Stopwatch.GetTimestamp();
            if (t >= tWarmupEnd && havePrev)
            {
                double ms = (t - tPrev) * 1000.0 / freq;
                if (ms <= 100.0) deltas.Add(ms);
                else gaps++;
            }
            lastPacket = st.dwPacketNumber;
            tPrev = t;
            havePrev = true;
        }

        Console.WriteLine($"({polls / Math.Max(seconds, 0.001):0} XInputGetState calls/s sampling rate)");
        PrintStats("XInput state changes", deltas, 0, gaps);
        Console.WriteLine();
        Console.WriteLine("Compare the modal interval here with the raw-HID probe run on the");
        Console.WriteLine("same pad: identical => the raw node is just as slow; raw much faster");
        Console.WriteLine("=> the cap lives in the XInput driver stack, not the device.");
        return 0;
    }

    // ------------------------------------------------------------------ //
    //  Dump mode (byte-mapping harness for undocumented reports)
    // ------------------------------------------------------------------ //

    private static int DumpReports(HidDevice device, double seconds)
    {
        Console.WriteLine($"Dumping: {device.DevicePath}");
        if (!TryOpen(device, out var stream)) return 3;

        using (stream)
        {
            stream.ReadTimeout = 1000;
            int bufLen = SafeMaxInputLength(device);
            var buf = new byte[bufLen];
            var prev = new byte[bufLen];
            int prevLen = -1;

            bool timed = seconds > 0;
            long tEnd = Stopwatch.GetTimestamp() + (long)(seconds * Stopwatch.Frequency);
            Console.WriteLine(timed
                ? $"Printing reports whose content changed, for {seconds:0.#} s."
                : "Printing reports whose content changed. Ctrl+C to stop.");
            Console.WriteLine("Move ONE control at a time to see which bytes it owns.");
            Console.WriteLine();
            Console.WriteLine("        idx  " + string.Join(" ", Enumerable.Range(0, bufLen).Select(i => $"{i:d2}")));

            long tPrev = 0;
            bool havePrev = false;
            long freq = Stopwatch.Frequency;

            while (!s_stop && !(timed && Stopwatch.GetTimestamp() >= tEnd))
            {
                int n;
                try { n = stream.Read(buf, 0, buf.Length); }
                catch (TimeoutException) { continue; }
                catch (IOException ex)
                {
                    Console.Error.WriteLine($"Read failed (device unplugged?): {ex.Message}");
                    break;
                }

                long t = Stopwatch.GetTimestamp();
                bool changed = n != prevLen || !buf.AsSpan(0, n).SequenceEqual(prev.AsSpan(0, n));
                if (changed)
                {
                    double ms = havePrev ? (t - tPrev) * 1000.0 / freq : 0;
                    var hex = new StringBuilder();
                    var marks = new StringBuilder();
                    for (int i = 0; i < n; i++)
                    {
                        hex.Append($"{buf[i]:X2} ");
                        marks.Append(prevLen == n && buf[i] != prev[i] ? "^^ " : "   ");
                    }
                    Console.WriteLine($"[{ms,8:F2} ms]  {hex.ToString().TrimEnd()}");
                    if (prevLen == n && marks.ToString().Contains('^'))
                        Console.WriteLine($"             {marks.ToString().TrimEnd()}");

                    Array.Copy(buf, prev, n);
                    prevLen = n;
                }
                tPrev = t;
                havePrev = true;
            }
        }
        return 0;
    }

    // ------------------------------------------------------------------ //
    //  Map mode - derive TRUE byte offsets per control from the wire.
    //  Needed because Windows' reconstructed descriptor can misplace field
    //  offsets (caps APIs don't expose real bit positions), which shows up
    //  as tiny axis ranges / a frozen Z / a dead hat in --decode.
    // ------------------------------------------------------------------ //

    private static readonly string[] s_mapSteps =
    {
        "REST: touch nothing, sticks centered",
        "hold LEFT stick fully LEFT",
        "hold LEFT stick fully RIGHT",
        "hold LEFT stick fully UP",
        "hold LEFT stick fully DOWN",
        "hold RIGHT stick fully LEFT",
        "hold RIGHT stick fully RIGHT",
        "hold RIGHT stick fully UP",
        "hold RIGHT stick fully DOWN",
        "hold LEFT trigger fully pressed",
        "hold RIGHT trigger fully pressed",
        "hold DPAD UP",
        "hold DPAD RIGHT",
        "hold button A",
    };

    // The stick block on this pad sits at LE u16 offsets 1/3/5/7 (byte 0 is
    // the report id); print it decoded so rails are visible at a glance.
    private static string AxesAtOdd(byte[] report, int len) => len >= 9
        ? $"axes@1/3/5/7: {report[1] | (report[2] << 8),5} {report[3] | (report[4] << 8),5} {report[5] | (report[6] << 8),5} {report[7] | (report[8] << 8),5}"
        : "";

    private static int MapReports(HidDevice device)
    {
        Console.WriteLine($"Mapping: {device.DevicePath}");
        if (!TryOpen(device, out var stream)) return 3;

        using (stream)
        {
            stream.ReadTimeout = 200;
            int len = SafeMaxInputLength(device);
            var latest = new byte[len];
            bool haveLatest = false;
            string? readError = null;
            bool stopReader = false;
            var latestLock = new object();

            var readerThread = new Thread(() =>
            {
                var tmp = new byte[len];
                while (!Volatile.Read(ref stopReader))
                {
                    int n;
                    try { n = stream.Read(tmp, 0, tmp.Length); }
                    catch (TimeoutException) { continue; }
                    catch (Exception ex) { readError = ex.Message; break; }
                    if (n <= 0) continue;
                    lock (latestLock) { Array.Copy(tmp, latest, len); haveLatest = true; }
                }
            })
            { IsBackground = true, Name = "map-reader" };
            readerThread.Start();

            Console.WriteLine();
            Console.WriteLine("Flow per step: press Enter FIRST (hands free), THEN do the action and");
            Console.WriteLine("HOLD it for the whole 3-second countdown, releasing only after");
            Console.WriteLine("\"captured\" appears. Do nothing else with the pad during a countdown.");
            Console.WriteLine();

            var snaps = new byte[s_mapSteps.Length][];
            for (int i = 0; i < s_mapSteps.Length && !s_stop; i++)
            {
                Console.Write($"[{i + 1}/{s_mapSteps.Length}] NEXT: {s_mapSteps[i]}   ->  Enter to arm: ");
                if (Console.ReadLine() is null)
                {
                    Console.Error.WriteLine("--map needs an interactive console (stdin is redirected).");
                    break;
                }
                Console.Write("    HOLD IT NOW ");
                for (int tick = 0; tick < 30 && !s_stop; tick++)
                {
                    Thread.Sleep(100);
                    if (tick % 5 == 4) Console.Write(".");
                }
                lock (latestLock)
                {
                    if (haveLatest) snaps[i] = (byte[])latest.Clone();
                }
                Console.WriteLine(snaps[i] is null
                    ? "  no report received yet - wiggle a stick once and redo this step"
                    : "  captured - release.");
                if (readError is not null)
                {
                    Console.Error.WriteLine($"read failed: {readError}");
                    break;
                }
            }

            Volatile.Write(ref stopReader, true);
            readerThread.Join(1000);

            Console.WriteLine();
            Console.WriteLine("==== MAP SUMMARY (paste this whole block back) ====");
            var baseline = snaps[0];
            if (baseline is null)
            {
                Console.WriteLine("no baseline captured - rerun and wiggle a stick once before step 1.");
                Console.WriteLine("===================================================");
                return 2;
            }

            Console.WriteLine($"report length: {len} bytes (byte 0 = report id 0x{baseline[0]:X2})");
            Console.WriteLine($"baseline: {Hex(baseline, len, len)}");
            Console.WriteLine($"baseline {AxesAtOdd(baseline, len)}");
            for (int i = 1; i < s_mapSteps.Length; i++)
            {
                var snap = snaps[i];
                Console.Write($"{s_mapSteps[i],-33}: ");
                if (snap is null) { Console.WriteLine("(no data)"); continue; }

                var byteChanges = new List<string>();
                for (int b = 0; b < len; b++)
                    if (snap[b] != baseline[b])
                        byteChanges.Add($"b{b:d2} {baseline[b]:X2}->{snap[b]:X2}");
                if (byteChanges.Count == 0) { Console.WriteLine("no change"); continue; }
                Console.WriteLine(string.Join("  ", byteChanges));

                // Both alignments: fields sit at odd offsets on this pad, so
                // pair every changed byte with its neighbor.
                var wordChanges = new List<string>();
                for (int b = 0; b + 1 < len; b++)
                    if (snap[b] != baseline[b] || snap[b + 1] != baseline[b + 1])
                        wordChanges.Add($"u16@{b:d2} {baseline[b] | (baseline[b + 1] << 8)}->{snap[b] | (snap[b + 1] << 8)}");
                if (wordChanges.Count > 0)
                    Console.WriteLine($"{new string(' ', 35)}{string.Join("  ", wordChanges)}");
                Console.WriteLine($"{new string(' ', 35)}{AxesAtOdd(snap, len)}");
            }
            Console.WriteLine("===================================================");
            Console.WriteLine("Paste EVERYTHING between the ==== lines, including the baseline lines.");
        }
        return 0;
    }

    // ------------------------------------------------------------------ //
    //  Decode mode - live view through the Step-2 RawHidGamepadReader
    // ------------------------------------------------------------------ //

    private sealed class Observed
    {
        public bool HaveFirst;
        public RawGamepadState First;
        public int MinLX = int.MaxValue, MaxLX = int.MinValue;
        public int MinLY = int.MaxValue, MaxLY = int.MinValue;
        public int MinRX = int.MaxValue, MaxRX = int.MinValue;
        public int MinRY = int.MaxValue, MaxRY = int.MinValue;
        public int MinLT = int.MaxValue, MaxLT = int.MinValue;
        public int MinRT = int.MaxValue, MaxRT = int.MinValue;
        public uint ButtonsSeen;
        public readonly List<int> PressOrder = new();
        public readonly HashSet<RawDpad> DpadSeen = new();
        public long PrevTicks;
        public long Count;
        public double SumMs, MinMs = double.MaxValue, MaxMs;
        public readonly List<double> IntervalSamples = new();
    }

    private static string DpadName(RawDpad d) => d switch
    {
        RawDpad.None => "none",
        RawDpad.Up => "U",
        RawDpad.Up | RawDpad.Right => "UR",
        RawDpad.Right => "R",
        RawDpad.Down | RawDpad.Right => "DR",
        RawDpad.Down => "D",
        RawDpad.Down | RawDpad.Left => "DL",
        RawDpad.Left => "L",
        RawDpad.Up | RawDpad.Left => "UL",
        _ => d.ToString(),
    };

    private static int DecodeLive(int vid, int pid, string filter, double seconds)
    {
        Console.WriteLine($"Live decode of VID=0x{vid:X4} PID=0x{pid:X4} filter=\"{filter}\"");
        Console.WriteLine("""
            via RawHidGamepadReader (descriptor-driven). For a complete summary, do
            this sequence, then press Ctrl+C:
              1. slow full circle on EACH stick, then push each to all 4 rails
              2. press each button ONCE, in a fixed order you note down, e.g.
                 A, B, X, Y, LB, RB, Back/View, Start/Menu, L3, R3 (pause between)
              3. dpad: all 8 directions
              4. pull LT fully, release; pull RT fully, release
            A paste-ready DECODE SUMMARY block is printed at the end.
            """);
        Console.WriteLine();

        var obs = new Observed();
        using var reader = new RawHidGamepadReader(vid, pid, filter);
        reader.Status += msg => Console.WriteLine($"\n[reader] {msg}");
        reader.StateUpdated += st =>
        {
            lock (obs)
            {
                if (!obs.HaveFirst) { obs.First = st; obs.HaveFirst = true; }
                obs.MinLX = Math.Min(obs.MinLX, st.LeftX); obs.MaxLX = Math.Max(obs.MaxLX, st.LeftX);
                obs.MinLY = Math.Min(obs.MinLY, st.LeftY); obs.MaxLY = Math.Max(obs.MaxLY, st.LeftY);
                obs.MinRX = Math.Min(obs.MinRX, st.RightX); obs.MaxRX = Math.Max(obs.MaxRX, st.RightX);
                obs.MinRY = Math.Min(obs.MinRY, st.RightY); obs.MaxRY = Math.Max(obs.MaxRY, st.RightY);
                obs.MinLT = Math.Min(obs.MinLT, st.LeftTrigger); obs.MaxLT = Math.Max(obs.MaxLT, st.LeftTrigger);
                obs.MinRT = Math.Min(obs.MinRT, st.RightTrigger); obs.MaxRT = Math.Max(obs.MaxRT, st.RightTrigger);

                uint newBits = st.Buttons & ~obs.ButtonsSeen;
                for (int bit = 0; bit < 32; bit++)
                    if ((newBits & (1u << bit)) != 0) obs.PressOrder.Add(bit + 1);
                obs.ButtonsSeen |= st.Buttons;
                if (st.Dpad != RawDpad.None) obs.DpadSeen.Add(st.Dpad);

                if (obs.PrevTicks != 0)
                {
                    double ms = (st.TimestampTicks - obs.PrevTicks) * 1000.0 / Stopwatch.Frequency;
                    if (ms <= 250)
                    {
                        obs.Count++;
                        obs.SumMs += ms;
                        if (ms < obs.MinMs) obs.MinMs = ms;
                        if (ms > obs.MaxMs) obs.MaxMs = ms;
                        if (obs.IntervalSamples.Count < (1 << 17)) obs.IntervalSamples.Add(ms);
                    }
                }
                obs.PrevTicks = st.TimestampTicks;
            }
        };
        reader.Start();

        bool live = !Console.IsOutputRedirected; // piped/redirected: no \r line noise
        bool timed = seconds > 0;
        long freq = Stopwatch.Frequency;
        long tEnd = Stopwatch.GetTimestamp() + (long)(seconds * freq);
        long lastStatsAt = Stopwatch.GetTimestamp();
        RateStats? rate = null;
        int prevLen = 0;

        while (!s_stop && !(timed && Stopwatch.GetTimestamp() >= tEnd))
        {
            Thread.Sleep(50);
            long now = Stopwatch.GetTimestamp();
            if (now - lastStatsAt >= freq)
            {
                rate = reader.GetRateStats();
                lastStatsAt = now;
            }
            if (!live || !reader.IsConnected) continue;

            var st = reader.LastState;
            string dpad =
                (st.Dpad.HasFlag(RawDpad.Up) ? "U" : "-") +
                (st.Dpad.HasFlag(RawDpad.Down) ? "D" : "-") +
                (st.Dpad.HasFlag(RawDpad.Left) ? "L" : "-") +
                (st.Dpad.HasFlag(RawDpad.Right) ? "R" : "-");
            string line =
                $"LX{st.LeftX,7} LY{st.LeftY,7} RX{st.RightX,7} RY{st.RightY,7}  " +
                $"LT{st.LeftTrigger,6} RT{st.RightTrigger,6}  BTN 0x{st.Buttons:X4} DPAD {dpad}  " +
                $"| {(rate == null ? "measuring..." : rate.ToString())}";
            Console.Write("\r" + line.PadRight(Math.Max(prevLen, line.Length)));
            prevLen = line.Length;
        }
        if (live) Console.WriteLine();
        reader.Stop();

        Console.WriteLine();
        Console.WriteLine("==== DECODE SUMMARY (paste this whole block back) ====");
        Console.WriteLine($"layout   : {reader.LayoutDescription ?? "(never connected)"}");
        Console.WriteLine($"triggers : {(reader.LayoutDescription == null ? "?" : reader.HasSeparateTriggers ? "separate" : "COMBINED on Z")}");
        lock (obs)
        {
            if (!obs.HaveFirst)
            {
                Console.WriteLine("no reports received - was the pad connected and moving?");
            }
            else
            {
                Console.WriteLine($"LX range : {obs.MinLX} .. {obs.MaxLX}   (first seen {obs.First.LeftX})");
                Console.WriteLine($"LY range : {obs.MinLY} .. {obs.MaxLY}   (first seen {obs.First.LeftY})");
                Console.WriteLine($"RX range : {obs.MinRX} .. {obs.MaxRX}   (first seen {obs.First.RightX})");
                Console.WriteLine($"RY range : {obs.MinRY} .. {obs.MaxRY}   (first seen {obs.First.RightY})");
                Console.WriteLine($"LT range : {obs.MinLT} .. {obs.MaxLT}   (first seen {obs.First.LeftTrigger})");
                Console.WriteLine($"RT range : {obs.MinRT} .. {obs.MaxRT}   (first seen {obs.First.RightTrigger})");
                Console.WriteLine($"buttons  : mask 0x{obs.ButtonsSeen:X4}, press order: " +
                    (obs.PressOrder.Count == 0 ? "(none pressed)" : string.Join(", ", obs.PressOrder)));
                Console.WriteLine($"dpad     : {(obs.DpadSeen.Count == 0 ? "(none seen)" : string.Join(" ", obs.DpadSeen.Select(DpadName)))}");
                double avg = obs.Count > 0 ? obs.SumMs / obs.Count : 0;
                double med = obs.IntervalSamples.Count > 0 ? Percentile(obs.IntervalSamples, 50) : 0;
                Console.WriteLine($"rate     : {obs.Count} intervals  min={obs.MinMs:F3}  med={med:F3}  avg={avg:F3}  max={obs.MaxMs:F3} ms");
                Console.WriteLine($"           ~{(med > 0 ? 1000.0 / med : 0):F0} Hz sustained while input changes; the average");
                Console.WriteLine($"           includes pauses - HID only delivers reports ON CHANGE.");
            }
        }
        Console.WriteLine("======================================================");
        return 0;
    }

    // ------------------------------------------------------------------ //
    //  Helpers
    // ------------------------------------------------------------------ //

    private static bool TryOpen(HidDevice device, out HidStream stream)
    {
        if (device.TryOpen(out stream!)) return true;
        Console.Error.WriteLine("Could not open the device for reading.");
        Console.Error.WriteLine("Close anything that might hold it exclusively (Steam, DS4Windows,");
        Console.Error.WriteLine("vendor software) and/or retry from an elevated prompt.");
        return false;
    }

    private static int SafeMaxInputLength(HidDevice device)
    {
        try { return Math.Max(8, device.GetMaxInputReportLength()); }
        catch { return 64; }
    }

    private static string Hex(byte[] buf, int n, int max)
    {
        int shown = Math.Min(n, max);
        string s = string.Join(" ", buf.Take(shown).Select(b => $"{b:X2}"));
        return shown < n ? s + " ..." : s;
    }

    private static double Percentile(List<double> values, double p)
    {
        var sorted = values.OrderBy(v => v).ToList();
        int i = (int)Math.Round((p / 100.0) * (sorted.Count - 1));
        return sorted[Math.Clamp(i, 0, sorted.Count - 1)];
    }

    private static void PrintStats(string what, List<double> deltas, int timeouts, int gaps)
    {
        Console.WriteLine();
        Console.WriteLine($"--- {what}: {deltas.Count} intervals ---");
        if (gaps > 0) Console.WriteLine($"({gaps} pauses > 100 ms excluded; {timeouts} read timeouts)");
        else if (timeouts > 0) Console.WriteLine($"({timeouts} read timeouts)");
        if (deltas.Count == 0) return;

        double min = deltas.Min(), max = deltas.Max(), avg = deltas.Average();
        double med = Percentile(deltas, 50), p99 = Percentile(deltas, 99);

        Console.WriteLine($"interval ms: min={min:F3}  median={med:F3}  avg={avg:F3}  p99={p99:F3}  max={max:F3}");
        Console.WriteLine($"=> ~{1000.0 / med:F0} Hz at the median ({1000.0 / avg:F0} Hz at the mean)");
        Console.WriteLine();

        // Buckets centered on the common USB polling intervals, with
        // geometric boundaries (x0.707 .. x1.414 around each center).
        (double center, string label)[] buckets =
        {
            (0.125, "0.125 ms (~8000 Hz)"),
            (0.25,  "0.25 ms  (~4000 Hz)"),
            (0.5,   "0.5 ms   (~2000 Hz)"),
            (1,     "1 ms     (~1000 Hz)"),
            (2,     "2 ms      (~500 Hz)"),
            (4,     "4 ms      (~250 Hz)"),
            (8,     "8 ms      (~125 Hz)"),
            (16,    "16 ms    (~62.5 Hz)"),
        };
        var counts = new int[buckets.Length + 1];
        foreach (double d in deltas)
        {
            int b = buckets.Length; // overflow bucket
            for (int i = 0; i < buckets.Length; i++)
            {
                if (d < buckets[i].center * 1.4142)
                {
                    b = i;
                    break;
                }
            }
            counts[b]++;
        }
        int biggest = counts.Max();
        for (int i = 0; i <= buckets.Length; i++)
        {
            if (counts[i] == 0) continue;
            string label = i < buckets.Length ? buckets[i].label : "> 22.6 ms (slower)  ";
            int barLen = biggest > 0 ? (int)Math.Round(40.0 * counts[i] / biggest) : 0;
            double pct = 100.0 * counts[i] / deltas.Count;
            Console.WriteLine($"  {label,-22} {new string('#', barLen),-40} {counts[i],6} ({pct:F1}%)");
        }
    }
}
