// Multitrack recorder for the M8 24-channel USB audio stream.
// Released under the MIT licence, https://opensource.org/licenses/MIT
#include "recorder.h"

#include "../ini.h"
#include "juce_host.h"
#include "m8_audio_capture.h"
#include "ringbuffer.h"
#include "wav_writer.h"

#include <SDL3/SDL.h>

#define MON_MAX_FRAMES 16384 // monitor scratch ceiling (frames)

// ---------------------------------------------------------------------------
// Tunables
// ---------------------------------------------------------------------------
#define REC_BITS 24                  // M8 multichannel USB audio is 24-bit
#define REC_MAX_FILES 16             // up to 32 channels paired as stereo
#define DISK_CHUNK_FRAMES 4096       // frames drained per disk-thread pass
#define RING_SECONDS 4               // ring buffer headroom
#define SILENCE_PEAK 0.0001f         // tracks under this peak are dropped (~-80 dBFS)

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------
typedef struct {
  char output_dir[1024];
  char session_name[256]; // optional take name; folder = name_timestamp
  int arm_cc;
  int arm_channel; // 0 = any, else 1-16
  bool monitor_enabled;
  char monitor_device[256];
  bool drop_silent; // delete silent tracks on stop
  unsigned int record_key; // SDL scancode
} recorder_config_s;

static recorder_config_s cfg;

// ---------------------------------------------------------------------------
// Capture state (lives while the M8 is connected)
// ---------------------------------------------------------------------------
// The 24-channel input is captured via CoreAudio (m8_audio_capture); SDL is
// only used for the stereo master monitor output (SDL caps at 8 channels).
static SDL_AudioStream *monitor_stream = NULL; // master (ch 1-2) -> output
static bool capture_ready = false;
static int channels = 0;       // M8 capture channels (24)
static int rec_channels = 0;   // channels + 2 (extra stereo = processed master)
static int sample_rate = 44100;
static size_t frame_bytes = 0; // rec_channels * sizeof(float) in the ring

static int16_t *monitor_tmp = NULL; // capture-thread scratch (stereo S16)
static float *host_out = NULL;      // capture-thread scratch (stereo float, wet master)
static float *host_sends = NULL;    // capture-thread scratch (3 sends interleaved = 6 ch)
static float *cap_combined = NULL;  // capture-thread scratch (rec_channels interleaved)

// ---------------------------------------------------------------------------
// Record-session state (lives while recording)
// ---------------------------------------------------------------------------
static RingBuffer *rb = NULL;
static SDL_Mutex *rb_lock = NULL;
static SDL_Thread *disk_thread = NULL;
static SDL_AtomicInt recording;      // 1 while writing to disk
static SDL_AtomicInt thread_running; // 1 while disk thread loops

static wav_writer_s *writers[REC_MAX_FILES];
static int num_files = 0;
static uint8_t *disk_chunk = NULL;        // popped interleaved float frames
static uint8_t *file_scratch[REC_MAX_FILES]; // packed 24-bit stereo per file
static char file_names[REC_MAX_FILES][64];   // for silent-track cleanup
static float file_peak[REC_MAX_FILES];       // max abs sample per file
static char session_dir[1200];

// ---------------------------------------------------------------------------
// Trigger state machine: record  <=>  armed && playing  (or manual override)
// ---------------------------------------------------------------------------
static bool st_armed = false;
static bool st_playing = false;
static bool st_manual = false;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static void expand_tilde(const char *in, char *out, size_t out_size) {
  if (in[0] == '~' && (in[1] == '/' || in[1] == '\0')) {
    const char *home = SDL_getenv("HOME");
    SDL_snprintf(out, out_size, "%s%s", home ? home : "", in + 1);
  } else {
    SDL_snprintf(out, out_size, "%s", in);
  }
}

static bool mkpath(const char *path) {
  char tmp[1300];
  SDL_snprintf(tmp, sizeof(tmp), "%s", path);
  const size_t len = SDL_strlen(tmp);
  for (size_t i = 1; i < len; i++) {
    if (tmp[i] == '/') {
      tmp[i] = '\0';
      if (tmp[0] != '\0')
        SDL_CreateDirectory(tmp);
      tmp[i] = '/';
    }
  }
  return SDL_CreateDirectory(tmp);
}

static void track_filename(int idx, int total_files, char *out, size_t out_size) {
  // 24-channel M8 layout: 12 dry stems + processed master + 3 processed sends.
  if (total_files == 16) {
    switch (idx) {
    case 0: SDL_snprintf(out, out_size, "01_master.wav"); return;
    case 9: SDL_snprintf(out, out_size, "10_modfx.wav"); return;
    case 10: SDL_snprintf(out, out_size, "11_delay.wav"); return;
    case 11: SDL_snprintf(out, out_size, "12_reverb.wav"); return;
    case 12: SDL_snprintf(out, out_size, "13_master_wet.wav"); return;
    case 13: SDL_snprintf(out, out_size, "14_send1_wet.wav"); return;
    case 14: SDL_snprintf(out, out_size, "15_send2_wet.wav"); return;
    case 15: SDL_snprintf(out, out_size, "16_send3_wet.wav"); return;
    default: SDL_snprintf(out, out_size, "%02d_track%d.wav", idx + 1, idx); return; // 1-8
    }
  }
  SDL_snprintf(out, out_size, "%02d_pair%d.wav", idx + 1, idx + 1);
}

static SDL_AudioDeviceID find_device(bool recording_dev, const char *match) {
  int count = 0;
  SDL_AudioDeviceID *devs =
      recording_dev ? SDL_GetAudioRecordingDevices(&count) : SDL_GetAudioPlaybackDevices(&count);
  if (devs == NULL)
    return 0;
  SDL_AudioDeviceID exact = 0, substr = 0;
  for (int i = 0; i < count; i++) {
    const char *name = SDL_GetAudioDeviceName(devs[i]);
    if (name == NULL)
      continue;
    if (SDL_strcmp(name, match) == 0 && exact == 0)
      exact = devs[i];
    else if (SDL_strstr(name, match) != NULL && substr == 0)
      substr = devs[i];
  }
  SDL_free(devs);
  return exact ? exact : substr;
}

static inline int16_t f32_to_s16(float v) {
  if (v > 1.0f) v = 1.0f;
  if (v < -1.0f) v = -1.0f;
  return (int16_t)(v * 32767.0f);
}

// ---------------------------------------------------------------------------
// Capture callback (CoreAudio IO thread): feed monitor + disk ring.
// `src` is interleaved float32 [-1,1], `frames` frames of `ch` channels.
// ---------------------------------------------------------------------------
static void recorder_on_frames(const float *src, int frames, int ch) {
  if (!capture_ready || frames <= 0)
    return;
  const int nf = frames > MON_MAX_FRAMES ? MON_MAX_FRAMES : frames;

  // Compute the processed (wet) master: 3 sends + master chain through the
  // plugin host. Falls back to the M8 master pair (ch 0,1) when inactive.
  const bool host = juce_host_is_active();
  if (host) {
    juce_host_process_full(src, ch, nf, host_out, host_sends); // master + 3 send wets
  } else {
    for (int fr = 0; fr < nf; fr++) {
      const float *f = src + (size_t)fr * ch;
      host_out[2 * fr] = f[0];
      host_out[2 * fr + 1] = f[1];
    }
    for (int i = 0; i < nf * 2 * JUCE_HOST_NUM_SENDS; i++)
      host_sends[i] = 0.0f;
  }

  // Master monitor: send the wet master back to the M8.
  if (monitor_stream != NULL) {
    for (int i = 0; i < nf * 2; i++)
      monitor_tmp[i] = f32_to_s16(host_out[i]);
    SDL_PutAudioStreamData(monitor_stream, monitor_tmp, nf * 2 * (int)sizeof(int16_t));
  }

  // Disk: record the raw 24 channels + the wet master (2 extra channels).
  if (SDL_GetAtomicInt(&recording)) {
    const int ns = 2 * JUCE_HOST_NUM_SENDS;
    for (int fr = 0; fr < nf; fr++) {
      float *dst = cap_combined + (size_t)fr * rec_channels;
      const float *s = src + (size_t)fr * ch;
      for (int c = 0; c < ch; c++)
        dst[c] = s[c];
      dst[ch] = host_out[2 * fr];         // master wet L
      dst[ch + 1] = host_out[2 * fr + 1]; // master wet R
      for (int k = 0; k < ns; k++)        // send1/2/3 wet L/R
        dst[ch + 2 + k] = host_sends[fr * ns + k];
    }
    const uint32_t bytes = (uint32_t)nf * (uint32_t)frame_bytes;
    SDL_LockMutex(rb_lock);
    if (rb != NULL) {
      const uint32_t pushed = ring_buffer_push(rb, (const uint8_t *)cap_combined, bytes);
      if (pushed == (uint32_t)-1 || pushed < bytes)
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "recorder: ring overflow (disk too slow?)");
    }
    SDL_UnlockMutex(rb_lock);
  }
}

// ---------------------------------------------------------------------------
// Disk thread: pop whole frames, deinterleave to per-file stereo, write.
// ---------------------------------------------------------------------------
static int disk_thread_fn(void *data) {
  (void)data;
  const uint32_t chunk_bytes = (uint32_t)(DISK_CHUNK_FRAMES * frame_bytes);

  while (SDL_GetAtomicInt(&thread_running) || (rb && rb->size > 0)) {
    SDL_LockMutex(rb_lock);
    uint32_t avail = rb ? rb->size : 0;
    uint32_t want = avail < chunk_bytes ? avail : chunk_bytes;
    want -= (uint32_t)(want % frame_bytes); // keep frame-aligned
    uint32_t popped = 0;
    if (want > 0)
      popped = ring_buffer_pop(rb, disk_chunk, want);
    SDL_UnlockMutex(rb_lock);

    if (popped == 0 || popped == (uint32_t)-1) {
      SDL_Delay(2);
      continue;
    }

    const int frames = (int)(popped / frame_bytes);
    const float *src = (const float *)disk_chunk;
    for (int f = 0; f < num_files; f++) {
      uint8_t *dst = file_scratch[f]; // packed 24-bit LE, stereo
      const int c0 = 2 * f, c1 = 2 * f + 1;
      float peak = file_peak[f];
      size_t o = 0;
      for (int fr = 0; fr < frames; fr++) {
        const float *frame = src + (size_t)fr * rec_channels;
        for (int lr = 0; lr < 2; lr++) {
          float v = frame[lr == 0 ? c0 : c1];
          float a = v < 0 ? -v : v;
          if (a > peak) peak = a;
          if (v > 1.0f) v = 1.0f;
          if (v < -1.0f) v = -1.0f;
          const int32_t s = (int32_t)(v * 8388607.0f); // 2^23 - 1
          dst[o++] = (uint8_t)(s & 0xFF);
          dst[o++] = (uint8_t)((s >> 8) & 0xFF);
          dst[o++] = (uint8_t)((s >> 16) & 0xFF);
        }
      }
      file_peak[f] = peak;
      wav_writer_write(writers[f], dst, (size_t)frames * 2 * 3);
    }
  }
  return 0;
}

// ---------------------------------------------------------------------------
// Record session begin / end (capture stream is already running).
// ---------------------------------------------------------------------------
static bool rec_begin(void) {
  if (SDL_GetAtomicInt(&recording) || !capture_ready)
    return SDL_GetAtomicInt(&recording);

  num_files = rec_channels / 2; // 12 M8 stems + 1 processed master
  if (num_files > REC_MAX_FILES)
    num_files = REC_MAX_FILES;

  char base[1024];
  expand_tilde(cfg.output_dir, base, sizeof(base));
  SDL_DateTime dt;
  SDL_Time now;
  char stamp[32];
  if (SDL_GetCurrentTime(&now) && SDL_TimeToDateTime(now, &dt, true))
    SDL_snprintf(stamp, sizeof(stamp), "%04d-%02d-%02d_%02d%02d%02d", dt.year, dt.month, dt.day,
                 dt.hour, dt.minute, dt.second);
  else
    SDL_snprintf(stamp, sizeof(stamp), "take");
  if (cfg.session_name[0] != '\0')
    SDL_snprintf(session_dir, sizeof(session_dir), "%s/%s_%s", base, cfg.session_name, stamp);
  else
    SDL_snprintf(session_dir, sizeof(session_dir), "%s/%s", base, stamp);
  if (!mkpath(session_dir)) {
    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "recorder: cannot create %s", session_dir);
    return false;
  }

  for (int f = 0; f < num_files; f++) {
    char path[1300];
    track_filename(f, num_files, file_names[f], sizeof(file_names[f]));
    file_peak[f] = 0.0f;
    SDL_snprintf(path, sizeof(path), "%s/%s", session_dir, file_names[f]);
    writers[f] = wav_writer_open(path, 2, (uint32_t)sample_rate, REC_BITS);
    if (writers[f] == NULL) {
      for (int g = 0; g < f; g++) {
        wav_writer_close(writers[g]);
        writers[g] = NULL;
      }
      return false;
    }
    file_scratch[f] = SDL_malloc((size_t)DISK_CHUNK_FRAMES * 2 * 3); // 24-bit stereo
  }

  disk_chunk = SDL_malloc((size_t)DISK_CHUNK_FRAMES * frame_bytes);
  rb = ring_buffer_create((uint32_t)(RING_SECONDS * sample_rate * frame_bytes));

  SDL_SetAtomicInt(&thread_running, 1);
  SDL_SetAtomicInt(&recording, 1);
  disk_thread = SDL_CreateThread(disk_thread_fn, "m8c_recorder_disk", NULL);

  // NOTE: do not touch the SDL window here — rec_begin runs on the realtime /
  // MIDI thread. The REC window-title indicator is updated from the main loop.
  SDL_Log("recorder: REC started -> %s (%d ch, %d Hz, %d-bit, %d files)", session_dir, channels,
          sample_rate, REC_BITS, num_files);
  return true;
}

static void rec_end(void) {
  if (!SDL_GetAtomicInt(&recording))
    return;

  SDL_SetAtomicInt(&recording, 0); // capture_cb stops pushing

  if (disk_thread != NULL) {
    SDL_SetAtomicInt(&thread_running, 0);
    SDL_WaitThread(disk_thread, NULL); // drains remaining ring
    disk_thread = NULL;
  }

  // Free ring under lock so a racing capture_cb sees rb == NULL.
  SDL_LockMutex(rb_lock);
  if (rb != NULL) {
    ring_buffer_free(rb);
    rb = NULL;
  }
  SDL_UnlockMutex(rb_lock);

  int kept = 0, dropped = 0;
  for (int f = 0; f < num_files; f++) {
    if (writers[f] != NULL) {
      wav_writer_close(writers[f]);
      writers[f] = NULL;
    }
    // Drop tracks that recorded only silence.
    if (cfg.drop_silent && file_peak[f] < SILENCE_PEAK) {
      char path[1300];
      SDL_snprintf(path, sizeof(path), "%s/%s", session_dir, file_names[f]);
      SDL_RemovePath(path);
      dropped++;
    } else {
      kept++;
    }
    if (file_scratch[f] != NULL) {
      SDL_free(file_scratch[f]);
      file_scratch[f] = NULL;
    }
  }
  num_files = 0;
  if (disk_chunk != NULL) {
    SDL_free(disk_chunk);
    disk_chunk = NULL;
  }
  SDL_Log("recorder: REC stopped -> %s (%d tracks kept, %d silent dropped)", session_dir, kept,
          dropped);
}

static void reconcile(void) {
  const bool want = st_manual || (st_armed && st_playing);
  const bool is_rec = SDL_GetAtomicInt(&recording) != 0;
  if (want && !is_rec)
    rec_begin();
  else if (!want && is_rec)
    rec_end();
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
void recorder_init(void) {
  SDL_snprintf(cfg.output_dir, sizeof(cfg.output_dir), "%s", "~/Music/m8c-recordings");
  cfg.session_name[0] = '\0';
  cfg.arm_cc = 100;
  cfg.arm_channel = 0;
  cfg.monitor_enabled = true;
  SDL_snprintf(cfg.monitor_device, sizeof(cfg.monitor_device), "%s", "M8");
  cfg.drop_silent = true;
  cfg.record_key = SDL_SCANCODE_F8;

  const char *cfg_path = SDL_getenv("M8C_RECORDER_CONFIG");
  if (cfg_path == NULL)
    cfg_path = "config/recorder.ini";
  ini_t *ini = ini_load(cfg_path);
  if (ini != NULL) {
    const char *v;
    if ((v = ini_get(ini, "recorder", "output_dir")) != NULL)
      SDL_snprintf(cfg.output_dir, sizeof(cfg.output_dir), "%s", v);
    if ((v = ini_get(ini, "recorder", "session_name")) != NULL)
      SDL_snprintf(cfg.session_name, sizeof(cfg.session_name), "%s", v);
    if ((v = ini_get(ini, "recorder", "drop_silent")) != NULL)
      cfg.drop_silent = (SDL_strcasecmp(v, "true") == 0 || SDL_strcmp(v, "1") == 0);
    if ((v = ini_get(ini, "recorder", "arm_cc")) != NULL)
      cfg.arm_cc = SDL_atoi(v);
    if ((v = ini_get(ini, "recorder", "arm_channel")) != NULL)
      cfg.arm_channel = SDL_atoi(v);
    if ((v = ini_get(ini, "recorder", "monitor_enabled")) != NULL)
      cfg.monitor_enabled = (SDL_strcasecmp(v, "true") == 0 || SDL_strcmp(v, "1") == 0);
    if ((v = ini_get(ini, "recorder", "monitor_output_device")) != NULL)
      SDL_snprintf(cfg.monitor_device, sizeof(cfg.monitor_device), "%s", v);
    if ((v = ini_get(ini, "recorder", "record_key")) != NULL)
      cfg.record_key = (unsigned int)SDL_atoi(v);
    ini_free(ini);
  } else {
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "recorder: no config at %s, using defaults",
                cfg_path);
  }

  const char *e;
  if ((e = SDL_getenv("M8C_REC_DIR")) != NULL)
    SDL_snprintf(cfg.output_dir, sizeof(cfg.output_dir), "%s", e);
  if ((e = SDL_getenv("M8C_REC_NAME")) != NULL)
    SDL_snprintf(cfg.session_name, sizeof(cfg.session_name), "%s", e);
  if ((e = SDL_getenv("M8C_REC_DROP_SILENT")) != NULL)
    cfg.drop_silent = (SDL_strcasecmp(e, "true") == 0 || SDL_strcmp(e, "1") == 0);
  if ((e = SDL_getenv("M8C_REC_CC")) != NULL)
    cfg.arm_cc = SDL_atoi(e);
  if ((e = SDL_getenv("M8C_REC_CHANNEL")) != NULL)
    cfg.arm_channel = SDL_atoi(e);
  if ((e = SDL_getenv("M8C_REC_MONITOR")) != NULL)
    cfg.monitor_enabled = (SDL_strcasecmp(e, "true") == 0 || SDL_strcmp(e, "1") == 0);
  if ((e = SDL_getenv("M8C_REC_MONITOR_DEVICE")) != NULL)
    SDL_snprintf(cfg.monitor_device, sizeof(cfg.monitor_device), "%s", e);
  if ((e = SDL_getenv("M8C_REC_KEY")) != NULL)
    cfg.record_key = (unsigned int)SDL_atoi(e);

  SDL_AtomicInt zero = {0};
  recording = zero;
  thread_running = zero;

  SDL_Log("recorder: arm CC %d (channel %s), monitor %s, output %s", cfg.arm_cc,
          cfg.arm_channel == 0 ? "any" : "fixed", cfg.monitor_enabled ? cfg.monitor_device : "off",
          cfg.output_dir);
}

bool recorder_open_capture(void) {
  if (capture_ready)
    return true;

  // Start the multichannel CoreAudio capture (delivers interleaved float32).
  if (!m8_capture_start(recorder_on_frames)) {
    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "recorder: could not start M8 capture");
    return false;
  }
  channels = m8_capture_channels();
  sample_rate = m8_capture_rate();
  if (channels < 2) {
    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "recorder: capture has only %d channel(s)", channels);
    m8_capture_stop();
    return false;
  }
  // Record: 24 captured channels + processed master (2) + 3 send wets (6).
  rec_channels = channels + 2 + 2 * JUCE_HOST_NUM_SENDS;
  // The ring carries float32 frames of rec_channels (converted to 24-bit on disk).
  frame_bytes = (size_t)rec_channels * sizeof(float);

  monitor_tmp = SDL_malloc((size_t)MON_MAX_FRAMES * 2 * sizeof(int16_t));
  host_out = SDL_malloc((size_t)MON_MAX_FRAMES * 2 * sizeof(float));
  host_sends = SDL_malloc((size_t)MON_MAX_FRAMES * 2 * JUCE_HOST_NUM_SENDS * sizeof(float));
  cap_combined = SDL_malloc((size_t)MON_MAX_FRAMES * rec_channels * sizeof(float));
  rb_lock = SDL_CreateMutex();

  // Prepare the plugin host at the capture rate (max block headroom for HAL).
  juce_host_prepare((double)sample_rate, MON_MAX_FRAMES);

  // Master monitor output via SDL (stereo is well within SDL's 8-ch limit).
  if (cfg.monitor_enabled) {
    if (!SDL_WasInit(SDL_INIT_AUDIO))
      SDL_InitSubSystem(SDL_INIT_AUDIO);
    SDL_AudioDeviceID out_dev = find_device(false, cfg.monitor_device);
    if (out_dev == 0)
      out_dev = SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK;
    SDL_AudioSpec mspec = {SDL_AUDIO_S16, 2, sample_rate};
    monitor_stream = SDL_OpenAudioDeviceStream(out_dev, &mspec, NULL, NULL);
    if (monitor_stream != NULL)
      SDL_ResumeAudioStreamDevice(monitor_stream);
    else
      SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "recorder: monitor output unavailable: %s",
                  SDL_GetError());
  }

  capture_ready = true;
  SDL_Log("recorder: capture open (%d ch @ %d Hz), monitor %s", channels, sample_rate,
          monitor_stream ? "on" : "off");
  return true;
}

void recorder_close_capture(void) {
  capture_ready = false;
  rec_end();
  m8_capture_stop();
  if (monitor_stream != NULL) {
    SDL_DestroyAudioStream(monitor_stream);
    monitor_stream = NULL;
  }
  if (rb_lock != NULL) {
    SDL_DestroyMutex(rb_lock);
    rb_lock = NULL;
  }
  if (monitor_tmp != NULL) {
    SDL_free(monitor_tmp);
    monitor_tmp = NULL;
  }
  if (host_out != NULL) {
    SDL_free(host_out);
    host_out = NULL;
  }
  if (host_sends != NULL) {
    SDL_free(host_sends);
    host_sends = NULL;
  }
  if (cap_combined != NULL) {
    SDL_free(cap_combined);
    cap_combined = NULL;
  }
  st_armed = st_playing = st_manual = false;
}

void recorder_shutdown(void) { recorder_close_capture(); }

bool recorder_is_recording(void) { return SDL_GetAtomicInt(&recording) != 0; }

void recorder_set_armed(bool armed) {
  if (armed != st_armed) {
    if (armed && !st_playing)
      SDL_Log("recorder: ARMED, waiting for PLAY");
    else
      SDL_Log("recorder: %s", armed ? "ARMED" : "disarmed");
  }
  st_armed = armed;
  reconcile();
}

void recorder_set_playing(bool playing) {
  st_playing = playing;
  reconcile();
}

void recorder_toggle_manual(void) {
  st_manual = !st_manual;
  SDL_Log("recorder: manual %s", st_manual ? "ON" : "OFF");
  reconcile();
}

int recorder_arm_cc(void) { return cfg.arm_cc; }
int recorder_arm_channel(void) { return cfg.arm_channel; }
unsigned int recorder_record_key_scancode(void) { return cfg.record_key; }
