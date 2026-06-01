// Standalone probe: how does SDL3 see and open the M8 24-channel input?
// Build: see tools/build_probe.sh
#include <SDL3/SDL.h>

static const char *fmt_name(SDL_AudioFormat f) {
  switch (f) {
  case SDL_AUDIO_S16LE: return "S16LE";
  case SDL_AUDIO_S32LE: return "S32LE";
  case SDL_AUDIO_F32LE: return "F32LE";
  case SDL_AUDIO_U8:    return "U8";
  case SDL_AUDIO_S8:    return "S8";
  default:             return "?";
  }
}

int main(void) {
  if (!SDL_Init(SDL_INIT_AUDIO)) {
    SDL_Log("init failed: %s", SDL_GetError());
    return 1;
  }

  int n = 0;
  SDL_AudioDeviceID *devs = SDL_GetAudioRecordingDevices(&n);
  SDL_AudioDeviceID m8 = 0;
  SDL_Log("--- recording devices (%d) ---", n);
  for (int i = 0; i < n; i++) {
    const char *name = SDL_GetAudioDeviceName(devs[i]);
    SDL_AudioSpec s;
    int frames = 0;
    SDL_GetAudioDeviceFormat(devs[i], &s, &frames);
    SDL_Log("  [%d] '%s'  fmt=%s ch=%d rate=%d frames=%d", i, name ? name : "(null)",
            fmt_name(s.format), s.channels, s.freq, frames);
    if (name && SDL_strcmp(name, "M8") == 0)
      m8 = devs[i];
  }
  if (m8 == 0 && n > 0) {
    for (int i = 0; i < n; i++) {
      const char *name = SDL_GetAudioDeviceName(devs[i]);
      if (name && SDL_strstr(name, "M8")) { m8 = devs[i]; break; }
    }
  }
  SDL_free(devs);

  if (m8 == 0) { SDL_Log("No M8 device."); return 2; }

  // Attempt 1: explicit S16, 24 ch
  SDL_AudioSpec want = {SDL_AUDIO_S16, 24, 44100};
  SDL_AudioStream *st = SDL_OpenAudioDeviceStream(m8, &want, NULL, NULL);
  SDL_Log("open S16/24ch: %s (%s)", st ? "OK" : "FAIL", st ? "" : SDL_GetError());
  if (st) SDL_DestroyAudioStream(st);

  // Attempt 2: NULL spec (device native format, no conversion)
  st = SDL_OpenAudioDeviceStream(m8, NULL, NULL, NULL);
  if (st) {
    SDL_AudioSpec src, dst;
    SDL_GetAudioStreamFormat(st, &src, &dst);
    SDL_Log("open NULL: OK  src(device)=%s/%dch/%d  dst(stream)=%s/%dch/%d", fmt_name(src.format),
            src.channels, src.freq, fmt_name(dst.format), dst.channels, dst.freq);
    SDL_DestroyAudioStream(st);
  } else {
    SDL_Log("open NULL: FAIL (%s)", SDL_GetError());
  }

  // Attempt 3: device-native format but force S16 sample type, same ch count
  SDL_AudioSpec dvs; int fr;
  if (SDL_GetAudioDeviceFormat(m8, &dvs, &fr)) {
    SDL_AudioSpec w2 = {SDL_AUDIO_S16, dvs.channels, dvs.freq};
    st = SDL_OpenAudioDeviceStream(m8, &w2, NULL, NULL);
    SDL_Log("open S16/%dch (native ch): %s (%s)", dvs.channels, st ? "OK" : "FAIL",
            st ? "" : SDL_GetError());
    if (st) SDL_DestroyAudioStream(st);
  }

  // Attempt 4: probe channel-count ceiling with explicit specs
  for (int ch = 2; ch <= 24; ch += 2) {
    SDL_AudioSpec w = {SDL_AUDIO_S16, ch, 44100};
    SDL_AudioStream *s = SDL_OpenAudioDeviceStream(m8, &w, NULL, NULL);
    SDL_Log("  open S16/%2dch: %s%s", ch, s ? "OK" : "FAIL", s ? "" : SDL_GetError());
    if (s) SDL_DestroyAudioStream(s);
  }

  SDL_Quit();
  return 0;
}
