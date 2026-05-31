// In-app overlay for the VST/AU plugin racks (3 send chains + master),
// styled like the m8c settings/log overlays. Lets the user scan, pick a plugin
// from the list, and load/bypass/reorder/edit it per bus.
// Released under the MIT licence, https://opensource.org/licenses/MIT
#ifndef PLUGIN_RACK_H_
#define PLUGIN_RACK_H_

#include <SDL3/SDL.h>
#include <stdbool.h>

struct app_context;

void plugin_rack_toggle_open(void);
bool plugin_rack_is_open(void);
void plugin_rack_handle_event(struct app_context *ctx, const SDL_Event *e);
void plugin_rack_render_overlay(SDL_Renderer *rend, int texture_w, int texture_h);
void plugin_rack_on_texture_size_change(SDL_Renderer *rend);

#endif
