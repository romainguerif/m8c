// Multichannel capture of the M8 USB audio input.
// SDL3 caps at 8 channels, so the 24-channel M8 stream is captured via a
// platform backend (CoreAudio on macOS). Delivered as interleaved S16 frames.
// Released under the MIT licence, https://opensource.org/licenses/MIT
#ifndef M8_AUDIO_CAPTURE_H_
#define M8_AUDIO_CAPTURE_H_

#include <stdbool.h>
#include <stdint.h>

// Called from the realtime capture thread with `num_frames` interleaved
// float32 frames of `channels` channels each (samples in [-1, 1]). Float keeps
// the M8's native 24-bit resolution intact. Must be fast and non-blocking.
//
// `out_monitor` is a scratch buffer of `num_frames * 2` floats the callback may
// fill with the interleaved stereo (L,R) monitor mix. Return the number of
// stereo frames written (0 = no monitor). On macOS, when the capture device
// also has an output (duplex), this is written straight to that output in the
// SAME callback — one device, one clock, one buffer (lowest latency, no drift).
typedef int (*m8_capture_cb)(const float *frames, int num_frames, int channels,
                             float *out_monitor);

// Find the M8 input device, open it at its native channel count, and start
// delivering frames to `cb`. Returns false if no device / unsupported platform.
bool m8_capture_start(m8_capture_cb cb);

// Stop capture and release the device.
void m8_capture_stop(void);

// Negotiated format (valid after a successful start).
int m8_capture_channels(void);
int m8_capture_rate(void);
int m8_capture_block_frames(void);      // negotiated IOProc buffer size, or 0
int m8_capture_set_block_frames(int frames); // set live; returns value in effect

// True if the capture device also drives monitor output in the same IOProc
// (duplex). When false, only SDL monitoring is possible.
bool m8_capture_has_output(void);

// Switch monitoring to the duplex CoreAudio output (on=true) or back to the
// caller's own path (on=false). No-op if the device has no output.
void m8_capture_set_monitor_duplex(bool on);

// --- Secondary capture: a second M8 (e.g. a headless) stereo input ---
// Auto-detects a second "M8" input device (>=2 ch) distinct from the primary,
// and streams its stereo through a lock-free ring. Returns false if none.
bool m8_capture2_start(void);
void m8_capture2_stop(void);
bool m8_capture2_active(void);
// Pop up to `frames` interleaved stereo frames (out holds frames*2 floats).
// Returns the number of stereo frames written (may be < frames on drift).
int m8_capture2_read(float *out, int frames);

#endif
