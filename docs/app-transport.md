# App transport — wiring notes

`src/app_transport.c/h` gives the PC application a **second USB interface**
alongside XInput, so it can read the physical pad at ~1000 Hz and write back a
processed output state that the game then sees:

```
pad -> Pico -> application (~1000 Hz) -> Pico -> game
```

This document records what was missing and what completes it, because the
application-side half (`app_transport.c`, `xinput_ms_os_desc.c`, `usb_serial.c`,
and the `main.c` calls) landed before the USB plumbing did.

## Why the second interface exists at all

`XInputGetState()` is served from a fixed ~125 Hz (8 ms) cache inside
Microsoft's `xusb22.sys`. Our XInput endpoint has always declared
`bInterval = 1`, and it makes no difference — the ceiling is host-side. An
application that wants the pad faster has to read something that is not an
XInput device, hence a plain HID interface it can `ReadFile` directly.

## What was missing

`app_transport.c` and `xinput_ms_os_desc.c` were complete, and `main.c` called
`app_transport_init()` and `app_transport_core0_task()`. But nothing put the
interface on the wire:

| Gap | Effect |
|---|---|
| `XInputDescriptors.h` still declared `bNumInterfaces 1` and stock `045E:028E` | Interface 1 was never enumerated, so no application could ever open it |
| `usbdriver.cpp` had no `app_transport` routing | `tud_hid_*_cb` never reached `app_transport_on_{get,set}_report()` |
| The MS OS descriptors were never served | `xusb22.sys` cannot bind interface 0 under a third-party VID/PID without them |
| `usb_serial_string()` was computed but never served as string index 3 | Windows' per-`VID&PID&serial` driver cache could pin a stale probe across reflashes |

The consequence was not a slow pad but a **dead** one: `app_transport_core0_task()`
drives the XInput output *only* from `output_state`, with a 50 ms watchdog
falling back to neutral. With no application able to connect, that watchdog was
permanently expired, so every loop wrote a neutral report.

## What now completes it

* **`XInputDescriptors.h`** — `bNumInterfaces 2`, `wTotalLength 80`, and a HID
  interface 1 (IN `0x82`, OUT `0x02`, 64 bytes, both `bInterval 1`) whose
  `wDescriptorLength` is 33, matching the `_Static_assert` in `app_transport.c`.
  `bDeviceClass` is now `0x00` so Windows treats the device as composite and
  binds the two interfaces separately, and the ids move to **`1209:0001`**.
* **`usbdriver.cpp`** — routes `get_report` / `set_report` /
  `descriptor_report` for HID instance 0 to `app_transport_*`, serves the MS OS
  string at index `0xEE`, and offers vendor control transfers to
  `ms_os_desc_vendor_control_xfer_cb()` first.
* **`XInputDriver::get_descriptor_string_cb`** — serves the per-board serial for
  index 3, and bounds-checks the table (Windows probes indices we do not define).

### Why the ids had to change

Not cosmetic. A composite device at `045E:028E` matches `xusb22.sys` by
*device-level hardware id*, which hands it the whole device and leaves
interface 1 unbound. Off Microsoft's ids, `xusb22.sys` no longer matches
anything — hence the MS OS descriptors, which request the `XUSB10` compatible id
for interface 0 explicitly. `xinput_ms_os_desc.h` documents this in full; the
short version is that class/subclass/protocol matching alone does **not** bind
xusb22 under a third-party id.

`1209:0001` also happens to be what the application's `Rp2350AppTransport`
already looks for, so no application-side change is needed.

## Behaviour

* Reports are sent **on change only**, so a motionless stick produces no
  traffic and a moving one produces a report per 1 ms slot.
* Nothing shapes the sticks: `hid_app.c` copies the pad's values straight
  through. Any deadzone/curve work belongs in the application.
* **With no application running the pad outputs neutral**, by design — output
  comes only from `output_state`, and the 50 ms watchdog falls back to neutral
  when it goes stale.
* The set-report echo in `usbdriver.cpp` is skipped for this interface: the
  application writes on every change, and each echo would claim a 1 ms IN slot
  that a `physical_state` report needs.

## Verifying

Enumeration: Device Manager should show an *XInput-compatible* device plus a
*HID-compliant vendor-defined device* under one parent at `VID_1209&PID_0001`.
Interface 0 showing code 28 means the MS OS descriptor exchange did not happen.

Rate, with `diagnostics/hid-rate-probe`. **List the nodes first** — a bound
XInput device has more than one:

```powershell
dotnet run -- --vid 1209 --pid 0001 --list
dotnet run -- --vid 1209 --pid 0001 --filter mi_01
```

Time the **`MI_01`** node — that is the app-transport interface (13-byte
reports, first byte `0x11`).

Do **not** time the `IG_00` node. That is not an interface the device declares:
`xusb22.sys` synthesizes it for every XInput device (it is how apps detect an
XInput pad) and serves it from the same ~125 Hz cache as `XInputGetState`, so
it reads ~8 ms no matter how fast the hardware is. Its presence is still useful
evidence — it means the MS OS descriptor handshake worked and `xusb22` bound
interface 0. The probe prints a warning if you land on it.

Move a stick continuously and read the **median** interval, not the average —
HID only reports on change, so pauses inflate the mean without costing latency.
Expect a median near 1.0 ms on `MI_01`.

## The pad's own polling interval

Once the app transport was on the wire it measured a hard 8.000 ms, and the
cause was upstream of everything above: Pico-PIO-USB copies the controller's
declared `bInterval` straight into its per-endpoint frame counter
(`pio_usb_ll_configure_endpoint`), so a pad asking for 8 ms is polled at
125 Hz — and nothing downstream can be faster than its own input. Neither the
app transport nor the XInput output can invent samples that were never read.

`xinput_host.c` therefore overrides the IN endpoint's interval to
`XINPUT_HOST_IN_POLL_INTERVAL_MS` (1 ms) when the pad asks for something
slower. Controllers routinely declare a lazy interval while answering far
faster; an interrupt IN endpoint simply NAKs when it has nothing new, so
polling more often costs bus bandwidth and nothing else. It is the same
override that host-side "polling rate" tools apply on Windows.

Raise the constant in `xinput_host.h` if a particular controller misbehaves
when polled faster than it asked for.

## Build note

`lib/tusb_gamepad/.../uart_bridge_task.cpp` needed `hardware/clocks.h` for
`set_sys_clock_khz()`; without it the vendored library does not compile against
pico-sdk 2.1.1.
