# RawHidGamepadReader — plan Step 2 core (raw IG_00 provider)

Step 1's probe settled the question: `IG_00` delivers reports fast, and the
~125 Hz seen through `XInputGetState` is host-side software throttling. So
this is **Step 2**: read the node raw.

The ReflexX sources aren't available in this session (no such repo on the
GitHub account this session can see), so the work is staged here as
drop-in files plus a live validation harness:

* `RawHidXInputProvider.cs` — the **finished ReflexX provider** (see below).
  It does **not** compile in this repo, by design: it needs ReflexX's types.
  It is stored here to be **copied out** into the ReflexX solution.
* `RawHidGamepadReader.cs` — the same decode as a standalone, ReflexX-free
  class; the probe project compiles this one (and only this one) for
  `--decode`. Leave both files exactly where they are — don't paste one
  into the other.

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
  An axis shown as `X[raw 16bit]` means Windows' preparsed-caps
  reconstruction returned a degenerate `0..0` logical range (its descriptor
  said `0..0xFFFF`, which reads as `0..-1` under HID's signed items), so the
  reader normalizes from the field's bit width instead — the same fallback
  SDL uses. Confirmed on this pad: all five axes come back degenerate and
  16-bit-raw except the hat.

It compiles as part of `../hid-rate-probe` (kept warning-free there) and
powers that tool's `--decode` mode.

## Validate on the real pad first

```powershell
cd ..\hid-rate-probe
dotnet run -- --decode
```

The tool prints a guided capture sequence (stick sweeps, buttons in a fixed
order, dpad, triggers); do it, press Ctrl+C, and it emits a paste-ready
`DECODE SUMMARY` block — observed per-axis ranges, button press order,
dpad coverage, and rate. That block is everything needed to finalize the
adapter's button map. Meanwhile the live line shows all axes, triggers,
button bits, dpad, and the rolling report rate. Check before wiring
anything into ReflexX:

* sticks reach full range in all four directions (and note the layout
  line's bit depths);
* triggers: **confirmed combined on this pad** — the layout declares a lone
  `Z` and no `Rz`/Brake/Accelerator, so expect `LT` to idle near mid-scale
  (~32768) with the two physical triggers pushing it opposite ways, and
  `RT` staying 0; see the caveat below;
* each button lights exactly one bit (note which — you need the numbering
  for the adapter map);
* dpad hits all 8 directions;
* the rate readout shows the Step-1 number on this same cable/hub.

## Dropping into ReflexX (Step 2)

`RawHidXInputProvider.cs` is the finished provider: written against the
`IInputProvider`/`GamepadState`/`InputDevice`/`GamepadButton` shapes visible
in `XinputAppTransport`, in the same lifecycle idiom (`TryOpenStream`, read
loop, `TearDownAfterReadFailure`, capped-backoff `TryReconnect`) — but
**input-only**: IG_00 accepts no app output reports, so there is no
`IOutputController` half; processed state keeps flowing out through the
existing transport. Copy the one file into `ReflexX.Infrastructure/Input/`
(it does not need `RawHidGamepadReader.cs`).

It compiles clean against stub types mirroring those shapes; the expected
in-tree fixes are cosmetic and all marked `TODO(map)`:

1. `GamepadButton` member names in `ButtonMap` and `s_hat8` (A/B/X/Y,
   shoulders, thumbs, Back/Start, dpad) — rename to the real enum members.
2. `ButtonMap` **order** — one `--decode` run: press A, B, X, Y, LB, RB,
   View, Menu, L3, R3 in that order; the DECODE SUMMARY's press-order list
   should read `1..10`. If not, reorder the array to match.
3. `CombinedZLeftIsHigh` — flip if LT/RT come out swapped in testing.
4. Any `IInputProvider` members missing here → stub them like
   `ExcludeXInputSlots`; delete any that belong to `IOutputController`.

Why the `XinputAppTransport` template can't just take the pad's VID/PID:
IG_00 is input-only (an `output >= ReportSize` device filter rejects it or
picks a vendor collection instead), its reports carry no `0x11` report ID
(that guard drops every report), and its layout is not the fixed 12-byte
app payload — it is whatever the descriptor declares, hence the
parser-driven decode.

`StateUpdated` fires on the read-loop thread at up to native rate — apply
the same marshalling/queueing ReflexX already uses for the app transport's
read loop.

Aside: if a `0x10`/`0x11` app channel to the **Pico in XInput mode** is ever
wanted (an `XinputAppTransport` in the literal sense), that requires
firmware work in this repo first — XInput mode currently enumerates a single
interface with no spare (see `src/boot_mode.h`), so a second vendor-HID
interface would have to be added alongside it.

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
* **Achieved-rate readout:** the provider's `LogDiagnostics(label)` logs
  reports/min/avg/max ms since its previous call — invoke it ~1/s (or on the
  existing diagnostics cadence) so the real-world number on the actual
  cable/hub stays visible in the Logs panel. (`RawHidGamepadReader` exposes
  the same via `GetRateStats()`.)

## Known issue: reconstructed field offsets can be wrong

A real `--decode` run on this pad showed buttons and rate perfect but tiny
stick ranges, a Z frozen through full trigger pulls, and a silent hat: the
descriptor Windows lets HidSharp reconstruct doesn't carry the true
in-report bit positions, so values can be extracted from the wrong offsets
(same root cause family as the degenerate ranges). The remedy is the
probe's `--map` mode: one guided run (hold one control per prompt) prints a
`MAP SUMMARY` of the actual byte offsets per control, from which a
verified fixed-offset decode gets wired into the reader and provider (the
descriptor-driven path stays as the fallback for other devices).

## Caveats to design for

* **Combined triggers — CONFIRMED on this pad.** The IG_00 collection
  declares `X Y Rx Ry Z Hat + 10 buttons`, no `Rz` and no Simulation
  `Brake`/`Accelerator`: the triggers share the one `Z` axis (the classic
  DirectInput view of XInput pads) and both-held is indistinguishable.
  The reader exposes this as `HasSeparateTriggers == false` so the adapter
  can branch on it. Recommended hybrid: raw HID for sticks/buttons/dpad at
  native rate, `XInputGetState` (125 Hz is plenty for triggers) for the two
  separate trigger bytes.
* **Guide button** may not be present on the IG_00 collection at all.
* The reader intentionally does **no** orientation or deadzone processing —
  it hands over exactly what the pad declares; all shaping stays in
  ReflexX's existing pipeline.
