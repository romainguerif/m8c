// Multichannel capture of the M8 USB audio input.
// Released under the MIT licence, https://opensource.org/licenses/MIT
#include "m8_audio_capture.h"

#include <SDL3/SDL.h>

static m8_capture_cb g_cb = NULL;
static int g_channels = 0;
static int g_rate = 0;
static int g_block_frames = 0; // actual IOProc buffer size negotiated with the device

// Requested IOProc buffer size (frames). Larger = more headroom for the plugin
// chain (fewer crackles) at the cost of latency. Overridable via env M8C_BLOCK.
#define M8_CAPTURE_BLOCK_FRAMES 1024

int m8_capture_channels(void) { return g_channels; }
int m8_capture_rate(void) { return g_rate; }
int m8_capture_block_frames(void) { return g_block_frames; }

#ifdef __APPLE__
// Forward decl (defined in the CoreAudio section below).
int m8_capture_set_block_frames(int frames);
#else
int m8_capture_set_block_frames(int frames) {
  (void)frames;
  return g_block_frames;
}
#endif

#ifdef __APPLE__
// ---------------------------------------------------------------------------
// CoreAudio (HAL AudioDeviceIOProc) implementation
// ---------------------------------------------------------------------------
#include <CoreAudio/CoreAudio.h>

static AudioObjectID g_dev = kAudioObjectUnknown;
static AudioDeviceIOProcID g_proc = NULL;
static AudioStreamBasicDescription g_asbd;     // input format
static AudioStreamBasicDescription g_out_asbd; // output format (if duplex)
static int g_out_channels = 0;                 // device output channels (0 = no output)
static float *g_scratch = NULL;     // interleaved float32 input
static int g_scratch_frames = 0;
static float *g_mon = NULL;         // interleaved stereo monitor scratch (frames*2)
static int g_mon_frames = 0;
static int g_monitor_duplex = 0;    // 0 = monitor via SDL (default), 1 = via this IOProc

bool m8_capture_has_output(void) { return g_out_channels >= 2; }
void m8_capture_set_monitor_duplex(bool on) { g_monitor_duplex = (on && g_out_channels >= 2) ? 1 : 0; }

// Set the IOProc buffer size (frames), clamped to the device's range. Safe to
// call while the device is running (CoreAudio re-negotiates live). Returns the
// value actually in effect.
int m8_capture_set_block_frames(int frames) {
  if (g_dev == kAudioObjectUnknown || frames <= 0)
    return g_block_frames;
  UInt32 want = (UInt32)frames;
  AudioObjectPropertyAddress rng = {kAudioDevicePropertyBufferFrameSizeRange,
                                    kAudioObjectPropertyScopeInput,
                                    kAudioObjectPropertyElementMain};
  AudioValueRange range = {0, 0};
  UInt32 rsize = sizeof(range);
  if (AudioObjectGetPropertyData(g_dev, &rng, 0, NULL, &rsize, &range) == noErr) {
    if (want < (UInt32)range.mMinimum)
      want = (UInt32)range.mMinimum;
    if (want > (UInt32)range.mMaximum)
      want = (UInt32)range.mMaximum;
  }
  AudioObjectPropertyAddress bfs = {kAudioDevicePropertyBufferFrameSize,
                                    kAudioObjectPropertyScopeInput,
                                    kAudioObjectPropertyElementMain};
  AudioObjectSetPropertyData(g_dev, &bfs, 0, NULL, sizeof(want), &want);
  UInt32 got = 0, gsize = sizeof(got);
  if (AudioObjectGetPropertyData(g_dev, &bfs, 0, NULL, &gsize, &got) == noErr)
    g_block_frames = (int)got;
  else
    g_block_frames = (int)want;
  SDL_Log("m8_capture: IOProc buffer = %d frames (%.1f ms @ %d Hz)", g_block_frames,
          g_rate > 0 ? 1000.0 * g_block_frames / g_rate : 0.0, g_rate);
  return g_block_frames;
}

// Sum of input channels for a device (0 = not an input device).
static int device_input_channels(AudioObjectID dev) {
  AudioObjectPropertyAddress addr = {kAudioDevicePropertyStreamConfiguration,
                                     kAudioObjectPropertyScopeInput,
                                     kAudioObjectPropertyElementMain};
  UInt32 size = 0;
  if (AudioObjectGetPropertyDataSize(dev, &addr, 0, NULL, &size) != noErr || size == 0)
    return 0;
  AudioBufferList *bl = SDL_malloc(size);
  int total = 0;
  if (AudioObjectGetPropertyData(dev, &addr, 0, NULL, &size, bl) == noErr) {
    for (UInt32 i = 0; i < bl->mNumberBuffers; i++)
      total += bl->mBuffers[i].mNumberChannels;
  }
  SDL_free(bl);
  return total;
}

// Sum of output channels for a device (0 = no output / input-only).
static int device_output_channels(AudioObjectID dev) {
  AudioObjectPropertyAddress addr = {kAudioDevicePropertyStreamConfiguration,
                                     kAudioObjectPropertyScopeOutput,
                                     kAudioObjectPropertyElementMain};
  UInt32 size = 0;
  if (AudioObjectGetPropertyDataSize(dev, &addr, 0, NULL, &size) != noErr || size == 0)
    return 0;
  AudioBufferList *bl = SDL_malloc(size);
  int total = 0;
  if (AudioObjectGetPropertyData(dev, &addr, 0, NULL, &size, bl) == noErr) {
    for (UInt32 i = 0; i < bl->mNumberBuffers; i++)
      total += bl->mBuffers[i].mNumberChannels;
  }
  SDL_free(bl);
  return total;
}

static bool device_name(AudioObjectID dev, char *out, size_t out_size) {
  AudioObjectPropertyAddress addr = {kAudioObjectPropertyName, kAudioObjectPropertyScopeGlobal,
                                     kAudioObjectPropertyElementMain};
  CFStringRef name = NULL;
  UInt32 size = sizeof(name);
  if (AudioObjectGetPropertyData(dev, &addr, 0, NULL, &size, &name) != noErr || name == NULL)
    return false;
  const bool ok = CFStringGetCString(name, out, (CFIndex)out_size, kCFStringEncodingUTF8);
  CFRelease(name);
  return ok;
}

static AudioObjectID find_m8_input_device(void) {
  AudioObjectPropertyAddress addr = {kAudioHardwarePropertyDevices,
                                     kAudioObjectPropertyScopeGlobal,
                                     kAudioObjectPropertyElementMain};
  UInt32 size = 0;
  if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &addr, 0, NULL, &size) != noErr)
    return kAudioObjectUnknown;
  const int n = (int)(size / sizeof(AudioObjectID));
  AudioObjectID *devs = SDL_malloc(size);
  if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &addr, 0, NULL, &size, devs) != noErr) {
    SDL_free(devs);
    return kAudioObjectUnknown;
  }

  // Pick the M8 input device with the MOST channels: with two M8s connected
  // (e.g. a real M8 + a headless), the real M8 exposes 24 channels while a
  // headless typically exposes only stereo. We want the rich one for capture.
  // Log every candidate so the audio topology is visible.
  AudioObjectID best = kAudioObjectUnknown;
  int best_ch = 0;
  for (int i = 0; i < n; i++) {
    const int ch = device_input_channels(devs[i]);
    if (ch < 2)
      continue;
    char name[256] = {0};
    if (!device_name(devs[i], name, sizeof(name)))
      continue;
    if (SDL_strstr(name, "M8") == NULL)
      continue;
    SDL_Log("m8_capture: candidate input '%s' (%d ch)", name, ch);
    // Prefer more channels; on a tie prefer the exact name "M8".
    if (ch > best_ch || (ch == best_ch && SDL_strcmp(name, "M8") == 0)) {
      best = devs[i];
      best_ch = ch;
    }
  }
  SDL_free(devs);
  if (best != kAudioObjectUnknown)
    SDL_Log("m8_capture: selected M8 input with %d channels", best_ch);
  return best;
}

// Write `mon` (interleaved stereo, `frames` frames) into the device output
// buffer list, converting to the device format and zeroing any extra channels.
static void write_output(AudioBufferList *output, const float *mon, int frames) {
  const bool is_float = (g_out_asbd.mFormatFlags & kAudioFormatFlagIsFloat) != 0;
  const int bps = (int)(g_out_asbd.mBitsPerChannel / 8);
  const int oc = g_out_channels;
  if ((int)output->mNumberBuffers == oc) {
    // Non-interleaved: one buffer per channel.
    for (int c = 0; c < oc; c++) {
      void *dst = output->mBuffers[c].mData;
      if (dst == NULL)
        continue;
      for (int fr = 0; fr < frames; fr++) {
        const float s = (c < 2 && mon != NULL) ? mon[fr * 2 + c] : 0.0f;
        if (is_float)
          ((float *)dst)[fr] = s;
        else if (bps == 2)
          ((int16_t *)dst)[fr] = (int16_t)(s * 32767.0f);
        else
          ((int32_t *)dst)[fr] = (int32_t)(s * 2147483647.0f);
      }
    }
  } else {
    // Interleaved in buffer 0.
    void *dst = output->mBuffers[0].mData;
    if (dst == NULL)
      return;
    for (int fr = 0; fr < frames; fr++) {
      for (int c = 0; c < oc; c++) {
        const float s = (c < 2 && mon != NULL) ? mon[fr * 2 + c] : 0.0f;
        const int i = fr * oc + c;
        if (is_float)
          ((float *)dst)[i] = s;
        else if (bps == 2)
          ((int16_t *)dst)[i] = (int16_t)(s * 32767.0f);
        else
          ((int32_t *)dst)[i] = (int32_t)(s * 2147483647.0f);
      }
    }
  }
}

static OSStatus io_proc(AudioObjectID dev, const AudioTimeStamp *now,
                        const AudioBufferList *input, const AudioTimeStamp *intime,
                        AudioBufferList *output, const AudioTimeStamp *outtime, void *client) {
  (void)dev; (void)now; (void)intime; (void)outtime; (void)client;
  if (input == NULL || input->mNumberBuffers == 0 || g_cb == NULL)
    return noErr;

  const int ch = g_channels;
  const bool is_float = (g_asbd.mFormatFlags & kAudioFormatFlagIsFloat) != 0;
  const int bytes_per_sample = (int)(g_asbd.mBitsPerChannel / 8);

  // Determine frame count and interleave into S16 scratch.
  int frames;
  if ((int)input->mNumberBuffers == ch) {
    // Non-interleaved: one channel per buffer.
    frames = (int)(input->mBuffers[0].mDataByteSize / bytes_per_sample);
  } else {
    // Interleaved in buffer 0.
    frames = (int)(input->mBuffers[0].mDataByteSize / (bytes_per_sample * ch));
  }
  if (frames <= 0)
    return noErr;

  if (frames > g_scratch_frames) {
    g_scratch = SDL_realloc(g_scratch, (size_t)frames * ch * sizeof(float));
    g_scratch_frames = frames;
  }

  // Produce interleaved float32 [-1,1], converting from the device format.
  const float int16_scale = 1.0f / 32768.0f;
  const float int32_scale = 1.0f / 2147483648.0f;
  if ((int)input->mNumberBuffers == ch) {
    for (int c = 0; c < ch; c++) {
      const void *src = input->mBuffers[c].mData;
      for (int fr = 0; fr < frames; fr++) {
        float s;
        if (is_float)
          s = ((const float *)src)[fr];
        else if (bytes_per_sample == 2)
          s = ((const int16_t *)src)[fr] * int16_scale;
        else
          s = ((const int32_t *)src)[fr] * int32_scale;
        g_scratch[fr * ch + c] = s;
      }
    }
  } else {
    const void *src = input->mBuffers[0].mData;
    const int count = frames * ch;
    for (int i = 0; i < count; i++) {
      if (is_float)
        g_scratch[i] = ((const float *)src)[i];
      else if (bytes_per_sample == 2)
        g_scratch[i] = ((const int16_t *)src)[i] * int16_scale;
      else
        g_scratch[i] = ((const int32_t *)src)[i] * int32_scale;
    }
  }

  // Hand a stereo monitor scratch to the callback (it fills the wet master).
  if (frames > g_mon_frames) {
    g_mon = SDL_realloc(g_mon, (size_t)frames * 2 * sizeof(float));
    g_mon_frames = frames;
  }
  const int mon = g_cb(g_scratch, frames, ch, g_mon);

  // Duplex monitoring: write straight to this device's output (same clock).
  // In SDL mode (or no monitor), still fill the output with silence so the
  // device doesn't emit garbage on the channels this IOProc owns.
  if (output != NULL && output->mNumberBuffers > 0 && g_out_channels > 0) {
    const bool duplex = (g_monitor_duplex != 0) && (mon > 0) && (g_mon != NULL);
    write_output(output, duplex ? g_mon : NULL, frames);
  }
  return noErr;
}

bool m8_capture_start(m8_capture_cb cb) {
  g_cb = cb;
  g_dev = find_m8_input_device();
  if (g_dev == kAudioObjectUnknown) {
    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "m8_capture: M8 input device not found (CoreAudio)");
    return false;
  }

  AudioObjectPropertyAddress fmt = {kAudioDevicePropertyStreamFormat,
                                    kAudioObjectPropertyScopeInput,
                                    kAudioObjectPropertyElementMain};
  UInt32 size = sizeof(g_asbd);
  if (AudioObjectGetPropertyData(g_dev, &fmt, 0, NULL, &size, &g_asbd) != noErr) {
    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "m8_capture: cannot get input format");
    return false;
  }
  g_channels = (int)g_asbd.mChannelsPerFrame;
  g_rate = (int)g_asbd.mSampleRate;
  SDL_Log("m8_capture: M8 input %d ch, %d Hz, %d-bit %s%s", g_channels, g_rate,
          (int)g_asbd.mBitsPerChannel,
          (g_asbd.mFormatFlags & kAudioFormatFlagIsFloat) ? "float" : "int",
          (g_asbd.mFormatFlags & kAudioFormatFlagIsNonInterleaved) ? " (non-interleaved)"
                                                                   : " (interleaved)");

  // Detect a duplex output on the same device (for low-latency monitoring,
  // optional — SDL monitoring stays the default). 0 = input-only.
  g_out_channels = device_output_channels(g_dev);
  if (g_out_channels > 0) {
    AudioObjectPropertyAddress ofmt = {kAudioDevicePropertyStreamFormat,
                                       kAudioObjectPropertyScopeOutput,
                                       kAudioObjectPropertyElementMain};
    UInt32 osize = sizeof(g_out_asbd);
    if (AudioObjectGetPropertyData(g_dev, &ofmt, 0, NULL, &osize, &g_out_asbd) == noErr) {
      SDL_Log("m8_capture: M8 output %d ch, %d-bit %s%s (duplex monitoring available)",
              g_out_channels, (int)g_out_asbd.mBitsPerChannel,
              (g_out_asbd.mFormatFlags & kAudioFormatFlagIsFloat) ? "float" : "int",
              (g_out_asbd.mFormatFlags & kAudioFormatFlagIsNonInterleaved) ? " (non-interleaved)"
                                                                           : " (interleaved)");
    } else {
      g_out_channels = 0; // can't read format -> don't risk writing output
    }
  } else {
    SDL_Log("m8_capture: M8 device is input-only (no duplex; SDL monitoring only)");
  }

  // Pin the IOProc buffer size for stable, predictable processing deadlines.
  {
    UInt32 want = M8_CAPTURE_BLOCK_FRAMES;
    const char *env = SDL_getenv("M8C_BLOCK");
    if (env != NULL) {
      int v = SDL_atoi(env);
      if (v > 0)
        want = (UInt32)v;
    }
    m8_capture_set_block_frames((int)want); // clamps, sets, logs, updates g_block_frames
  }

  OSStatus err = AudioDeviceCreateIOProcID(g_dev, io_proc, NULL, &g_proc);
  if (err != noErr || g_proc == NULL) {
    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "m8_capture: CreateIOProcID failed (%d)", (int)err);
    return false;
  }
  err = AudioDeviceStart(g_dev, g_proc);
  if (err != noErr) {
    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "m8_capture: AudioDeviceStart failed (%d)", (int)err);
    AudioDeviceDestroyIOProcID(g_dev, g_proc);
    g_proc = NULL;
    return false;
  }
  return true;
}

void m8_capture_stop(void) {
  if (g_dev != kAudioObjectUnknown && g_proc != NULL) {
    AudioDeviceStop(g_dev, g_proc);
    AudioDeviceDestroyIOProcID(g_dev, g_proc);
  }
  g_proc = NULL;
  g_dev = kAudioObjectUnknown;
  if (g_scratch != NULL) {
    SDL_free(g_scratch);
    g_scratch = NULL;
    g_scratch_frames = 0;
  }
  if (g_mon != NULL) {
    SDL_free(g_mon);
    g_mon = NULL;
    g_mon_frames = 0;
  }
  g_out_channels = 0;
  g_monitor_duplex = 0;
  g_cb = NULL;
}

#else
// ---------------------------------------------------------------------------
// Stub (TODO: ALSA backend for Linux/TrimUI)
// ---------------------------------------------------------------------------
bool m8_capture_start(m8_capture_cb cb) {
  (void)cb;
  SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "m8_capture: multichannel capture not implemented");
  return false;
}
void m8_capture_stop(void) {}
bool m8_capture_has_output(void) { return false; }
void m8_capture_set_monitor_duplex(bool on) { (void)on; }
#endif
