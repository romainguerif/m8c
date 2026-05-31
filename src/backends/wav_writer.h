// Minimal dependency-free WAV (PCM) writer for the m8c multitrack recorder.
// Released under the MIT licence, https://opensource.org/licenses/MIT
#ifndef WAV_WRITER_H_
#define WAV_WRITER_H_

#include <stddef.h>
#include <stdint.h>

typedef struct wav_writer_s wav_writer_s;

// Open `path` for writing and emit a placeholder WAV header.
// `bits_per_sample` must be 16 or 24 (the M8 multichannel USB audio is 24-bit).
// Returns NULL on failure. Header sizes are patched in wav_writer_close().
wav_writer_s *wav_writer_open(const char *path, uint16_t channels, uint32_t sample_rate,
                              uint16_t bits_per_sample);

// Append `byte_count` bytes of interleaved little-endian PCM. Returns the
// number of bytes written (== byte_count on success).
size_t wav_writer_write(wav_writer_s *w, const void *data, size_t byte_count);

// Patch the RIFF/data chunk sizes and close the file. Frees `w`.
void wav_writer_close(wav_writer_s *w);

#endif
