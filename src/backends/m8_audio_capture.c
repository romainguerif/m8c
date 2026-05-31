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
static AudioStreamBasicDescription g_asbd;
static float *g_scratch = NULL;     // interleaved float32 output
static int g_scratch_frames = 0;

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

  AudioObjectID exact = kAudioObjectUnknown, substr = kAudioObjectUnknown;
  for (int i = 0; i < n; i++) {
    if (device_input_channels(devs[i]) < 2)
      continue;
    char name[256] = {0};
    if (!device_name(devs[i], name, sizeof(name)))
      continue;
    if (SDL_strcmp(name, "M8") == 0 && exact == kAudioObjectUnknown)
      exact = devs[i];
    else if (SDL_strstr(name, "M8") != NULL && substr == kAudioObjectUnknown)
      substr = devs[i];
  }
  SDL_free(devs);
  return exact != kAudioObjectUnknown ? exact : substr;
}

static OSStatus io_proc(AudioObjectID dev, const AudioTimeStamp *now,
                        const AudioBufferList *input, const AudioTimeStamp *intime,
                        AudioBufferList *output, const AudioTimeStamp *outtime, void *client) {
  (void)dev; (void)now; (void)intime; (void)output; (void)outtime; (void)client;
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

  g_cb(g_scratch, frames, ch);
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
#endif
