// Copyright 2021 Jonne Kokkonen
// Released under the MIT licence, https://opensource.org/licenses/MIT
#ifndef M8_H_
#define M8_H_

#include "../config.h"

// Maximum number of M8s driven at once (dual-M8 mode). See docs/dual-m8.md.
#define M8_MAX_DEVICES 2

enum return_codes {
  DEVICE_DISCONNECTED = 0,
  DEVICE_PROCESSING = 1,
  DEVICE_FATAL_ERROR = -1
};

// Opens up to M8_MAX_DEVICES connected M8s (or just `preferred_device` if given).
// Returns the number of devices opened (0 = none).
int m8_initialize(int verbose, const char *preferred_device);
// Number of currently-open devices (0..M8_MAX_DEVICES).
int m8_device_count(void);
int m8_list_devices(void);
// Per-device operations. `dev` is a device index 0..m8_device_count()-1.
int m8_reset_display(int dev);
int m8_enable_display(int dev, unsigned char reset_display);
int m8_send_msg_controller(int dev, unsigned char input);
int m8_send_msg_keyjazz(int dev, unsigned char note, unsigned char velocity);
int m8_process_data(int dev, const config_params_s *conf);
int m8_pause_processing(void);
int m8_resume_processing(void);
int m8_close(void); // closes all open devices

#endif