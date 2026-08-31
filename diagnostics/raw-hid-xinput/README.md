# RawHidGamepadReader — plan Step 2 core (raw IG_00 provider)

Step 1's probe settled the question: `IG_00` delivers reports fast, and the
~125 Hz seen through `XInputGetState` is host-side software throttling. So
this is **Step 2**: read the node raw.

The ReflexX sources aren't available in this session (no such repo on the
GitHub account this session can see), so the provider core is staged here as
a **self-contained drop-in file** — HidSharp only, zero ReflexX types — plus
a live validation harness. Everything hard lives in the core; the ReflexX
adapter that remains is ~40 trivial lines.

## What's here

`RawHidGamepadReader.cs`:

* Enumerates the pad's XInput HID collection by VID/PID/path substring
  (defaults `3537:10C5`, `"ig_00"`), with automatic reconnect.
* **Descriptor-driven decode** — axes, hat, buttons and their logical
  min/max are read from the declared report descriptor via HidSharp's
  report parser. No fixed byte offsets, no assumed "Xbox 360 format";
  native resolution is preserved by normalizing from the declared range.
* `StateUpdated` fires per report at native rate (on the reader thread).
* `GetRateStats()` returns min/avg/max/last report interval since the last
  call — the achieved-rate readout for the Logs panel.
* `LayoutDescription` reports the declared per-axis logical ranges and bit
  depths — this answers "is it actually 16-bit under the XInput skin?".

It compiles as part of `../hid-rate-probe` (kept warning-free there) and
powers that tool's `--decode` mode.

## Validate on the real pad first

```powershell
cd ..\hid-rate-probe
dotnet run -- --decode
```

Live line shows all axes, triggers, button bits, dpad, and the rolling
report rate. Check before wiring anything into ReflexX:

* sticks reach full range in all four directions (and note the layout
  line's bit depths);
* **LT and RT move independently** — see the combined-trigger caveat below;
* each button lights exactly one bit (note which — you need the numbering
  for the adapter map);
* dpad hits all 8 directions;
* the rate readout shows the Step-1 number on this same cable/hub.

## Dropping into ReflexX (Step 2)

1. Copy `RawHidGamepadReader.cs` to `ReflexX.Infrastructure/Input/`
   (rename the `RawHidXInput` namespace to taste). HidSharp is already a
   dependency there (`Rp2350AppTransport.cs`).
2. Add the thin adapter. Sketch — **placeholder member names**, align with
   the real `IInputProvider`/`GamepadState` shapes:

```csharp
public sealed class RawHidXInputProvider : IInputProvider, IDisposable
{
    private readonly RawHidGamepadReader _reader = new(0x3537, 0x10C5, "ig_00");

    public event Action<GamepadState>? StateUpdated; // match the real signature

    public void Start()
    {
        _reader.Status += msg => Log.Info($"[RawHid] {msg}");        // -> Logs panel
        _reader.StateUpdated += raw => StateUpdated?.Invoke(Map(raw));
        _reader.Start();                                             // own thread, reconnects itself
    }

    private static GamepadState Map(in RawGamepadState raw) => new()
    {
        LeftX  = raw.LeftX,
        LeftY  = Invert(raw.LeftY),   // HID Y is down-positive; XInput is up-positive
        RightX = raw.RightX,
        RightY = Invert(raw.RightY),
        LeftTrigger  = raw.LeftTrigger,   // 0..65535; rescale if GamepadState wants bytes
        RightTrigger = raw.RightTrigger,
        // Button numbering: CONFIRM ONCE with --decode. Typical XInput-style order:
        // 1=A 2=B 3=X 4=Y 5=LB 6=RB 7=Back 8=Start 9=LS 10=RS 11=Guide
        A = raw.GetButton(1), B = raw.GetButton(2),
        X = raw.GetButton(3), Y = raw.GetButton(4),
        // ... dpad from raw.Dpad flags ...
    };

    private static short Invert(short v) => v == short.MinValue ? short.MaxValue : (short)-v;

    public void Dispose() => _reader.Dispose();
}
```

`StateUpdated` fires on the reader thread at up to native rate — apply the
same marshalling/queueing ReflexX already uses for `Rp2350AppTransport`'s
read loop.

## Step 3 wiring

* **Double-read exclusion:** while this provider is active, `XInputProvider`
  must not also emit state for the same pad. `XInputGetState` doesn't expose
  VID/PID per user index, so the robust rule is mode-level: when the
  high-polling toggle is ON and `_reader.IsConnected`, suppress the
  XInput-provider path for that pad (fall back automatically when
  disconnected). Extend `DirectInputProvider.cs`'s existing XInput-VID/PID
  exclusion list with `3537:10C5` the same way it already avoids
  double-reads.
* **Registration:** `WebShellServices.cs`, gated behind a Settings toggle
  (e.g. "High-polling mode"), default **off** — device-specific behavior
  shouldn't silently change generic pads.
* **Achieved-rate readout:** poll `GetRateStats()` about once a second and
  surface it in the Logs panel / debug stat, so the real-world number on the
  actual cable/hub stays visible.

## Caveats to design for

* **Combined triggers.** If the IG_00 descriptor declares only `Z` (no
  `Rz` and no Simulation `Brake`/`Accelerator`), the triggers share one
  axis — the classic DirectInput view of XInput pads — and both-held is
  indistinguishable. `--decode`'s layout line reveals this in seconds. If
  so, a clean hybrid works: raw HID for sticks/buttons/dpad at native rate,
  `XInputGetState` (125 Hz is plenty for triggers) for the two trigger
  bytes.
* **Guide button** may not be present on the IG_00 collection at all.
* The reader intentionally does **no** orientation or deadzone processing —
  it hands over exactly what the pad declares; all shaping stays in
  ReflexX's existing pipeline.
