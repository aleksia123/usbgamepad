# HidRateProbe — where does the 125 Hz ceiling live?

Throwaway console probe for **Step 1** of the polling-rate diagnosis: is the
~125 Hz seen through `XInputGetState` baked into the pad's XInput HID
collection (`HID\VID_3537&PID_10C5&IG_00`), or is it only the host-side
XInput driver/cache throttling access to a faster endpoint?

Background, already established:

* This repo's firmware is not the bottleneck — the XInput interrupt-IN
  endpoint in `lib/tusb_gamepad` already advertises `bInterval = 1`
  (1 ms / 1000 Hz), single clean interface.
* The ~125 Hz cap on the `XInputGetState` path is enforced host-side by
  Microsoft's XInput driver stack (`xusb22.sys`) regardless of `bInterval`.
* Open question (this tool answers it): does reading the pad's `IG_00` HID
  node **raw** bypass that cap, or is the interface itself slow?

## Prereqs

* Windows 10/11, [.NET 8 SDK or newer](https://dotnet.microsoft.com/download)
  (`dotnet --version` to check).
* **Step 0:** put the physical pad back into **XInput mode** (PlayStation mode
  was only a detour to look at report formats).
* Plug the pad **directly into the PC** — not through the Pico — so you are
  measuring the pad's own interfaces.
* Close anything that may grab the pad (Steam, DS4Windows, vendor software).

## Procedure

From this directory:

```powershell
# 1. Sanity: see the pad's HID nodes (IG_00, MI_01&COL0x, ...)
dotnet run -- --list

# 2. The core measurement: raw blocking reads of IG_00.
#    Wiggle a stick CONTINUOUSLY for the whole run.
dotnet run -- --seconds 8

# 3. Baseline on the same machine/session: XInputGetState path.
dotnet run -- --xinput

# 4. (Only if step 2 says the cap is real) peek at the vendor collection:
dotnet run -- --dump --filter col04

# 5. (After a FAST verdict) validate the plan-Step-2 reader core live:
dotnet run -- --decode
```

`--decode` runs `../raw-hid-xinput/RawHidGamepadReader.cs` — the
descriptor-driven reader destined for ReflexX — and shows decoded axes,
buttons, dpad and the achieved rate in real time. See
`../raw-hid-xinput/README.md` for the validation checklist and integration
guide.

HID interrupt reports are only delivered when the report content *changes* —
an untouched pad produces silence, which the tool reports as timeouts. Keep
the stick moving.

## Interpreting the raw-IG_00 result

| Median interval | Meaning | Next step |
|---|---|---|
| ~1 ms (≈1000 Hz) or better | Cap was `xinput1_4`'s software layer. Reading `IG_00` raw already fixes it. | **Step 2**: raw-HID provider for `IG_00` in ReflexX (descriptor-driven decode, no byte guessing). |
| ~8 ms (≈125 Hz) | Cap is real for this interface — negotiated at the USB level for Xbox compatibility. | **Step 4**: the fast mode lives on the vendor collection (`MI_01&COL04`); map it with `--dump --filter col04`, one control at a time. |
| in between (2–4 ms) | Faster than the XInput path but not full rate. | Still worth Step 2; probe `COL04` too and compare. |
| only timeouts while wiggling | This `IG_00` collection doesn't serve plain `ReadFile` input reports. | Treat like the ~8 ms case: go look at the vendor collection (or RawInput). |

The tool prints the verdict after each run. Also note the reported input
report *length* — it tells you whether the sticks are really 16-bit end to
end on this node.

`--dump` prints a hex line per changed report with `^^` under the bytes that
changed — move one control at a time to map byte offsets (Step 4's harness).

## Other targets

The same tool can time any HID node:

```powershell
dotnet run -- --vid 045E --pid 028E --list     # e.g. the Pico's XInput identity
dotnet run -- --vid <v> --pid <p> --no-filter --index 0
```

Useful later for verifying the full pad → Pico → PC pass-through rate on the
Pico's own `IG_00` node.
