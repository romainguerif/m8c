// Minimal dependency-free WAV (PCM) writer for the m8c multitrack recorder.
// Released under the MIT licence, https://opensource.org/licenses/MIT
#include "wav_writer.h"

#include <SDL3/SDL.h>

struct wav_writer_s {
  SDL_IOStream *io;
  uint32_t data_bytes;     // total PCM bytes written
  uint16_t channels;
  uint32_t sample_rate;
  uint16_t bits_per_sample;
};

// Little-endian helpers (WAV is always little-endian).
static void put_u32le(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)(v & 0xFF);
  p[1] = (uint8_t)((v >> 8) & 0xFF);
  p[2] = (uint8_t)((v >> 16) & 0xFF);
  p[3] = (uint8_t)((v >> 24) & 0xFF);
}
static void put_u16le(uint8_t *p, uint16_t v) {
  p[0] = (uint8_t)(v & 0xFF);
  p[1] = (uint8_t)((v >> 8) & 0xFF);
}

// Build the canonical 44-byte PCM header. data_bytes may be 0 at open time and
// is patched on close.
static void build_header(uint8_t hdr[44], const wav_writer_s *w, uint32_t data_bytes) {
  const uint16_t block_align = (uint16_t)(w->channels * (w->bits_per_sample / 8));
  const uint32_t byte_rate = w->sample_rate * block_align;

  SDL_memcpy(hdr, "RIFF", 4);
  put_u32le(hdr + 4, 36 + data_bytes); // chunk size = 36 + Subchunk2Size
  SDL_memcpy(hdr + 8, "WAVE", 4);

  SDL_memcpy(hdr + 12, "fmt ", 4);
  put_u32le(hdr + 16, 16);             // Subchunk1Size (PCM)
  put_u16le(hdr + 20, 1);              // AudioFormat = PCM
  put_u16le(hdr + 22, w->channels);
  put_u32le(hdr + 24, w->sample_rate);
  put_u32le(hdr + 28, byte_rate);
  put_u16le(hdr + 32, block_align);
  put_u16le(hdr + 34, w->bits_per_sample);

  SDL_memcpy(hdr + 36, "data", 4);
  put_u32le(hdr + 40, data_bytes);     // Subchunk2Size
}

wav_writer_s *wav_writer_open(const char *path, uint16_t channels, uint32_t sample_rate,
                              uint16_t bits_per_sample) {
  if (bits_per_sample != 16 && bits_per_sample != 24) {
    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "wav_writer: only 16/24-bit PCM supported (got %u)",
                 bits_per_sample);
    return NULL;
  }

  wav_writer_s *w = SDL_calloc(1, sizeof(wav_writer_s));
  if (w == NULL) {
    return NULL;
  }

  w->io = SDL_IOFromFile(path, "wb");
  if (w->io == NULL) {
    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "wav_writer: cannot open %s: %s", path,
                 SDL_GetError());
    SDL_free(w);
    return NULL;
  }

  w->channels = channels;
  w->sample_rate = sample_rate;
  w->bits_per_sample = bits_per_sample;
  w->data_bytes = 0;

  uint8_t hdr[44];
  build_header(hdr, w, 0);
  if (SDL_WriteIO(w->io, hdr, sizeof(hdr)) != sizeof(hdr)) {
    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "wav_writer: header write failed for %s", path);
    SDL_CloseIO(w->io);
    SDL_free(w);
    return NULL;
  }

  return w;
}

size_t wav_writer_write(wav_writer_s *w, const void *data, size_t byte_count) {
  if (w == NULL || byte_count == 0) {
    return 0;
  }
  const size_t written = SDL_WriteIO(w->io, data, byte_count);
  w->data_bytes += (uint32_t)written;
  return written;
}

void wav_writer_close(wav_writer_s *w) {
  if (w == NULL) {
    return;
  }
  if (w->io != NULL) {
    // Patch the two size fields now that we know data_bytes.
    uint8_t hdr[44];
    build_header(hdr, w, w->data_bytes);
    if (SDL_SeekIO(w->io, 0, SDL_IO_SEEK_SET) == 0) {
      SDL_WriteIO(w->io, hdr, sizeof(hdr));
    } else {
      SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "wav_writer: seek failed, sizes not patched");
    }
    SDL_CloseIO(w->io);
  }
  SDL_free(w);
}
