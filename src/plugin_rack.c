// In-app overlay for the VST/AU plugin racks. See plugin_rack.h.
// Released under the MIT licence, https://opensource.org/licenses/MIT
#include "plugin_rack.h"

#include "SDL2_inprint.h"
#include "fonts/fonts.h"
#include "host/juce_host.h"

#include <SDL3/SDL.h>

#define LINE_H 10
#define MARGIN 4

enum { MODE_RACK, MODE_BROWSER, MODE_PARAMPICK, MODE_SONGS };

#define MAX_SONGS 128

static const char *BUS_NAMES[JUCE_HOST_NUM_BUSES] = {"SEND 1", "SEND 2", "SEND 3", "MASTER"};

static struct {
  bool is_open;
  int mode;
  int sel_bus;                       // 0..3
  int sel_slot[JUCE_HOST_NUM_BUSES]; // per-bus selected slot
  int target_bus;                    // bus the browser loads into
  int browser_sel;
  int browser_scroll;
  int focus;                         // 0 = rack grid, 1 = params strip (edit)
  int params_bus, params_slot;       // slot whose params the picker edits
  int params_sel;                    // selected quick (0..2)
  int pick_sel, pick_scroll;         // parameter picker selection
  bool started;                      // initial auto-load of last song done
  int needs_redraw;
  SDL_Texture *texture;
  // --- Songs ---
  char songs_dir[1024];
  char cur_song[64];                 // current song name ("" = none)
  char song_names[MAX_SONGS][64];    // browser list (without .xml)
  int song_count, song_sel, song_scroll;
  bool naming;                       // text-entry active (new song)
  char name_buf[64];
  int name_len;
} g;

static SDL_AtomicInt g_pending_song; // CC recall: 0 = none, N+1 = recall song N

static void songs_dir_path(void) {
  if (g.songs_dir[0])
    return;
  const char *pref = SDL_GetPrefPath("", "m8c");
  SDL_snprintf(g.songs_dir, sizeof(g.songs_dir), "%ssongs", pref ? pref : "");
  SDL_CreateDirectory(g.songs_dir);
}

static void song_path(const char *name, char *out, size_t n) {
  songs_dir_path();
  SDL_snprintf(out, n, "%s/%s.xml", g.songs_dir, name);
}

static void last_song_path(char *out, size_t n) {
  const char *pref = SDL_GetPrefPath("", "m8c");
  SDL_snprintf(out, n, "%slast_song.txt", pref ? pref : "");
}

static void write_last_song(void) {
  char p[1024];
  last_song_path(p, sizeof(p));
  SDL_IOStream *io = SDL_IOFromFile(p, "w");
  if (io) {
    SDL_WriteIO(io, g.cur_song, SDL_strlen(g.cur_song));
    SDL_CloseIO(io);
  }
}

static void songs_refresh(void) {
  songs_dir_path();
  g.song_count = 0;
  int count = 0;
  char **files = SDL_GlobDirectory(g.songs_dir, "*.xml", 0, &count);
  if (files) {
    for (int i = 0; i < count && g.song_count < MAX_SONGS; i++) {
      char nm[64];
      SDL_snprintf(nm, sizeof(nm), "%s", files[i]);
      char *dot = SDL_strrchr(nm, '.'); // strip .xml
      if (dot) *dot = '\0';
      if (nm[0])
        SDL_snprintf(g.song_names[g.song_count++], 64, "%s", nm);
    }
    SDL_free(files);
  }
}

// Save the current song (called on structural changes / close / quit).
static void autosave(void) {
  if (!g.cur_song[0])
    return;
  char p[1100];
  song_path(g.cur_song, p, sizeof(p));
  juce_host_save(p);
}

static void load_song(const char *name) {
  char p[1100];
  song_path(name, p, sizeof(p));
  juce_host_load(p);
  SDL_snprintf(g.cur_song, sizeof(g.cur_song), "%s", name);
  write_last_song();
}

void plugin_rack_toggle_open(void) {
  g.is_open = !g.is_open;
  if (g.is_open) {
    g.mode = MODE_RACK;
    g.focus = 0;
  } else {
    autosave(); // save current song on close
  }
  g.needs_redraw = 1;
}

// Polled from the main loop: do the one-time startup load + CC song recalls
// (plugin instantiation must happen on the main thread).
void plugin_rack_tick(void) {
  if (!juce_host_is_active())
    return;
  if (!g.started) {
    g.started = true;
    char p[1024];
    last_song_path(p, sizeof(p));
    SDL_IOStream *io = SDL_IOFromFile(p, "r");
    if (io) {
      char nm[64] = {0};
      size_t r = SDL_ReadIO(io, nm, sizeof(nm) - 1);
      nm[r] = '\0';
      SDL_CloseIO(io);
      if (nm[0]) {
        load_song(nm);
        SDL_Log("plugin_rack: loaded last song '%s'", nm);
      }
    }
  }
  const int pend = SDL_GetAtomicInt(&g_pending_song);
  if (pend > 0) {
    SDL_SetAtomicInt(&g_pending_song, 0);
    songs_refresh();
    const int idx = pend - 1;
    if (idx < g.song_count) {
      load_song(g.song_names[idx]);
      SDL_Log("plugin_rack: CC recalled song %d '%s'", idx, g.song_names[idx]);
    }
    g.needs_redraw = 1;
  }
}

void plugin_rack_request_song(int index) {
  if (index >= 0)
    SDL_SetAtomicInt(&g_pending_song, index + 1);
}

void plugin_rack_shutdown(void) { autosave(); }

bool plugin_rack_is_open(void) { return g.is_open; }

static void clamp_slot(void) {
  int n = juce_host_bus_slot_count(g.sel_bus);
  int cap = juce_host_bus_capacity(g.sel_bus);
  int max = (n < cap ? n : cap - 1); // allow selecting the first empty slot
  if (max < 0) max = 0;
  if (g.sel_slot[g.sel_bus] > max) g.sel_slot[g.sel_bus] = max;
  if (g.sel_slot[g.sel_bus] < 0) g.sel_slot[g.sel_bus] = 0;
}

// Begin/end text entry for naming a new song.
static void start_naming(void) {
  g.naming = true;
  g.name_len = 0;
  g.name_buf[0] = '\0';
  SDL_StartTextInput(SDL_GetKeyboardFocus());
}
static void stop_naming(void) {
  g.naming = false;
  SDL_StopTextInput(SDL_GetKeyboardFocus());
}

void plugin_rack_handle_event(struct app_context *ctx, const SDL_Event *e) {
  (void)ctx;

  // Text entry (new song name) intercepts everything while active.
  if (g.naming) {
    if (e->type == SDL_EVENT_TEXT_INPUT) {
      for (const char *c = e->text.text; *c && g.name_len < (int)sizeof(g.name_buf) - 1; c++)
        if (*c != '/' && *c != '\\' && (unsigned char)*c >= 32) // keep filename-safe
          g.name_buf[g.name_len++] = *c;
      g.name_buf[g.name_len] = '\0';
      g.needs_redraw = 1;
      return;
    }
    if (e->type == SDL_EVENT_KEY_DOWN) {
      switch (e->key.scancode) {
      case SDL_SCANCODE_BACKSPACE:
        if (g.name_len > 0) g.name_buf[--g.name_len] = '\0';
        break;
      case SDL_SCANCODE_RETURN:
        if (g.name_len > 0) {
          SDL_snprintf(g.cur_song, sizeof(g.cur_song), "%s", g.name_buf);
          autosave(); // writes current host state to the new song file
          songs_refresh();
          write_last_song();
        }
        stop_naming();
        g.mode = MODE_SONGS;
        break;
      case SDL_SCANCODE_ESCAPE:
        stop_naming();
        break;
      default:
        break;
      }
      g.needs_redraw = 1;
    }
    return;
  }

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
    const int step = shift ? 20 : 1; // Shift = fast scroll
    switch (sc) {
    case SDL_SCANCODE_UP:
      g.browser_sel = (g.browser_sel > step) ? g.browser_sel - step : 0;
      break;
    case SDL_SCANCODE_DOWN:
      g.browser_sel += step;
      if (g.browser_sel > count - 1) g.browser_sel = count - 1;
      if (g.browser_sel < 0) g.browser_sel = 0;
      break;
    case SDL_SCANCODE_S:
      juce_host_scan();
      break;
    case SDL_SCANCODE_RETURN:
      if (count > 0) {
        juce_host_bus_add(g.target_bus, g.browser_sel);
        autosave();
      }
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

  if (g.mode == MODE_PARAMPICK) {
    const int pc = juce_host_slot_param_count(g.params_bus, g.params_slot);
    const int step = shift ? 20 : 1; // Shift = fast scroll
    switch (sc) {
    case SDL_SCANCODE_UP:
      g.pick_sel = (g.pick_sel > step) ? g.pick_sel - step : 0;
      break;
    case SDL_SCANCODE_DOWN:
      g.pick_sel += step;
      if (g.pick_sel > pc - 1) g.pick_sel = pc - 1;
      if (g.pick_sel < 0) g.pick_sel = 0;
      break;
    case SDL_SCANCODE_RETURN:
      juce_host_slot_quick_assign(g.params_bus, g.params_slot, g.params_sel, g.pick_sel);
      autosave();
      g.mode = MODE_RACK;
      g.focus = 1;
      break;
    case SDL_SCANCODE_ESCAPE:
      g.mode = MODE_RACK;
      g.focus = 1;
      break;
    default:
      break;
    }
    return;
  }

  if (g.mode == MODE_SONGS) {
    const int step = shift ? 10 : 1;
    switch (sc) {
    case SDL_SCANCODE_UP:
      g.song_sel = (g.song_sel > step) ? g.song_sel - step : 0;
      break;
    case SDL_SCANCODE_DOWN:
      g.song_sel += step;
      if (g.song_sel > g.song_count - 1) g.song_sel = g.song_count - 1;
      if (g.song_sel < 0) g.song_sel = 0;
      break;
    case SDL_SCANCODE_RETURN: // load selected song
      if (g.song_sel >= 0 && g.song_sel < g.song_count) {
        load_song(g.song_names[g.song_sel]);
        g.mode = MODE_RACK;
      }
      break;
    case SDL_SCANCODE_N: // new song (type a name)
      start_naming();
      break;
    case SDL_SCANCODE_X:
    case SDL_SCANCODE_DELETE:
    case SDL_SCANCODE_BACKSPACE:
      if (g.song_sel >= 0 && g.song_sel < g.song_count) {
        char p[1100];
        song_path(g.song_names[g.song_sel], p, sizeof(p));
        SDL_RemovePath(p);
        if (SDL_strcmp(g.song_names[g.song_sel], g.cur_song) == 0)
          g.cur_song[0] = '\0';
        songs_refresh();
        if (g.song_sel >= g.song_count) g.song_sel = g.song_count - 1;
        if (g.song_sel < 0) g.song_sel = 0;
      }
      break;
    case SDL_SCANCODE_ESCAPE:
      g.mode = MODE_RACK;
      break;
    default:
      break;
    }
    return;
  }

  // RACK mode — when focused on the params strip, keys edit the selected slot's
  // quick params instead of navigating the grid.
  if (g.focus == 1) {
    const int slot = g.sel_slot[g.sel_bus];
    const bool loaded = slot < juce_host_bus_slot_count(g.sel_bus);
    switch (sc) {
    case SDL_SCANCODE_UP:
      if (g.params_sel > 0) g.params_sel--;
      break;
    case SDL_SCANCODE_DOWN:
      if (g.params_sel < JUCE_HOST_NUM_QUICK - 1) g.params_sel++;
      break;
    case SDL_SCANCODE_LEFT:
      if (loaded) juce_host_slot_quick_nudge(g.sel_bus, slot, g.params_sel, -0.05f);
      break;
    case SDL_SCANCODE_RIGHT:
      if (loaded) juce_host_slot_quick_nudge(g.sel_bus, slot, g.params_sel, +0.05f);
      break;
    case SDL_SCANCODE_RETURN: // assign a plugin parameter from the list
      if (loaded) {
        g.params_bus = g.sel_bus;
        g.params_slot = slot;
        g.pick_sel = 0;
        g.pick_scroll = 0;
        g.mode = MODE_PARAMPICK;
      }
      break;
    case SDL_SCANCODE_L: // MIDI-learn: bind the next incoming CC
      if (loaded) juce_host_begin_learn(g.sel_bus, slot, g.params_sel);
      break;
    case SDL_SCANCODE_X:
    case SDL_SCANCODE_DELETE:
    case SDL_SCANCODE_BACKSPACE:
      if (loaded) { juce_host_slot_quick_assign(g.sel_bus, slot, g.params_sel, -1); autosave(); }
      break;
    case SDL_SCANCODE_P:
    case SDL_SCANCODE_ESCAPE:
      g.focus = 0; // back to grid navigation
      break;
    default:
      break;
    }
    return;
  }

  // RACK mode (grid navigation).
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
      autosave();
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
        autosave();
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
  case SDL_SCANCODE_P: // focus the quick-params strip for the selected slot
    if (g.sel_slot[g.sel_bus] < juce_host_bus_slot_count(g.sel_bus))
      g.focus = 1;
    break;
  case SDL_SCANCODE_B: {
    int s = g.sel_slot[g.sel_bus];
    juce_host_slot_set_bypass(g.sel_bus, s, !juce_host_slot_is_bypassed(g.sel_bus, s));
    autosave();
    break;
  }
  case SDL_SCANCODE_X:
  case SDL_SCANCODE_DELETE:
  case SDL_SCANCODE_BACKSPACE:
    juce_host_bus_remove(g.sel_bus, g.sel_slot[g.sel_bus]);
    clamp_slot();
    autosave();
    break;
  case SDL_SCANCODE_C: // cycle the lane's MIDI channel (0=off, 1..16)
    if (g.sel_bus < JUCE_HOST_NUM_SENDS) {
      int mc = (juce_host_bus_midi_channel(g.sel_bus) + 1) % 17;
      juce_host_bus_set_midi_channel(g.sel_bus, mc);
      autosave();
    }
    break;
  case SDL_SCANCODE_O: // songs: load / new / delete
    songs_refresh();
    g.song_sel = 0;
    g.song_scroll = 0;
    g.mode = MODE_SONGS;
    break;
  case SDL_SCANCODE_S:
    juce_host_scan();
    break;
  case SDL_SCANCODE_W: // force-save the current song (or name a new one)
    if (g.cur_song[0])
      autosave();
    else
      start_naming();
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

// Draw a solid highlight bar (selection cursor) at a row.
static void hl_bar(SDL_Renderer *rend, int x, int y, int w) {
  SDL_SetRenderDrawColor(rend, 0x00, 0xC0, 0xFF, 0xFF); // bright cyan-blue
  SDL_FRect r = {(float)x, (float)(y - 1), (float)w, (float)LINE_H};
  SDL_RenderFillRect(rend, &r);
}

static void render_rack(SDL_Renderer *rend, int tw, int th) {
  const Uint32 fg = 0xFFFFFF, sel = 0x00FFFF, title = 0xFF0000, dim = 0x888888, hdr = 0xAAAAFF;
  const int gx = (int)fonts_get(0)->glyph_x;
  const int col_w = tw / JUCE_HOST_NUM_BUSES;
  const int chars = (col_w - 2) / (gx > 0 ? gx : 6);

  {
    char rt[80];
    SDL_snprintf(rt, sizeof(rt), "PLUGIN RACKS  [%s]", g.cur_song[0] ? g.cur_song : "no song");
    inprint(rend, rt, MARGIN, MARGIN, title, title);
  }

  const int top = MARGIN + LINE_H + 2;
  for (int b = 0; b < JUCE_HOST_NUM_BUSES; b++) {
    int x = b * col_w + 2;
    int y = top;
    const Uint32 hc = (b == g.sel_bus) ? sel : hdr;
    inprint(rend, BUS_NAMES[b], x, y, hc, 0);
    y += LINE_H;
    // 2nd header line: MIDI channel + the M8 send CC for this lane.
    char sub[24];
    if (b < JUCE_HOST_NUM_SENDS) {
      int mc = juce_host_bus_midi_channel(b);
      if (mc > 0) SDL_snprintf(sub, sizeof(sub), "ch%d CC%d", mc, 20 + b);
      else SDL_snprintf(sub, sizeof(sub), "ch- CC%d", 20 + b);
    } else {
      SDL_snprintf(sub, sizeof(sub), "out");
    }
    inprint(rend, sub, x, y, dim, 0);
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
      if (is_sel)
        hl_bar(rend, x - 2, y, col_w - 2);
      Uint32 c = is_sel ? 0x000000
                        : (s < n ? (juce_host_slot_is_bypassed(b, s) ? dim : fg) : dim);
      inprint(rend, line, x, y, c, 0);
      y += LINE_H;
    }
  }

  // --- Quick-params strip for the selected slot (bottom) ---
  const int sel_slot = g.sel_slot[g.sel_bus];
  const bool sel_loaded = sel_slot < juce_host_bus_slot_count(g.sel_bus);
  int sy = th - LINE_H * 7;
  {
    char sname[40], shead[64];
    if (sel_loaded) {
      juce_host_slot_label(g.sel_bus, sel_slot, sname, sizeof(sname));
      const char *tag = !g.focus ? "" : (juce_host_is_learning() ? "  [LEARN: move a CC]" : "  [EDIT]");
      SDL_snprintf(shead, sizeof(shead), "PARAMS: %s%s", sname, tag);
    } else {
      SDL_snprintf(shead, sizeof(shead), "PARAMS: (empty slot)");
    }
    inprint(rend, shead, MARGIN, sy, g.focus ? sel : hdr, 0);
    sy += LINE_H;
    if (sel_loaded) {
      for (int q = 0; q < JUCE_HOST_NUM_QUICK; q++) {
        char pn[40], qline[120], ccb[10];
        juce_host_slot_quick_label(g.sel_bus, sel_slot, q, pn, sizeof(pn));
        const float v = juce_host_slot_quick_value(g.sel_bus, sel_slot, q);
        const int cc = juce_host_slot_quick_cc(g.sel_bus, sel_slot, q);
        if (cc >= 0) SDL_snprintf(ccb, sizeof(ccb), "CC%d", cc);
        else SDL_snprintf(ccb, sizeof(ccb), "CC--");
        SDL_snprintf(qline, sizeof(qline), "Q%d %-18s %3d%%  %s", q + 1, pn,
                     (int)(v * 100.0f + 0.5f), ccb);
        const bool qs = (g.focus && q == g.params_sel);
        if (qs) hl_bar(rend, 0, sy, tw);
        inprint(rend, qline, MARGIN, sy, qs ? 0x000000 : fg, 0);
        sy += LINE_H;
      }
    }
  }

  // M8 CC legend (which CCs to use on the M8).
  inprint(rend, "M8 CC  arm 100   song recall 102   sends 20/21/22 on track ch",
          MARGIN, th - LINE_H * 3, hdr, 0);

  // Footer key hints (context-dependent), split to fit the 320px width.
  if (g.focus) {
    inprint(rend, "Up/Dn pick Q   Left/Right value   Enter assign param", MARGIN, th - LINE_H * 2,
            dim, 0);
    inprint(rend, "L MIDI-learn   X clear   P/Esc back", MARGIN, th - LINE_H, dim, 0);
  } else {
    inprint(rend, "Arrows move  Shift+UpDn reorder  A add  E editor  P params", MARGIN,
            th - LINE_H * 2, dim, 0);
    inprint(rend, "B byp  X del  C midi-ch  O songs  S scan  W save  Esc close", MARGIN,
            th - LINE_H, dim, 0);
  }
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
    const int y = top + i * LINE_H;
    const bool selrow = (idx == g.browser_sel);
    if (selrow)
      hl_bar(rend, 0, y, tw);
    inprint(rend, line, MARGIN, y, selrow ? 0x000000 : fg, 0);
  }

  inprint(rend, "Up/Down (Shift=fast)  Enter=load  Esc=back  S=rescan", MARGIN, th - LINE_H, dim, 0);
}

static void render_parampick(SDL_Renderer *rend, int tw, int th) {
  const Uint32 fg = 0xFFFFFF, title = 0xFF0000, dim = 0x888888;
  const int gx = (int)fonts_get(0)->glyph_x;
  const int pc = juce_host_slot_param_count(g.params_bus, g.params_slot);

  char head[64];
  SDL_snprintf(head, sizeof(head), "ASSIGN PARAM TO Q%d  (%d params)", g.params_sel + 1, pc);
  inprint(rend, head, MARGIN, MARGIN, title, title);

  const int top = MARGIN + LINE_H + 2;
  const int rows = (th - top - LINE_H) / LINE_H;
  if (g.pick_sel < g.pick_scroll) g.pick_scroll = g.pick_sel;
  if (g.pick_sel >= g.pick_scroll + rows) g.pick_scroll = g.pick_sel - rows + 1;

  const int maxchars = (tw - 2 * MARGIN) / (gx > 0 ? gx : 6);
  for (int i = 0; i < rows; i++) {
    int idx = g.pick_scroll + i;
    if (idx >= pc) break;
    char raw[96], line[96];
    juce_host_slot_param_name(g.params_bus, g.params_slot, idx, raw, sizeof(raw));
    trunc_copy(line, raw, maxchars);
    const int y = top + i * LINE_H;
    const bool selrow = (idx == g.pick_sel);
    if (selrow) hl_bar(rend, 0, y, tw);
    inprint(rend, line, MARGIN, y, selrow ? 0x000000 : fg, 0);
  }
  inprint(rend, "Up/Down (Shift=fast)  Enter=assign  Esc=back", MARGIN, th - LINE_H, dim, 0);
}

static void render_songs(SDL_Renderer *rend, int tw, int th) {
  const Uint32 fg = 0xFFFFFF, sel = 0x00FFFF, title = 0xFF0000, dim = 0x888888;
  const int gx = (int)fonts_get(0)->glyph_x;

  // Naming prompt for a new song.
  if (g.naming) {
    inprint(rend, "NEW SONG - type a name:", MARGIN, MARGIN, title, title);
    char line[80];
    SDL_snprintf(line, sizeof(line), "> %s_", g.name_buf);
    inprint(rend, line, MARGIN, MARGIN + LINE_H * 2, sel, 0);
    inprint(rend, "Enter=create  Esc=cancel", MARGIN, th - LINE_H, dim, 0);
    return;
  }

  char head[80];
  SDL_snprintf(head, sizeof(head), "SONGS  (%d)   current: %s", g.song_count,
               g.cur_song[0] ? g.cur_song : "(none)");
  inprint(rend, head, MARGIN, MARGIN, title, title);

  if (g.song_count == 0)
    inprint(rend, "No songs yet. Press N to create one.", MARGIN, MARGIN + LINE_H * 2, fg, 0);

  const int top = MARGIN + LINE_H + 2;
  const int rows = (th - top - LINE_H) / LINE_H;
  if (g.song_sel < g.song_scroll) g.song_scroll = g.song_sel;
  if (g.song_sel >= g.song_scroll + rows) g.song_scroll = g.song_sel - rows + 1;

  const int maxchars = (tw - 2 * MARGIN) / (gx > 0 ? gx : 6);
  for (int i = 0; i < rows; i++) {
    int idx = g.song_scroll + i;
    if (idx >= g.song_count) break;
    char line[80];
    trunc_copy(line, g.song_names[idx], maxchars);
    const int y = top + i * LINE_H;
    const bool selrow = (idx == g.song_sel);
    if (selrow) hl_bar(rend, 0, y, tw);
    inprint(rend, line, MARGIN, y, selrow ? 0x000000 : fg, 0);
  }
  inprint(rend, "Up/Dn  Enter=load  N=new  X=del  Esc=back", MARGIN, th - LINE_H, dim, 0);
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
  else if (g.mode == MODE_PARAMPICK)
    render_parampick(rend, texture_w, texture_h);
  else if (g.mode == MODE_SONGS)
    render_songs(rend, texture_w, texture_h);
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
