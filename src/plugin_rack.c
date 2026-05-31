// In-app overlay for the VST/AU plugin racks. See plugin_rack.h.
// Released under the MIT licence, https://opensource.org/licenses/MIT
#include "plugin_rack.h"

#include "SDL2_inprint.h"
#include "fonts/fonts.h"
#include "host/juce_host.h"

#include <SDL3/SDL.h>

#define LINE_H 10
#define MARGIN 4

enum { MODE_RACK, MODE_BROWSER };

static const char *BUS_NAMES[JUCE_HOST_NUM_BUSES] = {"SEND 1", "SEND 2", "SEND 3", "MASTER"};

static struct {
  bool is_open;
  int mode;
  int sel_bus;                       // 0..3
  int sel_slot[JUCE_HOST_NUM_BUSES]; // per-bus selected slot
  int target_bus;                    // bus the browser loads into
  int browser_sel;
  int browser_scroll;
  bool loaded_once; // auto-load persisted state on first open
  int needs_redraw;
  SDL_Texture *texture;
  char state_path[1024];
} g;

static void state_path(char *out, size_t n) {
  const char *pref = SDL_GetPrefPath("", "m8c");
  SDL_snprintf(out, n, "%shost_state.xml", pref ? pref : "");
}

void plugin_rack_toggle_open(void) {
  g.is_open = !g.is_open;
  if (g.is_open) {
    g.mode = MODE_RACK;
    if (!g.loaded_once) {
      state_path(g.state_path, sizeof(g.state_path));
      juce_host_load(g.state_path);
      g.loaded_once = true;
    }
  }
  g.needs_redraw = 1;
}

bool plugin_rack_is_open(void) { return g.is_open; }

static void clamp_slot(void) {
  int n = juce_host_bus_slot_count(g.sel_bus);
  int cap = juce_host_bus_capacity(g.sel_bus);
  int max = (n < cap ? n : cap - 1); // allow selecting the first empty slot
  if (max < 0) max = 0;
  if (g.sel_slot[g.sel_bus] > max) g.sel_slot[g.sel_bus] = max;
  if (g.sel_slot[g.sel_bus] < 0) g.sel_slot[g.sel_bus] = 0;
}

void plugin_rack_handle_event(struct app_context *ctx, const SDL_Event *e) {
  (void)ctx;
  if (e->type != SDL_EVENT_KEY_DOWN)
    return;
  const SDL_Scancode sc = e->key.scancode;
  const bool shift = (e->key.mod & SDL_KMOD_SHIFT) != 0;
  // Allow key-repeat (hold to scroll) for navigation arrows only; one-shot
  // actions (load, bypass, scan, ...) must not auto-repeat.
  const bool is_nav = (sc == SDL_SCANCODE_UP || sc == SDL_SCANCODE_DOWN ||
                       sc == SDL_SCANCODE_LEFT || sc == SDL_SCANCODE_RIGHT);
  if (e->key.repeat && !is_nav)
    return;
  g.needs_redraw = 1;

  if (g.mode == MODE_BROWSER) {
    const int count = juce_host_known_count();
    switch (sc) {
    case SDL_SCANCODE_UP:
      if (g.browser_sel > 0) g.browser_sel--;
      break;
    case SDL_SCANCODE_DOWN:
      if (g.browser_sel < count - 1) g.browser_sel++;
      break;
    case SDL_SCANCODE_S:
      juce_host_scan();
      break;
    case SDL_SCANCODE_RETURN:
      if (count > 0)
        juce_host_bus_add(g.target_bus, g.browser_sel);
      g.mode = MODE_RACK;
      break;
    case SDL_SCANCODE_ESCAPE:
      g.mode = MODE_RACK;
      break;
    default:
      break;
    }
    return;
  }

  // RACK mode.
  switch (sc) {
  case SDL_SCANCODE_ESCAPE:
    g.is_open = false;
    break;
  case SDL_SCANCODE_LEFT:
    if (g.sel_bus > 0) g.sel_bus--;
    clamp_slot();
    break;
  case SDL_SCANCODE_RIGHT:
    if (g.sel_bus < JUCE_HOST_NUM_BUSES - 1) g.sel_bus++;
    clamp_slot();
    break;
  case SDL_SCANCODE_UP:
    if (shift) { // Shift+Up = move the plugin up in the chain
      juce_host_bus_move(g.sel_bus, g.sel_slot[g.sel_bus], -1);
      if (g.sel_slot[g.sel_bus] > 0) g.sel_slot[g.sel_bus]--;
    } else if (g.sel_slot[g.sel_bus] > 0) {
      g.sel_slot[g.sel_bus]--;
    }
    break;
  case SDL_SCANCODE_DOWN: {
    int n = juce_host_bus_slot_count(g.sel_bus);
    if (shift) { // Shift+Down = move the plugin down in the chain
      if (g.sel_slot[g.sel_bus] < n - 1) {
        juce_host_bus_move(g.sel_bus, g.sel_slot[g.sel_bus], +1);
        g.sel_slot[g.sel_bus]++;
      }
    } else {
      int cap = juce_host_bus_capacity(g.sel_bus);
      int max = (n < cap) ? n : cap - 1;
      if (g.sel_slot[g.sel_bus] < max) g.sel_slot[g.sel_bus]++;
    }
    break;
  }
  case SDL_SCANCODE_RETURN:
  case SDL_SCANCODE_A: {
    int n = juce_host_bus_slot_count(g.sel_bus);
    if (g.sel_slot[g.sel_bus] >= n) {
      // empty slot -> open browser to add
      g.target_bus = g.sel_bus;
      g.mode = MODE_BROWSER;
    } else if (sc == SDL_SCANCODE_RETURN) {
      juce_host_slot_open_editor(g.sel_bus, g.sel_slot[g.sel_bus]); // open GUI
    } else {
      g.target_bus = g.sel_bus;
      g.mode = MODE_BROWSER;
    }
    break;
  }
  case SDL_SCANCODE_E:
    juce_host_slot_open_editor(g.sel_bus, g.sel_slot[g.sel_bus]);
    break;
  case SDL_SCANCODE_B: {
    int s = g.sel_slot[g.sel_bus];
    juce_host_slot_set_bypass(g.sel_bus, s, !juce_host_slot_is_bypassed(g.sel_bus, s));
    break;
  }
  case SDL_SCANCODE_X:
  case SDL_SCANCODE_DELETE:
  case SDL_SCANCODE_BACKSPACE:
    juce_host_bus_remove(g.sel_bus, g.sel_slot[g.sel_bus]);
    clamp_slot();
    break;
  case SDL_SCANCODE_S:
    juce_host_scan();
    break;
  case SDL_SCANCODE_W:
    state_path(g.state_path, sizeof(g.state_path));
    juce_host_save(g.state_path);
    break;
  default:
    break;
  }
}

static void trunc_copy(char *dst, const char *src, int maxchars) {
  int i = 0;
  for (; src[i] && i < maxchars; i++) dst[i] = src[i];
  dst[i] = '\0';
}

static void render_rack(SDL_Renderer *rend, int tw, int th) {
  const Uint32 fg = 0xFFFFFF, sel = 0x00FFFF, title = 0xFF0000, dim = 0x888888, hdr = 0xAAAAFF;
  const int gx = (int)fonts_get(0)->glyph_x;
  const int col_w = tw / JUCE_HOST_NUM_BUSES;
  const int chars = (col_w - 2) / (gx > 0 ? gx : 6);

  inprint(rend, "PLUGIN RACKS", MARGIN, MARGIN, title, title);

  const int top = MARGIN + LINE_H + 2;
  for (int b = 0; b < JUCE_HOST_NUM_BUSES; b++) {
    int x = b * col_w + 2;
    int y = top;
    inprint(rend, BUS_NAMES[b], x, y, b == g.sel_bus ? sel : hdr, 0);
    y += LINE_H;
    int cap = juce_host_bus_capacity(b);
    int n = juce_host_bus_slot_count(b);
    for (int s = 0; s < cap; s++) {
      char name[64] = {0}, line[80];
      bool is_sel = (b == g.sel_bus && s == g.sel_slot[b]);
      if (s < n) {
        char raw[128];
        juce_host_slot_label(b, s, raw, sizeof(raw));
        trunc_copy(name, raw, chars - 2);
        SDL_snprintf(line, sizeof(line), "%c%s", juce_host_slot_is_bypassed(b, s) ? '/' : ' ', name);
      } else {
        SDL_snprintf(line, sizeof(line), " ----");
      }
      Uint32 c = is_sel ? sel : (s < n ? (juce_host_slot_is_bypassed(b, s) ? dim : fg) : dim);
      inprint(rend, line, x, y, c, 0);
      y += LINE_H;
    }
  }

  // Footer hints.
  char foot[160];
  SDL_snprintf(foot, sizeof(foot),
               "Arrows=nav  Shift+Up/Dn=move  A/Enter=add/edit  E=editor  B=bypass  X=del  S=scan  W=save");
  inprint(rend, foot, MARGIN, th - LINE_H, dim, 0);
  char lat[48];
  SDL_snprintf(lat, sizeof(lat), "PDC: %d smp", juce_host_latency_samples());
  inprint(rend, lat, tw - (int)SDL_strlen(lat) * gx - MARGIN, MARGIN, dim, 0);
}

static void render_browser(SDL_Renderer *rend, int tw, int th) {
  const Uint32 fg = 0xFFFFFF, sel = 0x00FFFF, title = 0xFF0000, dim = 0x888888;
  const int gx = (int)fonts_get(0)->glyph_x;
  const int count = juce_host_known_count();

  char head[80];
  SDL_snprintf(head, sizeof(head), "PICK PLUGIN FOR %s  (%d found)", BUS_NAMES[g.target_bus], count);
  inprint(rend, head, MARGIN, MARGIN, title, title);

  if (juce_host_is_scanning()) {
    inprint(rend, "scanning...", MARGIN, MARGIN + LINE_H * 2, sel, 0);
    return;
  }
  if (count == 0) {
    inprint(rend, "No plugins. Press S to scan VST3/AU.", MARGIN, MARGIN + LINE_H * 2, fg, 0);
  }

  const int top = MARGIN + LINE_H + 2;
  const int rows = (th - top - LINE_H) / LINE_H;
  if (g.browser_sel < g.browser_scroll) g.browser_scroll = g.browser_sel;
  if (g.browser_sel >= g.browser_scroll + rows) g.browser_scroll = g.browser_sel - rows + 1;

  const int maxchars = (tw - 2 * MARGIN) / (gx > 0 ? gx : 6);
  for (int i = 0; i < rows; i++) {
    int idx = g.browser_scroll + i;
    if (idx >= count) break;
    char raw[160], line[160];
    juce_host_known_label(idx, raw, sizeof(raw));
    trunc_copy(line, raw, maxchars);
    inprint(rend, line, MARGIN, top + i * LINE_H, idx == g.browser_sel ? sel : fg, 0);
  }

  inprint(rend, "Up/Down  Enter=load  Esc=back  S=rescan", MARGIN, th - LINE_H, dim, 0);
}

void plugin_rack_render_overlay(SDL_Renderer *rend, int texture_w, int texture_h) {
  if (!g.is_open)
    return;

  const struct inline_font *previous_font = inline_font_get_current();
  if (previous_font->glyph_x != fonts_get(0)->glyph_x) {
    inline_font_close();
    inline_font_initialize(fonts_get(0));
  }

  if (g.texture == NULL) {
    g.texture = SDL_CreateTexture(rend, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_TARGET,
                                  texture_w, texture_h);
    if (g.texture == NULL) {
      inline_font_close();
      inline_font_initialize(previous_font);
      return;
    }
    SDL_SetTextureBlendMode(g.texture, SDL_BLENDMODE_BLEND);
    SDL_SetTextureScaleMode(g.texture, SDL_SCALEMODE_NEAREST);
    g.needs_redraw = 1;
  }

  // The host's known list / slot state can change without a key event (scan),
  // so just redraw every frame while open — cheap at 320x240.
  SDL_Texture *prev = SDL_GetRenderTarget(rend);
  SDL_SetRenderTarget(rend, g.texture);
  SDL_SetRenderDrawColor(rend, 0, 0, 0, 235);
  SDL_RenderClear(rend);

  if (g.mode == MODE_BROWSER)
    render_browser(rend, texture_w, texture_h);
  else
    render_rack(rend, texture_w, texture_h);

  SDL_SetRenderTarget(rend, prev);
  SDL_RenderTexture(rend, g.texture, NULL, NULL);

  if (previous_font->glyph_x != fonts_get(0)->glyph_x) {
    inline_font_close();
    inline_font_initialize(previous_font);
  }
}

void plugin_rack_on_texture_size_change(SDL_Renderer *rend) {
  (void)rend;
  if (g.texture) {
    SDL_DestroyTexture(g.texture);
    g.texture = NULL;
  }
  g.needs_redraw = 1;
}
