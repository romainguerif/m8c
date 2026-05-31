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
typedef void (*m8_capture_cb)(const float *frames, int num_frames, int channels);

// Find the M8 input device, open it at its native channel count, and start
// delivering frames to `cb`. Returns false if no device / unsupported platform.
bool m8_capture_start(m8_capture_cb cb);

// Stop capture and release the device.
void m8_capture_stop(void);

// Negotiated format (valid after a successful start).
int m8_capture_channels(void);
int m8_capture_rate(void);

#endif
