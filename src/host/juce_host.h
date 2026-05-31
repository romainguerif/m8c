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
