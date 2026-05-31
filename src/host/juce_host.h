// C facade over the JUCE plugin host. The C side of m8c only ever sees this
// header (opaque handles + POD); JUCE headers stay inside the C++/Obj-C++ unit.
//
// Topology: 3 stereo "send" buses (0,1,2) + 1 stereo "master" bus (3), each an
// ordered chain of plugin slots. The 8 M8 track stems feed the sends by
// per-(track,bus) gains; sends + dry sum into the master chain; the master
// output returns to the M8. Plugin chains are mutated from the main/message
// thread; juce_host_process() runs on the realtime audio thread.
// Released under the MIT licence, https://opensource.org/licenses/MIT
#ifndef JUCE_HOST_H_
#define JUCE_HOST_H_

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define JUCE_HOST_NUM_SENDS 3
#define JUCE_HOST_BUS_MASTER 3   // bus id of the master chain
#define JUCE_HOST_NUM_BUSES 4
#define JUCE_HOST_NUM_TRACKS 8

// If this process was relaunched as the isolated plugin-scanner worker,
// runs the worker loop and never returns (exits the process). Otherwise
// returns false immediately. Call FIRST in SDL_AppInit, before any UI/audio.
bool juce_host_run_scanner_if_worker(int argc, char **argv);

// --- Lifecycle (main thread) ---
void juce_host_init(void);
void juce_host_pump(void);
void juce_host_shutdown(void);

// Prepare all chains for audio. Called when capture opens / format changes.
void juce_host_prepare(double sample_rate, int max_block_frames);

// --- Plugin discovery (main/message thread; may block briefly) ---
void juce_host_scan(void);                 // scan VST3 + AU into the known list
bool juce_host_is_scanning(void);
int  juce_host_known_count(void);
void juce_host_known_label(int index, char *out, int out_size); // "Name [FMT]"

// --- Bus / slot management (main thread) ---
int  juce_host_bus_capacity(int bus);      // max slots (4 sends, 8 master)
int  juce_host_bus_slot_count(int bus);
int  juce_host_bus_add(int bus, int known_index); // append; -> slot index or -1
void juce_host_bus_remove(int bus, int slot);
void juce_host_bus_move(int bus, int slot, int delta); // reorder (+1/-1)
void juce_host_slot_label(int bus, int slot, char *out, int out_size);
void juce_host_slot_set_bypass(int bus, int slot, bool bypassed);
bool juce_host_slot_is_bypassed(int bus, int slot);
void juce_host_slot_open_editor(int bus, int slot); // native plugin GUI window

// Per-lane MIDI input channel (1-16, or 0 = no MIDI). Lets a lane host an
// instrument played by MIDI arriving on that channel (from the M8 or a
// hardware keyboard). Master bus ignores MIDI.
int  juce_host_bus_midi_channel(int bus);
void juce_host_bus_set_midi_channel(int bus, int channel);

// Feed a raw MIDI message (from any source) to the host; routed by channel to
// matching lanes. Called from the MIDI thread.
void juce_host_push_midi(const unsigned char *data, int len);

// --- Per-slot quick params: 3 plugin parameters exposed as macros, each
//     assignable from the parameter list or via MIDI-learn (bind a CC). ---
#define JUCE_HOST_NUM_QUICK 3
int  juce_host_slot_param_count(int bus, int slot);
void juce_host_slot_param_name(int bus, int slot, int param, char *out, int out_size);
// quick index 0..2:
int  juce_host_slot_quick_param(int bus, int slot, int quick);      // param idx or -1
void juce_host_slot_quick_assign(int bus, int slot, int quick, int param);
void juce_host_slot_quick_label(int bus, int slot, int quick, char *out, int out_size);
float juce_host_slot_quick_value(int bus, int slot, int quick);     // 0..1
void juce_host_slot_quick_nudge(int bus, int slot, int quick, float delta);
int  juce_host_slot_quick_cc(int bus, int slot, int quick);         // bound CC or -1
void juce_host_begin_learn(int bus, int slot, int quick);           // arm MIDI-learn
bool juce_host_is_learning(void);

// Total compensated output latency (samples), for info/UI.
int  juce_host_latency_samples(void);

// --- Per-track send gains (audio + control threads; atomic) ---
// track 0..7, bus 0..2, gain 0..1.
void juce_host_set_send(int track, int bus, float gain);
float juce_host_get_send(int track, int bus);

// --- Realtime processing (audio thread) ---
// in24: interleaved float32, `channels` per frame (24). Writes `frames` stereo
// frames to out_stereo (interleaved L,R). Returns true if it produced output,
// false if the caller should fall back to its own monitor path.
bool juce_host_process(const float *in24, int channels, int frames, float *out_stereo);

// Like juce_host_process, but also writes the 3 send buses' processed (wet)
// outputs to out_sends (interleaved 6 ch: s1L,s1R,s2L,s2R,s3L,s3R). For the
// multitrack recorder. out_sends may be NULL.
bool juce_host_process_full(const float *in24, int channels, int frames, float *out_stereo,
                            float *out_sends);

// True once at least one chain has content or prepare succeeded (so the caller
// knows to route monitoring through the host instead of its own path).
bool juce_host_is_active(void);

// --- Persistence (main thread) ---
void juce_host_save(const char *path);
void juce_host_load(const char *path);

#ifdef __cplusplus
}
#endif

#endif
