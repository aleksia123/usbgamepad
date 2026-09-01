# Deadzone Tuner

Desktop tool for visually tuning `StickDeadzoneConfig` (see
[`src/deadzone.h`](../../src/deadzone.h)) before pasting the values into
`hid_app.c`. The firmware itself is untouched and stays plain C.

- `native/deadzone.hpp` - class-based C++ port of `src/deadzone.h`'s math.
  Verified bit-exact against the C original across ~251k input/config
  combinations (see parity check performed during development).
- `native/deadzone_api.cpp` - flat C ABI wrapper, exported from
  `deadzone_native.dll`, so the UI calls the same math instead of a third
  reimplementation.
- `DeadzoneTuner/` - WPF (C#, .NET 8) UI. P/Invokes `deadzone_native.dll`.

## Build

```powershell
# 1. Native DLL (needs a MinGW-w64 g++, e.g. MSYS2's ucrt64 toolchain)
tools/deadzone_tuner/native/build.ps1

# 2. UI (needs the .NET 8 SDK)
dotnet build tools/deadzone_tuner/DeadzoneTuner -c Release
```

Run `tools/deadzone_tuner/DeadzoneTuner/bin/Release/net8.0-windows/DeadzoneTuner.exe`.

## Using it

- Pick **Radial** or **Axial** to match the shape you want.
- Drag inside the square to move the raw stick position (blue dot); the
  orange dot shows the value after deadzone shaping.
- The deadzone boundary is drawn in red, maxzone in dashed green.
- The bottom panel plots the response curve (output vs. input magnitude)
  for the currently selected shape/axis.
- **Copy C snippet** puts a ready-to-paste `StickDeadzoneConfig` literal
  on the clipboard.
