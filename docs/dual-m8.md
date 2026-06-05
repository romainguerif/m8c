# Dual-M8 mode (Phase A: display + control)

## Goal
Drive **two M8s from one m8c instance** — e.g. a real M8 + a headless M8 on a
second USB port — to get one combined 16-track surface. Both screens show in a
single window; **one gamepad/keyboard** controls them, with a **combo to switch
focus** between the two.

## Activation: automatic, not a setting
- At startup m8c enumerates connected M8 serial ports.
  - **≥ 2 M8s found** (and no `--dev` given) → **dual mode**.
  - **1 M8** (or `--dev <port>` forcing a specific one) → **normal single mode,
    completely unchanged.**
- No config flag, no menu. Plug two in → it goes dual. This keeps the default
  experience identical and risk-free.

## Architecture

### Devices (`src/backends/m8_libserialport.c`)
Per-M8 state is wrapped in a `m8_device_s` (serial port, read/SLIP buffers, SLIP
parser, message queue, reader thread, stop flag, index). An array
`g_dev[M8_MAX_DEVICES]` + `g_count` replaces the old single globals. Each device
has its **own reader thread** filling its **own queue**; SLIP has no context
param, so each device uses a small dedicated `recv_*` callback that pushes to its
own queue (max 2 → 2 tiny callbacks, no change to the borrowed SLIP lib).

### Public API (`src/backends/m8.h`) — device-indexed
```
int  m8_initialize(verbose, preferred_device); // opens up to M8_MAX_DEVICES, returns count
int  m8_device_count(void);
int  m8_process_data(int dev, conf);
int  m8_send_msg_controller(int dev, input);
int  m8_send_msg_keyjazz(int dev, note, velocity);
int  m8_reset_display(int dev);
int  m8_enable_display(int dev, reset);
int  m8_close(void);                            // closes all
```

### Rendering (`src/render.c`)
- `device_texture[M8_MAX_DEVICES]` replaces the single `main_texture`.
- `renderer_set_active_device(dev)` selects which texture `draw_*` (and the
  per-frame target-restore) write to. The main loop sets the active device
  **before draining that device's command queue**, so `process_command()` and
  the `draw_*` signatures stay **unchanged** (lowest-risk integration).
- `render_screen()`:
  - **single** → texture[0] through the existing scaling/HD path (unchanged).
  - **dual** → both textures side-by-side, vertically centred, with a coloured
    border around the **focused** device.

### Input & focus (`src/input.c`, `src/events.c`)
- `g_focused_device` (0/1) selects the target of all key/gamepad input.
- A **gamepad combo** and a **keyboard key** toggle focus; the focused screen is
  highlighted. Single mode: always device 0.
- Physical buttons on a real M8 keep working in parallel (we only *send* extra
  key events; we don't take anything away).

## What stays shared (Phase A)
One window, one renderer, one config, and the **audio/MIDI/plugin-host/recorder
stay single-device for now** (they target the first M8). Combining audio across
both M8s (16-track recorder, shared sends/master) is **Phase B**.

## Phasing
1. **Backend + indexed API** — per-device struct, callers pass dev 0. Single mode
   identical. *(foundation, invisible)*
2. **Render texture array** — `renderer_set_active_device`, single render path
   unchanged.
3. **Auto dual-mode** — open 2 M8s, process both, split-screen + focus combo.
4. **Phase B (later)** — unified 16-track audio: combined recorder + shared
   send/master FX, synced transport (one M8 master clock, the other slaved).

## Sync note (Phase B)
Two M8s must share a clock or they drift. Plan: one M8 is MIDI-clock master, the
other slaved (the headless slaves cleanly). m8c already relays MIDI clock to the
plugin host, so it can also forward/echo clock between devices if needed.
