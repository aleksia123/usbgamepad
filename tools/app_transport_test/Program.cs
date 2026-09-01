using AppTransportTest;

const ushort VendorId = 0x1209;   // pid.codes shared/hobbyist VID
const ushort ProductId = 0x0001;  // pid.codes generic test PID
const byte ReportIdIn = 0x11;     // physical_state, device -> host
const byte ReportIdOut = 0x10;    // output_state, host -> device

Console.WriteLine($"Looking for RP2350 app-transport HID device (VID 0x{VendorId:X4}, PID 0x{ProductId:X4})...");

using HidDevice? device = HidDevice.FindAndOpen(VendorId, ProductId);
if (device is null)
{
    Console.WriteLine("Device not found. Is it plugged in and flashed with the app-transport firmware?");
    return 1;
}

Console.WriteLine($"Opened device. InputReportLength={device.InputReportLength} OutputReportLength={device.OutputReportLength}");

using var cts = new CancellationTokenSource();
Console.CancelKeyPress += (_, e) => { e.Cancel = true; cts.Cancel(); };

// --- background reader: print physical_state (report 0x11) as it arrives ---
Task readerTask = Task.Run(() =>
{
    while (!cts.IsCancellationRequested)
    {
        byte[]? report = device.ReadReport();
        if (report is null) break;
        if (report.Length < 1 + AppControllerPayload.Size || report[0] != ReportIdIn) continue;

        var payload = AppControllerPayload.FromBytes(report.AsSpan(1, AppControllerPayload.Size));
        Console.WriteLine($"[physical] {payload}");
    }
});

bool SendOutput(AppControllerPayload payload)
{
    byte[] report = new byte[device.OutputReportLength];
    report[0] = ReportIdOut;
    Array.Copy(payload.ToBytes(), 0, report, 1, AppControllerPayload.Size);
    return device.WriteReport(report);
}

// --- 1. neutral packet ---
Console.WriteLine("Sending neutral packet...");
SendOutput(default);
await Task.Delay(500);

// --- 2. sine-wave stick pattern (~5s) ---
Console.WriteLine("Sending sine-wave stick pattern for 5s (watch the sticks in Windows' game controller test panel)...");
var swStart = DateTime.UtcNow;
while ((DateTime.UtcNow - swStart).TotalSeconds < 5 && !cts.IsCancellationRequested)
{
    double t = (DateTime.UtcNow - swStart).TotalSeconds;
    short lx = (short)(Math.Sin(t * 2 * Math.PI * 0.5) * 30000);
    short ly = (short)(Math.Cos(t * 2 * Math.PI * 0.5) * 30000);

    SendOutput(new AppControllerPayload { ThumbLX = lx, ThumbLY = ly, ThumbRX = ly, ThumbRY = lx });
    await Task.Delay(10);
}

// --- 3. button and trigger test pattern ---
Console.WriteLine("Sending button/trigger test pattern (A, then triggers ramping)...");
SendOutput(new AppControllerPayload { Buttons = AppControllerPayload.A });
await Task.Delay(500);

for (int i = 0; i <= 255 && !cts.IsCancellationRequested; i += 5)
{
    SendOutput(new AppControllerPayload { LeftTrigger = (byte)i, RightTrigger = (byte)(255 - i) });
    await Task.Delay(20);
}

// --- 4. stop sending; confirm the firmware watchdog forces neutral within 50ms ---
Console.WriteLine("Stopped sending output reports. Firmware should force XInput output to neutral within ~50ms.");
Console.WriteLine("Press Ctrl+C to exit (physical_state reports keep printing above).");

try { await Task.Delay(Timeout.Infinite, cts.Token); } catch (TaskCanceledException) { }

cts.Cancel();
return 0;
