// Copyright 2021 Jonne Kokkonen
// Released under the MIT licence, https://opensource.org/licenses/MIT

#ifndef RENDER_H_
#define RENDER_H_

#include "command.h"
#include "config.h"

#include <stdint.h>

int renderer_initialize(config_params_s *conf);
void renderer_close(void);
void renderer_set_font_mode(int mode);
void renderer_fix_texture_scaling_after_window_resize(config_params_s *conf);
void renderer_clear_screen(void);
void renderer_request_redraw(void);

void draw_waveform(struct draw_oscilloscope_waveform_command *command);
void draw_rectangle(struct draw_rectangle_command *command);
int draw_character(struct draw_character_command *command);

void set_m8_model(unsigned int model);

// Dual-M8 rendering. renderer_set_active_device() selects which M8's texture
// draw_* target; set the device count and focused device for the split-screen
// composite. See docs/dual-m8.md.
void renderer_set_active_device(int dev);
void renderer_set_device_count(int count);
void renderer_set_focus(int dev);

void render_screen(config_params_s *conf);
int toggle_fullscreen(config_params_s *conf);
void display_keyjazz_overlay(uint8_t show, uint8_t base_octave, uint8_t velocity);

void show_error_message(const char *message);

// Set the window title (used for the recorder's REC indicator).
void renderer_set_title(const char *title);

int screensaver_init(void);
void screensaver_draw(void);
void screensaver_destroy(void);

#endif
