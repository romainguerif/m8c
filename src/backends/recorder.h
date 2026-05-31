// Multitrack recorder for the M8 24-channel USB audio stream.
// Owns the M8 audio input (opened once at native channel count): extracts the
// master (ch 1-2) for monitoring back to the M8, and writes one stereo WAV per
// track while recording. Recording is active iff ARMED (arm CC) AND PLAYING
// (MIDI transport). Released under the MIT licence.
#ifndef RECORDER_H_
#define RECORDER_H_

#include <stdbool.h>

// Load configuration (config/recorder.ini + env). Call once at startup.
void recorder_init(void);

// Open / close the M8 capture (24ch input + master monitor output). Call on
// device connect / disconnect.
bool recorder_open_capture(void);
void recorder_close_capture(void);

// Stop everything and release resources.
void recorder_shutdown(void);

bool recorder_is_recording(void);

// --- State inputs (reconciled internally) ---
void recorder_set_armed(bool armed);     // from the arm CC
void recorder_set_playing(bool playing); // from MIDI transport Start/Stop
void recorder_toggle_manual(void);       // keyboard force toggle (testing)

// --- Trigger configuration, consumed by the MIDI backend ---
int recorder_arm_cc(void);                        // CC number, 0-127
int recorder_arm_channel(void);                   // 1-16, or 0 = any
unsigned int recorder_record_key_scancode(void);  // SDL scancode test key

#endif
