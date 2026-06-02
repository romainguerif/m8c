// Copyright 2021 Jonne Kokkonen
// Released under the MIT licence, https://opensource.org/licenses/MIT

/* Uncomment this line to enable debug messages or call make with `make
   CFLAGS=-DDEBUG_MSG` */
// #define DEBUG_MSG

#define APP_VERSION "v2.2.4"

#include <SDL3/SDL.h>
#define SDL_MAIN_USE_CALLBACKS
#include <SDL3/SDL_main.h>
#include <stdlib.h>

#include "SDL2_inprint.h"
#include "backends/audio.h"
#include "backends/m8.h"
#include "backends/midi_cc.h"
#include "backends/recorder.h"
#include "juce_host.h"
#include "plugin_rack.h"
#include "common.h"
#include "config.h"
#include "gamepads.h"
#include "input.h"
#include "render.h"
#include "log_overlay.h"

static void do_wait_for_device(struct app_context *ctx) {
  static Uint64 ticks_poll_device = 0;
  static int screensaver_initialized = 0;

  // Handle app suspension
  if (ctx->app_suspended) {
    return;
  }

  if (!screensaver_initialized) {
    screensaver_initialized = screensaver_init();
  }
  screensaver_draw();
  render_screen(&ctx->conf);

  // Poll for M8 device every second
  if (ctx->device_connected == 0 && SDL_GetTicks() - ticks_poll_device > 1000) {
    ticks_poll_device = SDL_GetTicks();
    if (m8_initialize(0, ctx->preferred_device)) {

      // M8 connected: the recorder owns the M8 audio (24ch capture + master
      // monitor back to the M8). Then open its MIDI port for arm/transport.
      recorder_open_capture();
      midi_cc_open();

      // Dual-M8: a second M8 (e.g. a headless on another port) is opened too.
      renderer_set_device_count(m8_device_count());
      const int m8_enabled = m8_enable_display(0, 1);
      for (int d = 1; d < m8_device_count(); d++)
        m8_enable_display(d, 1);
      // Device was found; enable display and proceed to the main loop
      if (m8_enabled == 1) {
        ctx->app_state = RUN;
        ctx->device_connected = 1;
        SDL_Delay(100); // Give the display time to initialize
        screensaver_destroy();
        screensaver_initialized = 0;
        for (int d = 0; d < m8_device_count(); d++)
          m8_reset_display(d); // Avoid display glitches.
      } else {
        SDL_LogCritical(SDL_LOG_CATEGORY_ERROR, "Device not detected.");
        ctx->app_state = QUIT;
        screensaver_destroy();
        screensaver_initialized = 0;
#ifdef USE_RTMIDI
        show_error_message(
            "Cannot initialize M8 remote display. Make sure you're running "
            "firmware 6.0.0 or newer. Please close and restart the application to try again.");
#endif
      }
    }
  }
}

static config_params_s initialize_config(int argc, char *argv[], char **preferred_device,
                                         char **config_filename) {
  for (int i = 1; i < argc; i++) {
    if (SDL_strcmp(argv[i], "--list") == 0) {
      exit(m8_list_devices());
    }
    if (SDL_strcmp(argv[i], "--dev") == 0 && i + 1 < argc) {
      *preferred_device = argv[i + 1];
      SDL_Log("Using preferred device: %s", *preferred_device);
      i++;
    } else if (SDL_strcmp(argv[i], "--config") == 0 && i + 1 < argc) {
      *config_filename = argv[i + 1];
      SDL_Log("Using config file: %s", *config_filename);
      i++;
    }
  }

  config_params_s conf = config_initialize(*config_filename);

  if (TARGET_OS_IOS == 1) {
    // Predefined settings for iOS
    conf.init_fullscreen = 1;
  }
  config_read(&conf);

  return conf;
}

// Main callback loop - read inputs, process data from the device, render screen
SDL_AppResult SDL_AppIterate(void *appstate) {
  if (appstate == NULL) {
    return SDL_APP_FAILURE;
  }

  struct app_context *ctx = appstate;
  SDL_AppResult app_result = SDL_APP_CONTINUE;

  juce_host_pump();    // service the JUCE message loop (no-op on macOS for now)
  plugin_rack_tick();  // startup song auto-load + CC song recalls (main thread)

  // REC + transport indicator in the window title, updated on the main thread
  // only (recorder/transport are driven from the realtime/MIDI thread).
  {
    static int last_rec = -1, last_play = -1, last_bpm = -1;
    const int rec = recorder_is_recording() ? 1 : 0;
    const int play = juce_host_transport_playing();
    const int bpm = (int)(juce_host_bpm() + 0.5);
    if (rec != last_rec || play != last_play || bpm != last_bpm) {
      char title[64];
      SDL_snprintf(title, sizeof(title), "m8c%s%s %d BPM", rec ? "  \xE2\x97\x8F REC" : "",
                   play ? "  \xE2\x96\xB6" : "  \xE2\x96\xA0", bpm);
      renderer_set_title(title);
      last_rec = rec;
      last_play = play;
      last_bpm = bpm;
    }
  }

  switch (ctx->app_state) {
  case INITIALIZE:
    break;

  case WAIT_FOR_DEVICE:
    do_wait_for_device(ctx);
    break;

  case RUN: {
    // Handle app suspension
    if (ctx->app_suspended) {
      return SDL_APP_CONTINUE;
    }
    // Device 0 drives the app lifecycle (its M8 owns audio/MIDI/recorder).
    // Additional M8s (dual mode) are display + control only for now.
    renderer_set_focus(input_focused_device());
    renderer_set_active_device(0);
    const int result = m8_process_data(0, &ctx->conf);
    for (int d = 1; d < m8_device_count(); d++) {
      renderer_set_active_device(d);
      m8_process_data(d, &ctx->conf);
    }
    renderer_set_active_device(0);
    if (result == DEVICE_DISCONNECTED) {
      ctx->device_connected = 0;
      ctx->app_state = WAIT_FOR_DEVICE;
      m8_close(); // tear down all devices so polling can re-open cleanly
      renderer_set_device_count(0);
      recorder_close_capture(); // finalize WAVs if a take was running
      midi_cc_close();
      audio_close();
    } else if (result == DEVICE_FATAL_ERROR) {
      return SDL_APP_FAILURE;
    }
    render_screen(&ctx->conf);
    break;
  }

  case QUIT:
    app_result = SDL_APP_SUCCESS;
    break;
  }

  return app_result;
}

// Initialize the app: initialize context, configs, renderer controllers and attempt to find M8
SDL_AppResult SDL_AppInit(void **appstate, int argc, char **argv) {
  // If relaunched as the isolated plugin-scanner worker, run it and exit
  // before any UI/audio/serial is touched.
  if (juce_host_run_scanner_if_worker(argc, argv)) {
    return SDL_APP_SUCCESS;
  }

  SDL_SetAppMetadata("M8C",APP_VERSION,"fi.laamaa.m8c");

  char *config_filename = NULL;

  // Initialize in-app log capture/overlay
  log_overlay_init();

#ifndef NDEBUG
  // Show debug messages in the application log
  SDL_SetLogPriorities(SDL_LOG_PRIORITY_DEBUG);
  SDL_LogDebug(SDL_LOG_CATEGORY_TEST, "Running a Debug build");
#else
  // Show debug messages in the application log
  SDL_SetLogPriorities(SDL_LOG_PRIORITY_INFO);
#endif

  // Process the application's main callback roughly at 120 Hz
  SDL_SetHint(SDL_HINT_MAIN_CALLBACK_RATE, "120");

  struct app_context *ctx = SDL_calloc(1, sizeof(struct app_context));
  if (ctx == NULL) {
    SDL_LogCritical(SDL_LOG_CATEGORY_SYSTEM, "SDL_calloc failed: %s", SDL_GetError());
    return SDL_APP_FAILURE;
  }

  *appstate = ctx;
  ctx->app_state = INITIALIZE;
  ctx->conf = initialize_config(argc, argv, &ctx->preferred_device, &config_filename);

  // Load multitrack recorder configuration (config/recorder.ini + env).
  recorder_init();

  if (!renderer_initialize(&ctx->conf)) {
    SDL_LogCritical(SDL_LOG_CATEGORY_ERROR, "Failed to initialize renderer.");
    return SDL_APP_FAILURE;
  }

  // Bring up the embedded JUCE host now that SDL/NSApp exists (main thread).
  juce_host_init();

  ctx->device_connected =
      m8_initialize(1, ctx->preferred_device);

  if (gamepads_initialize() < 0) {
    SDL_LogCritical(SDL_LOG_CATEGORY_ERROR, "Failed to initialize game controllers.");
    return SDL_APP_FAILURE;
  }

  renderer_set_device_count(m8_device_count());
  if (ctx->device_connected && m8_enable_display(0, 1)) {
    for (int d = 1; d < m8_device_count(); d++)
      m8_enable_display(d, 1);
    recorder_open_capture();
    midi_cc_open();
    ctx->app_state = RUN;
    render_screen(&ctx->conf);
  } else {
    SDL_LogCritical(SDL_LOG_CATEGORY_ERROR, "Device not detected.");
    ctx->device_connected = 0;
    ctx->app_state = WAIT_FOR_DEVICE;
  }

  return SDL_APP_CONTINUE;
}

void SDL_AppQuit(void *appstate, SDL_AppResult result) {
  (void)result; // Suppress compiler warning

  struct app_context *app = appstate;

  if (app) {
    if (app->app_state == WAIT_FOR_DEVICE) {
      screensaver_destroy();
    }
    plugin_rack_shutdown(); // save current song while the host state is intact
    recorder_shutdown();
    midi_cc_close();
    juce_host_shutdown();
    if (app->conf.audio_enabled) {
      audio_close();
    }
    gamepads_close();
    renderer_close();
    inline_font_close();
    if (app->device_connected) {
      m8_close();
    }
    SDL_free(app);

    SDL_Log("Shutting down.");
    SDL_Quit();
  }
}