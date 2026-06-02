#include "events.h"
#include "backends/m8.h"
#include "backends/m8_audio_capture.h"
#include "backends/recorder.h"
#include "common.h"
#include "gamepads.h"
#include "input.h"
#include "plugin_rack.h"
#include "render.h"
#include "settings.h"
#include <SDL3/SDL.h>
#include <SDL3/SDL_events.h>

SDL_AppResult SDL_AppEvent(void *appstate, SDL_Event *event) {
  struct app_context *ctx = appstate;
  SDL_AppResult ret_val = SDL_APP_CONTINUE;

  switch (event->type) {

  // --- System events ---
  case SDL_EVENT_QUIT:
  case SDL_EVENT_TERMINATING:
    ret_val = SDL_APP_SUCCESS;
    break;
  case SDL_EVENT_WINDOW_RESIZED:
  case SDL_EVENT_WINDOW_MOVED:
    // If the window size is changed, some systems might need a little nudge to fix scaling
    renderer_fix_texture_scaling_after_window_resize(&ctx->conf);
    break;

  // --- iOS specific events ---
  case SDL_EVENT_DID_ENTER_BACKGROUND:
    // iOS: Application entered into the background on iOS. About 5 seconds to stop things.
    SDL_LogDebug(SDL_LOG_CATEGORY_SYSTEM, "Received SDL_EVENT_DID_ENTER_BACKGROUND");
    ctx->app_suspended = 1;
    if (ctx->device_connected)
      m8_pause_processing();
    break;
  case SDL_EVENT_WILL_ENTER_BACKGROUND:
    // iOS: App about to enter into the background
    break;
  case SDL_EVENT_WILL_ENTER_FOREGROUND:
    // iOS: App returning to the foreground
    SDL_LogDebug(SDL_LOG_CATEGORY_SYSTEM, "Received SDL_EVENT_WILL_ENTER_FOREGROUND");
    break;
  case SDL_EVENT_DID_ENTER_FOREGROUND:
    // iOS: App becomes interactive again
    SDL_LogDebug(SDL_LOG_CATEGORY_SYSTEM, "Received SDL_EVENT_DID_ENTER_FOREGROUND");
    ctx->app_suspended = 0;
    if (ctx->device_connected) {
      renderer_clear_screen();
      m8_resume_processing();
    }
    break;

  // --- Input events ---
  case SDL_EVENT_GAMEPAD_ADDED:
  case SDL_EVENT_GAMEPAD_REMOVED:
    // Reinitialize game controllers on controller add/remove/remap
    gamepads_initialize();
    break;

  case SDL_EVENT_KEY_DOWN:
    // Settings view toggles handled here to avoid being able to get stuck in the config view
    // Toggle settings with Command/Win+comma (for keyboards without function keys)
    if (event->key.key == SDLK_COMMA && event->key.repeat == 0 && (event->key.mod & SDL_KMOD_GUI)) {
      settings_toggle_open();
      return ret_val;
    }
    // Toggle settings with config defined key
    if (event->key.scancode == ctx->conf.key_toggle_settings && event->key.repeat == 0) {
      settings_toggle_open();
      return ret_val;
    }
    // Route to settings if open
    if (settings_is_open()) {
      settings_handle_event(ctx, event);
      return ret_val;
    }
    // Toggle the plugin rack overlay (F3), then route to it when open.
    if (event->key.scancode == SDL_SCANCODE_F3 && event->key.repeat == 0) {
      plugin_rack_toggle_open();
      return ret_val;
    }

    // Dual-M8: Tab switches which M8 the keyboard/gamepad controls.
    if (event->key.scancode == SDL_SCANCODE_TAB && event->key.repeat == 0 &&
        m8_device_count() >= 2) {
      const int next = input_focused_device() ? 0 : 1;
      input_set_focused_device(next);
      renderer_set_focus(next);
      renderer_set_title(next ? "m8c  focus: M8 #2" : "m8c  focus: M8 #1");
      return ret_val;
    }

    // Toggle monitor output: SDL (default, safe) <-> duplex CoreAudio (lowest
    // latency, one clock). Global; works with the overlay open.
    if (event->key.scancode == SDL_SCANCODE_F7 && event->key.repeat == 0) {
      const int mode = recorder_toggle_monitor_mode();
      if (mode < 0)
        renderer_set_title("m8c  monitor: duplex unavailable (input-only)");
      else
        renderer_set_title(mode ? "m8c  monitor: DUPLEX (low latency)"
                                : "m8c  monitor: SDL");
      return ret_val;
    }

    // Live audio buffer size: F9 = smaller (lower latency, for live playing),
    // F10 = larger (more headroom, for full recording). Global; works even
    // with the rack overlay open.
    if ((event->key.scancode == SDL_SCANCODE_F9 ||
         event->key.scancode == SDL_SCANCODE_F10) &&
        event->key.repeat == 0) {
      static const int ladder[] = {128, 256, 512, 1024, 2048};
      const int n = (int)(sizeof(ladder) / sizeof(ladder[0]));
      const int cur = m8_capture_block_frames();
      int idx = 0;
      for (int i = 0; i < n; i++)
        if (cur >= ladder[i])
          idx = i;
      idx += (event->key.scancode == SDL_SCANCODE_F10) ? 1 : -1;
      if (idx < 0)
        idx = 0;
      if (idx >= n)
        idx = n - 1;
      const int got = m8_capture_set_block_frames(ladder[idx]);
      const int rate = m8_capture_rate();
      char title[64];
      SDL_snprintf(title, sizeof(title), "m8c  buffer %d smp (%.1f ms)", got,
                   rate > 0 ? 1000.0 * got / rate : 0.0);
      renderer_set_title(title);
      return ret_val;
    }
    if (plugin_rack_is_open()) {
      plugin_rack_handle_event(ctx, event);
      return ret_val;
    }
    input_handle_key_down_event(ctx, event);
    break;

  case SDL_EVENT_KEY_UP:
    if (settings_is_open()) {
      settings_handle_event(ctx, event);
      return ret_val;
    }
    if (plugin_rack_is_open()) {
      return ret_val; // consume; rack acts on key-down only
    }
    input_handle_key_up_event(ctx, event);
    break;

  case SDL_EVENT_TEXT_INPUT:
    // Text entry for the plugin rack (naming a song).
    if (plugin_rack_is_open()) {
      plugin_rack_handle_event(ctx, event);
      return ret_val;
    }
    break;

  case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    if (settings_is_open()) {
      settings_handle_event(ctx, event);
      return ret_val;
    }

    // Allow toggling the settings view using a gamepad only when the device is disconnected to
    // avoid accidentally opening the screen while using the device
    if (event->gbutton.button == SDL_GAMEPAD_BUTTON_BACK) {
      if (ctx->app_state == WAIT_FOR_DEVICE && !settings_is_open()) {
        settings_toggle_open();
      }
    }

    input_handle_gamepad_button(ctx, event->gbutton.button, true);
    break;

  case SDL_EVENT_GAMEPAD_BUTTON_UP:
    if (settings_is_open()) {
      settings_handle_event(ctx, event);
      return ret_val;
    }
    input_handle_gamepad_button(ctx, event->gbutton.button, false);
    break;

  case SDL_EVENT_GAMEPAD_AXIS_MOTION:
    if (settings_is_open()) {
      settings_handle_event(ctx, event);
      return ret_val;
    }
    input_handle_gamepad_axis(ctx, event->gaxis.axis, event->gaxis.value);
    break;

  default:
    break;
  }
  return ret_val;
}
