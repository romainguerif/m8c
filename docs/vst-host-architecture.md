I have everything I need from the findings to synthesize the proposal. No further file inspection is required.

# m8c VST/AU Host — Architecture Proposal

This grafts JUCE's plugin-hosting core (lifted from your Element fork) onto the existing 24ch CoreAudio capture + SDL stereo monitor pipeline, without pulling in Element's graph/node/patching machinery. The whole thing reduces to: an `extern "C"` JUCE host library, a fixed routing topology (3 sends + master), MIDI-CC send control reusing `midi_cc.c`, a stripped-down PDC primitive (`DelayChannelOp`), and an m8c-styled SDL overlay.

---

## 1. Signal Flow

The existing path is: M8 USB → CoreAudio HAL `io_proc` (`m8_audio_capture.c`) delivers 24ch interleaved float32 → `recorder_on_frames(src, frames, ch)` (`recorder.c`) de-interleaves for monitor + disk → SDL stereo monitor back to M8.

We insert FX processing inside `recorder_on_frames`, on the HAL realtime thread, **before** the monitor push:

```
24ch capture (m8_audio_capture.c io_proc)
        │
        ▼  recorder_on_frames(src, frames, ch)   [HAL realtime thread]
        │
   de-interleave → 8 stereo track stems (ch 0..15) [+ existing fx-return/master ch]
        │
        ├─ per track t: outBus[b] += stem[t] * sendGain[t][b]   (b = 0..2)
        │       (sendGain read atomically; set by MIDI CC)
        │
        ├─ Send Bus 0 (stereo) → plugin chain 0 (processBlock in series) → wet0
        ├─ Send Bus 1 (stereo) → plugin chain 1 → wet1
        ├─ Send Bus 2 (stereo) → plugin chain 2 → wet2
        │
        ├─ PDC: delay each shorter bus + dry to align to max(busLatency)
        │
   master_in = dry_mix + wet0 + wet1 + wet2
        │
        ├─ Master chain (stereo) → processBlock in series → master_out
        │
        ▼
   master_out → existing SDL monitor stream (post-mix insert back to M8)
            └─ (optionally) → separate output device
```

Where things sit:
- **8 track stems**: the first 16 capture channels (8 stereo pairs). These are the send sources.
- **fx returns**: in this design FX returns are *internal* (wet0..2), not the M8's own fx-return channels. The M8's existing fx-return/master channels in the 24ch stream can still be recorded to disk unchanged, and the "dry mix" fed to the master can be either the M8 master pair or a sum of stems — see Open Question Q1.
- **Master chain**: processes `dry + sum(wets)`, controlled from the computer (not M8 CC).
- **Return target**: v1 returns `master_out` to the **M8 via the existing SDL monitor stream** (post-mix insert). SDL caps at 8ch, which is fine — we only need stereo back. A separate output device is a later option requiring a second IOProc or aggregate device (Q2).

All buffers pre-allocated and sized to the **maximum** HAL block (HAL gives variable block sizes). No malloc/lock/Obj-C anywhere in `recorder_on_frames`.

---

## 2. Data Model

Four fixed chains, no dynamic graph:

```c
typedef struct {
    char        plugin_desc_xml[...];   // JUCE PluginDescription::createXml()
    void       *instance;               // opaque juce::AudioPluginInstance*
    int         latency_samples;        // cached getLatencySamples()
    bool        bypassed;
} fx_slot_t;

typedef struct {
    fx_slot_t   slots[MAX_SLOTS_PER_BUS];   // ordered chain; suggest 4
    int         num_slots;
    int         total_latency;              // sum of slot latencies
} fx_bus_t;

fx_bus_t  sends[3];
fx_bus_t  master;
float     send_gain[8][3];   // per-track × per-bus, atomic on audio thread
```

- Each chain is an **ordered list** processed front-to-back via `instance->processBlock()`.
- **Persistence**: per slot store `PluginDescription` XML (via `createXml`) + opaque state blob from `AudioProcessor::getStateInformation(MemoryBlock)`. On load, `AudioPluginFormatManager::createPluginInstance(desc)` then `setStateInformation()`. Wrap as JSON: `{ sends:[{slots:[{desc, state_b64, bypassed}]}], master:{...}, send_gain:[[...]] }`. Store alongside m8c config (separate file recommended over embedding in M8 projects, since the M8 owns its own project format — Q5). Add a format version field; plugin state blobs are opaque and can break across plugin updates.

This deliberately avoids Element's `Processor`/`Node`/port/XML-node model — overkill for fixed stereo chains.

---

## 3. Control Mapping (MIDI CC)

**Constraint:** the M8 sends a limited set of CCs per instrument (~10 mappable CC slots per instrument) and uses MIDI **channel** as routing. We need up to 24 values (8 tracks × 3 buses).

**Proposed scheme — channel-as-track, CC-as-bus:**

- Map each of the 8 M8 tracks to **MIDI channel 1..8** (the M8 already routes per-instrument on distinct channels).
- Use **3 fixed CC numbers**, one per send bus, identical across all channels:
  - `CC20` → Send Bus 0 amount
  - `CC21` → Send Bus 1 amount
  - `CC22` → Send Bus 2 amount
- So `(channel, cc) → send_gain[channel-1][cc-20]`. This needs only **3 CC slots per instrument**, well within the M8's 10-slot budget, and naturally scales to all 8 tracks via channel routing. CC value 0..127 maps to gain (linear or -inf..0 dB curve).

This reuses the **exact** `midi_cc.c` path already feeding the recorder; the CC handler just writes `send_gain[ch][bus]` as an atomic float instead of (or in addition to) its current target. The master chain is **not** CC-controlled — adjusted from the overlay UI on the computer.

Plugin *parameter* automation via CC is explicitly out of scope for v1 (Element's `MappingEngine` is overkill). If wanted later, reserve e.g. CC23+ for a small fixed bank per focused slot.

---

## 4. PDC (simplified Element approach)

Element's full PDC (`graphbuilder.cpp`: `getInputLatency`/`getNodeDelay`/`setNodeDelay`, `DelayChannelOp`, feedback `DeferredDelayOp`) handles arbitrary graphs. Our topology is **3 parallel stereo chains + 1 serial master + 1 dry path** — no feedback, no mixing inside a chain. So:

1. Per bus: `busLatency[b] = Σ slot.getLatencySamples()` (only non-bypassed slots).
2. `maxSend = max(busLatency[0..2])`.
3. Before summing into the master input, delay each shorter path so all align:
   - dry path delayed by `maxSend`
   - bus `b` delayed by `maxSend - busLatency[b]`
4. Master chain adds `masterLatency` on top; this is the additional output latency to the M8.
5. **Total output latency** reported to the M8 = `maxSend + masterLatency`. Since we return audio to the M8 as a monitor insert, this is purely a monitoring delay — there is no host to report to, so we only need *internal alignment* (Q in PDC findings). If the M8 plays the dry signal itself while we feed back wet, you'd hear comb filtering unless the M8's own monitor is muted and only our processed return is heard. Decide the dry/wet topology (Q3).

**Reusable primitive:** lift `DelayChannelOp` verbatim from `element/src/engine/graphbuilder.cpp` (~line 215) — a self-contained `HeapBlock<float>` ring buffer with read/write indices, dependency-free, trivially portable to a C `delay_line_t` struct. Cap max delay (Element uses 16384 ≈ 341ms @48k); reject plugins reporting latency above a threshold (e.g. 4096) and warn.

**Latency change handling:** register an `AudioProcessorListener`; on `audioProcessorChanged`/latency change (re-query `getLatencySamples()` per `audioprocessornode.cpp` ~131-158), recompute delays **on the message thread** and atomically publish new delay amounts to the audio thread. AU plugins commonly change latency after a sample-rate change.

Ring buffers must be sized to **max block** and reset on block-size/sample-rate change.

---

## 5. JUCE Integration

**CMake** (copy Element's recipe):
- `cmake/FindJUCE.cmake` — FetchContent JUCE **8.0.12** (match Element for code-lift compatibility), `GIT_SHALLOW ON`, `EXCLUDE_FROM_ALL`, with `find_package(JUCE CONFIG)` fast-path.
- `project(m8c LANGUAGES C CXX)` (add CXX; macOS pulls in OBJCXX automatically for JUCE's `.mm`).
- Modules: `juce_core`, `juce_events`, `juce_data_structures`, `juce_audio_basics`, `juce_audio_processors`, `juce_audio_formats`, plus `juce_gui_basics` + `juce_gui_extra` (needed for plugin editor windows), `juce_dsp` (optional). `juce_audio_devices`/`juce_audio_utils` not strictly needed since CoreAudio I/O stays in m8c.
- Compile defs: `JUCE_PLUGINHOST_VST3=1`, `JUCE_PLUGINHOST_AU=1` (defer LV2/VST2). `JUCE_MODAL_LOOPS_PERMITTED=1` only if you use `runDispatchLoopUntil`.
- Build a **STATIC C++ lib `m8c_juce_host`** with all JUCE deps + the wrapper, link into the existing C executable.
- **Glob hazard:** m8c uses `file(GLOB src/*.c)`. Put the host in a **new dir `src/host/`** excluded from that glob, or `.cpp` lands in the C target and miscompiles.

**C/C++ boundary** — flat `extern "C"` facade in `src/host/juce_host.h` (no JUCE headers ever in a `.c` file; C sees opaque handles + POD):

```c
void juce_host_init(void);           // ScopedJuceInitialiser_GUI + initialiseNSApplication
void juce_host_shutdown(void);
void juce_host_pump(void);           // non-blocking message pump, called each SDL_AppIterate
void juce_host_scan_async(void);
int  juce_host_bus_load_plugin(int bus, const char *desc_xml); // ->slot_id
void juce_host_set_send(int track, int bus, float amount);     // atomic
void juce_host_prepare(double sr, int maxBlock);
void juce_host_process(const float *in24, int frames, float *outStereo); // REALTIME
void juce_host_open_editor(int slot_id);
```

Implementation in `src/host/juce_host.mm` (Obj-C++ on macOS).

**Threading — three roles:**
1. **macOS main thread = JUCE message thread.** m8c already uses `SDL_MAIN_USE_CALLBACKS` (`main.c:11`), so SDL owns NSApp and calls `SDL_AppIterate` on the main thread. **Lowest-risk first cut:** in `SDL_AppInit`, create a process-lifetime `juce::ScopedJuceInitialiser_GUI` + `juce::initialiseNSApplication()`. JUCE editor NSWindows ride SDL's already-running NSApp CFRunLoop. Add a per-tick non-blocking `MessageManager::getInstance()->dispatchPendingMessages()` in `SDL_AppIterate` as a belt-and-suspenders pump. **Do not** call `runDispatchLoop()` (blocks / behaves oddly on macOS). If editors prove unresponsive, fall back to inverting ownership (JUCE owns main, drives SDL from a `juce::Timer`) — bigger refactor of `main.c`.
2. **CoreAudio HAL `io_proc`** = realtime. `juce_host_process` is called from `recorder_on_frames`. De-interleave into pre-allocated `juce::AudioBuffer<float>` per bus, run each chain's `processBlock(buffer, midi)`, apply PDC, sum, run master, write stereo out. No locks/alloc/Obj-C.
3. **SDL render/event** ~120Hz — unchanged.

**Audio thread prep:** call `prepareToPlay(sampleRate, maxBlock)` once per instance off the audio thread at the capture device's actual rate/max block, before audio starts; re-call on rate/device change. **Warm up** each plugin with a few silent `processBlock` calls off the audio thread (first AU/VST3 call may alloc/lock).

**UI→audio handoff** (plugin add/remove/reorder): lock-free. Build an immutable chain snapshot on the message thread, publish via atomic pointer swap, or use the existing `src/backends/queue.h` SPSC queue. Never mutate a live chain from the message thread.

**Scanning:** v1 in-process on the message thread (`AudioPluginFormatManager` + `VST3PluginFormat`/`AudioUnitPluginFormat`, `PluginDirectoryScanner`) — simpler but a bad plugin crashes m8c. Later: lift Element's out-of-process scanner (`pluginmanager.cpp`: `ChildProcessCoordinator`/`Worker` + dead-man's-pedal blacklist). AU scan **must** run on the message thread.

**macOS signing:** host (and any scanner child) need `com.apple.security.cs.disable-library-validation` once codesigned/notarized, plus hardened-runtime/JIT considerations. m8c's existing `Entitlements.plist` needs updating.

---

## 6. UI

Reuse m8c's overlay pattern exactly (parallel to `settings.c`/`log_overlay.c`):
- New `src/plugin_rack.h/.c`: `plugin_rack_toggle_open()`, `plugin_rack_is_open()`, `plugin_rack_handle_event()`, `plugin_rack_render_overlay()`, `plugin_rack_on_texture_size_change()`.
- **Render hook:** call `plugin_rack_render_overlay()` in `render.c:render_screen()` after `settings_render_overlay()` (~line 550). Single 320×240 ARGB8888 `SDL_TEXTUREACCESS_TARGET` texture, `SDL_BLENDMODE_BLEND`, dirty-flag redraw.
- **Input hook:** in `events.c:SDL_AppEvent()`, add `plugin_rack_is_open()` check (enforce mutual exclusivity with settings to avoid alpha-blend layering).
- **Text:** `inprint(rend, str, x, y, fgcolor, bgcolor)` with m8c's palette (white `0xFFFFFF`, cyan `0x00FFFF` selected, etc.). Grid layout from `fonts_get(0)->glyph_x/glyph_y` — no hardcoded coords. Restore the previous font after rendering.
- **Layout:** 320px wide → 4 columns ≈ 80px each: Send 1 / Send 2 / Send 3 / Master, each a vertical rack of N slots (suggest 4). Keyboard/gamepad navigation (m8c is mouse-less): `selected_index` + column index, reuse settings' navigation. Clicking a slot opens a sub-menu to load/swap/bypass/reorder a plugin. Reorder via move-up/move-down keys (no drag-drop).

**Plugin editor windows — defer aggressively (staged):**
- **v0 (lowest risk, matches your overlay vision):** render the parameter list *in m8c's own SDL overlay* by reading `AudioProcessor::getParameters()` and calling `setValue()`. No JUCE GUI window at all.
- **v1:** `juce::GenericAudioProcessorEditor` in a stripped ~60-line `DocumentWindow` shim (lift the core of `element/src/ui/pluginwindow.cpp`: `setUsingNativeTitleBar(true)`, `setContentOwned`, `addToDesktop`; **mandatory** `proc->editorBeingDeleted(editor)` in dtor before deleting — skipping corrupts the plugin). This validates the `SDL_AppIterate` message-pump seam. Drop everything Element-specific (Node/GuiService/Toolbar/WindowManager).
- **v2:** native plugin editors via `createEditorIfNeeded()` with `GenericAudioProcessorEditor` fallback (the `grapheditorcomponent.cpp:976-992` pattern). Track open windows in an array for clean shutdown. These open as separate native NSWindows coexisting with SDL's NSApp — the m8c overlay is the rack chrome; the plugin GUI is its own OS window. Watch keyboard-focus arbitration between SDL's window and floating editor windows.

---

## 7. Phased Roadmap (de-risking sequence)

| Phase | Scope | Acceptance criterion |
|---|---|---|
| **P0 — Build seam** | Add CXX + JUCE static lib via FetchContent 8.0.12; `src/host/` excluded from glob; `juce_host_init/shutdown` from `SDL_AppInit/Quit`; `dispatchPendingMessages()` per iterate. No audio yet. | m8c builds + runs with JUCE linked; SDL window still works; no NSApp conflict; clean shutdown. **This validates the single biggest risk (macOS run-loop ownership) before any feature work.** |
| **P1 — One plugin, master only, no UI** | Scan in-process; hardcode-load one VST3/AU on the master chain; `juce_host_prepare` at capture rate; call `processBlock` in `recorder_on_frames` on the stereo master sum; return to SDL monitor. | Audible processed master signal back to M8; no glitches/dropouts at HAL block sizes; plugin state save/restore round-trips. |
| **P2 — 3 send buses** | Add the 3 stereo send chains + summing + dry/wet mix; static send gains in code. | Each bus audibly processes its summed input; sum returns to M8; multiple plugins per chain in series. |
| **P3 — MIDI CC sends** | Wire `(channel 1-8, CC20/21/22) → send_gain[track][bus]` through `midi_cc.c`; atomic reads on audio thread. | Turning M8 send CCs changes per-track contribution to each bus in real time, no zipper/locks. |
| **P4 — PDC** | Lift `DelayChannelOp`; compute per-bus latency, align buses + dry to max, master adds output latency; latency-change listener republishes atomically. | A high-latency plugin on one send stays phase-aligned with the others at the master sum (verify with a null/phase test); no artifacts on latency change. |
| **P5 — Overlay UI** | `src/plugin_rack.c`: 3 send racks + master rack, slots, load/swap/bypass/reorder, in-SDL param list (editor v0); persistence (JSON). | Open overlay with a key; load/reorder/bypass plugins live; tweak params in m8c's font; state persists across restart. |
| **P6 — Editor windows** | `GenericAudioProcessorEditor` in stripped `DocumentWindow` (v1) → native editors (v2). | Click slot → editor window opens, repaints, edits apply; close cleanly via `editorBeingDeleted`; no focus deadlock with SDL. |
| **P7 — Hardening (optional)** | Out-of-process scanner + blacklist; entitlements/notarization; plugin watchdog. | Bad plugin can't crash host during scan; signed build loads third-party plugins. |

Sequencing rationale: P0 proves the run-loop seam (make-or-break) with zero feature investment; audio path (P1-P4) is validated headless before any UI; the riskiest GUI surface (native editors) is last and fully deferrable.

---

## 8. Key Risks & Open Questions

**Risks (ranked):**
1. **macOS main-thread contention** (SDL NSApp vs JUCE MessageManager). Mitigated by P0 spike; fallback is inverting run-loop ownership.
2. **Realtime safety** — any malloc/lock/Obj-C in `recorder_on_frames`, or first-call plugin allocation, causes HAL-thread glitches. Pre-allocate everything; warm up plugins off-thread.
3. **Plugin crashes** — in-process scan/instantiation can hang the M8 USB stream. v1 risk; out-of-process scanner (P7) mitigates discovery, but `processBlock` crashes still need a watchdog / safe-mode.
4. **Variable HAL block size** — size all `AudioBuffer`s and PDC ring buffers to max block, not nominal.
5. **Dynamic latency** (AU after rate change) — recompute PDC on message thread, publish atomically.
6. **Build size / signing** — full JUCE GUI + audio_processors balloons build time and binary; entitlements update required.

**Open questions for you to decide before we start:**
1. **Dry-path definition (Q1):** is the "dry" master input the M8's own master pair from the 24ch stream, or a sum of the 8 stems we mix ourselves? Affects whether returning wet to the M8 causes comb filtering.
2. **Return target (Q2):** v1 = post-mix insert back to M8 via the existing SDL monitor (stereo) — confirm? Or do you want a second/aggregate output device (needs a second IOProc; SDL monitor caps at 8ch)?
3. **Dry/wet topology (Q3):** fully-wet return (only inter-bus alignment needed) vs dry+wet mix (dry must be delayed to total latency, and M8's own monitor must be muted to avoid double-monitoring)?
4. **Slots per rack:** 2 or 4? Drives UI layout and `MAX_SLOTS_PER_BUS`.
5. **Persistence scope (Q5):** plugin state as a separate m8c-side JSON file, or attempt to embed in M8 project files? (Recommend separate.)
6. **CC scheme confirmation:** is "channel 1-8 = track, CC20/21/22 = the 3 sends" workable with your M8 instrument CC mapping, or do you route differently?
7. **Editor staging:** OK to ship v1 with in-SDL param list / `GenericAudioProcessorEditor` only and defer native plugin GUIs to v2?
8. **Formats:** VST3 + AU v2 only for v1 (defer AUv3, LV2, VST2)? VST2 needs the proprietary SDK path and has licensing constraints.
9. **JUCE version:** pin to Element's 8.0.12 for `DelayChannelOp` / PDC lift compatibility — agreed?

**Relevant files** — m8c side: `/Users/romain/Desktop/M8C/src/main.c`, `/Users/romain/Desktop/M8C/src/backends/recorder.c`, `/Users/romain/Desktop/M8C/src/backends/m8_audio_capture.c`, `/Users/romain/Desktop/M8C/src/backends/queue.h`, `/Users/romain/Desktop/M8C/src/midi_cc.c`, `/Users/romain/Desktop/M8C/src/events.c`, `/Users/romain/Desktop/M8C/src/render.c`, `/Users/romain/Desktop/M8C/src/settings.c`, `/Users/romain/Desktop/M8C/src/SDL2_inprint.h`, `/Users/romain/Desktop/M8C/src/fonts/fonts.h`, `/Users/romain/Desktop/M8C/CMakeLists.txt`. New: `/Users/romain/Desktop/M8C/src/host/juce_host.{h,mm}`, `/Users/romain/Desktop/M8C/src/plugin_rack.{h,c}`. Element side to lift from: `/Users/romain/Desktop/element/element/cmake/FindJUCE.cmake`, `/Users/romain/Desktop/element/element/src/CMakeLists.txt`, `/Users/romain/Desktop/element/element/src/engine/graphbuilder.cpp` (`DelayChannelOp` ~215), `/Users/romain/Desktop/element/element/src/engine/processor.cpp` (latency ~851-865), `/Users/romain/Desktop/element/element/src/nodes/audioprocessornode.cpp` (prepare/latency-listener ~131-158), `/Users/romain/Desktop/element/element/src/ui/pluginwindow.cpp` (editor shim), `/Users/romain/Desktop/element/element/src/ui/grapheditorcomponent.cpp` (editor fallback ~976-992), `/Users/romain/Desktop/element/element/src/pluginmanager.cpp` (out-of-process scanner).
