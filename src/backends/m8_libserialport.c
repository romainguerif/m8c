// Copyright 2021 Jonne Kokkonen
// Released under the MIT licence, https://opensource.org/licenses/MIT

// Contains portions of code from libserialport's examples released to the
// public domain

#ifdef USE_LIBSERIALPORT
#include <SDL3/SDL.h>
#include <libserialport.h>
#include <stdlib.h>
#include <string.h>

#include "../command.h"
#include "../config.h"
#include "m8.h"
#include "queue.h"
#include "slip.h"

#define SERIAL_READ_SIZE 1024  // maximum amount of bytes to read from the serial in one pass
#define SERIAL_READ_DELAY_MS 4 // delay between serial reads in milliseconds

// Per-M8 connection state. Up to M8_MAX_DEVICES of these run at once (dual-M8
// mode); each owns its serial port, SLIP parser, message queue and reader
// thread. See docs/dual-m8.md.
typedef struct {
  struct sp_port *port;
  uint8_t serial_buffer[SERIAL_READ_SIZE];
  uint8_t slip_buffer[SERIAL_READ_SIZE];
  slip_handler_s slip;
  message_queue_s queue;
  SDL_Thread *thread;
  int should_stop;
  int index;
} m8_device_s;

static m8_device_s g_dev[M8_MAX_DEVICES];
static int g_count = 0;

int m8_device_count(void) { return g_count; }

// Helper function for error handling
static int check(enum sp_return result);

// SLIP's recv callback carries no context, so each device gets a tiny dedicated
// callback that pushes decoded messages to its own queue.
static int recv_to_queue_0(uint8_t *data, const uint32_t size) {
  push_message(&g_dev[0].queue, data, size);
  return 1;
}
static int recv_to_queue_1(uint8_t *data, const uint32_t size) {
  push_message(&g_dev[1].queue, data, size);
  return 1;
}
static int (*const recv_cb[M8_MAX_DEVICES])(uint8_t *, uint32_t) = {recv_to_queue_0,
                                                                    recv_to_queue_1};

static int detect_m8_serial_device(const struct sp_port *port, const char *preferred_device) {
  // Check the connection method - we want USB serial devices
  const enum sp_transport transport = sp_get_port_transport(port);

  if (transport == SP_TRANSPORT_USB) {
    // If a preferred device is specified, check if this port matches it
    if (preferred_device != NULL) {
      const char *port_name = sp_get_port_name(port);
      if (strcmp(preferred_device, port_name) == 0) {
        return 1; // Force return 1 for preferred device
      }
    }

    // Get the USB vendor and product IDs.
    int usb_vid, usb_pid;
    sp_get_port_usb_vid_pid(port, &usb_vid, &usb_pid);

    if (usb_vid == 0x16C0 && (usb_pid == 0x048A || usb_pid == 0x048B))
      return 1;
  }

  return 0;
}

// Checks whether the given device's port still exists among connected ports.
static int serial_port_connected(const m8_device_s *d) {
  if (d->port == NULL)
    return 0;

  int device_found = 0;
  struct sp_port **port_list;
  const enum sp_return result = sp_list_ports(&port_list);
  if (result != SP_OK) {
    SDL_LogError(SDL_LOG_CATEGORY_SYSTEM, "sp_list_ports() failed!\n");
    abort();
  }

  for (int i = 0; port_list[i] != NULL; i++) {
    const struct sp_port *port = port_list[i];
    if (detect_m8_serial_device(port, NULL)) {
      if (strcmp(sp_get_port_name(port), sp_get_port_name(d->port)) == 0)
        device_found = 1;
    }
  }

  sp_free_port_list(port_list);
  return device_found;
}

static void process_received_bytes(const uint8_t *buffer, int bytes_read, slip_handler_s *slip) {
  const uint8_t *cur = buffer;
  const uint8_t *end = buffer + bytes_read;
  while (cur < end) {
    const int slip_result = slip_read_byte(slip, *cur++);
    if (slip_result != SLIP_NO_ERROR) {
      SDL_LogError(SDL_LOG_CATEGORY_ERROR, "SLIP error %d", slip_result);
    }
  }
}

// Reader thread: one per device. On a read error it stops; the main loop reaps
// the disconnection (never frees the port from this thread to avoid races).
static int thread_process_serial_data(void *data) {
  m8_device_s *d = data;
  while (!d->should_stop) {
    const int bytes_read = sp_nonblocking_read(d->port, d->serial_buffer, SERIAL_READ_SIZE);
    if (bytes_read < 0) {
      SDL_LogCritical(SDL_LOG_CATEGORY_ERROR, "Error %d reading serial (device %d).", bytes_read,
                      d->index);
      d->should_stop = 1;
      return 0;
    }
    if (bytes_read > 0) {
      process_received_bytes(d->serial_buffer, bytes_read, &d->slip);
    }
    SDL_Delay(SERIAL_READ_DELAY_MS);
  }
  return 1;
}

static int configure_serial_port(struct sp_port *port) {
  if (check(sp_open(port, SP_MODE_READ_WRITE)) != SP_OK)
    return 0;
  if (check(sp_set_baudrate(port, 115200)) != SP_OK)
    return 0;
  if (check(sp_set_bits(port, 8)) != SP_OK)
    return 0;
  if (check(sp_set_parity(port, SP_PARITY_NONE)) != SP_OK)
    return 0;
  if (check(sp_set_stopbits(port, 1)) != SP_OK)
    return 0;
  if (check(sp_set_flowcontrol(port, SP_FLOWCONTROL_NONE)) != SP_OK)
    return 0;
  return 1;
}

// Helper function for error handling.
static int check(const enum sp_return result) {
  char *error_message;

  switch (result) {
  case SP_ERR_ARG:
    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Error: Invalid argument");
    break;
  case SP_ERR_FAIL:
    error_message = sp_last_error_message();
    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Error: Failed: %s", error_message);
    sp_free_error_message(error_message);
    break;
  case SP_ERR_SUPP:
    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Error: Not supported");
    break;
  case SP_ERR_MEM:
    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Error: Couldn't allocate memory");
    break;
  case SP_OK:
  default:
    break;
  }
  return result;
}

// Bring one already-configured device online: SLIP parser, queue, reader thread.
static int start_device(m8_device_s *d) {
  static const char *thread_names[M8_MAX_DEVICES] = {"SerialThread0", "SerialThread1"};
  const slip_descriptor_s slip_descriptor = {
      .buf = d->slip_buffer,
      .buf_size = sizeof(d->slip_buffer),
      .recv_message = recv_cb[d->index],
  };
  slip_init(&d->slip, &slip_descriptor);
  init_queue(&d->queue);
  d->should_stop = 0;
  d->thread = SDL_CreateThread(thread_process_serial_data,
                               thread_names[d->index < M8_MAX_DEVICES ? d->index : 0], d);
  if (!d->thread) {
    SDL_LogError(SDL_LOG_CATEGORY_SYSTEM, "SDL_CreateThread Error: %s", SDL_GetError());
    return 0;
  }
  return 1;
}

// Tear down one device (main thread only). Sends the disconnect byte, joins the
// reader thread, frees the queue and port.
static int disconnect_device(m8_device_s *d) {
  if (d->port == NULL)
    return 0;
  SDL_Log("Disconnecting M8 (device %d)", d->index);

  d->should_stop = 1;
  if (d->thread != NULL) {
    SDL_WaitThread(d->thread, NULL);
    d->thread = NULL;
  }
  destroy_queue(&d->queue);

  const unsigned char buf[1] = {'D'};
  int result = sp_blocking_write(d->port, buf, 1, 5);
  if (result != 1) {
    SDL_LogError(SDL_LOG_CATEGORY_SYSTEM, "Error sending disconnect, code %d", result);
    result = 0;
  }
  sp_close(d->port);
  sp_free_port(d->port);
  d->port = NULL;
  return result;
}

int m8_initialize(const int verbose, const char *preferred_device) {
  if (g_count > 0) {
    // Already initialized
    return g_count;
  }

  if (verbose) {
    SDL_Log("Looking for USB serial devices");
  }

  // Open every detected M8 (up to M8_MAX_DEVICES) for dual-M8 mode. A specific
  // `--dev` forces a single device (handled by the is_preferred break below).
  const int max_open = (preferred_device != NULL) ? 1 : M8_MAX_DEVICES;

  struct sp_port **port_list;
  const enum sp_return port_result = sp_list_ports(&port_list);
  if (port_result != SP_OK) {
    SDL_LogError(SDL_LOG_CATEGORY_SYSTEM, "sp_list_ports() failed!");
    return 0;
  }

  g_count = 0;
  for (int i = 0; port_list[i] != NULL && g_count < max_open; i++) {
    const struct sp_port *port = port_list[i];
    if (!detect_m8_serial_device(port, preferred_device))
      continue;

    const char *port_name = sp_get_port_name(port);
    const int is_preferred = (preferred_device != NULL && strcmp(preferred_device, port_name) == 0);

    m8_device_s *d = &g_dev[g_count];
    memset(d, 0, sizeof(*d));
    d->index = g_count;
    if (sp_copy_port(port, &d->port) != SP_OK || d->port == NULL)
      continue;

    SDL_Log("Found M8 in %s (device %d)", port_name, d->index);
    if (!configure_serial_port(d->port) || !start_device(d)) {
      sp_free_port(d->port);
      d->port = NULL;
      continue;
    }
    g_count++;

    if (is_preferred) {
      // Honour --dev: open only the requested device.
      break;
    }
  }

  sp_free_port_list(port_list);

  if (g_count == 0 && verbose) {
    SDL_LogError(SDL_LOG_CATEGORY_SYSTEM, "Cannot find a M8");
  }
  return g_count;
}

static int send_ping(const m8_device_s *d) {
  SDL_LogDebug(SDL_LOG_CATEGORY_SYSTEM, "Sending ping");
  const unsigned char buf[1] = {'X'};
  const int result = sp_blocking_write(d->port, buf, 1, 5);
  if (result != 1) {
    SDL_LogError(SDL_LOG_CATEGORY_SYSTEM, "Error sending ping, code %d", result);
    return 0;
  }
  return 1;
}

int m8_send_msg_controller(const int dev, const uint8_t input) {
  if (dev < 0 || dev >= g_count || g_dev[dev].port == NULL)
    return -1;
  const unsigned char buf[2] = {'C', input};
  const int result = sp_blocking_write(g_dev[dev].port, buf, 2, 5);
  if (result != 2) {
    SDL_LogError(SDL_LOG_CATEGORY_SYSTEM, "Error sending input, code %d", result);
    return -1;
  }
  return 1;
}

int m8_send_msg_keyjazz(const int dev, const uint8_t note, uint8_t velocity) {
  if (dev < 0 || dev >= g_count || g_dev[dev].port == NULL)
    return -1;

  if (velocity > 0x7F)
    velocity = 0x7F;

  // Special case for note off
  if (note == 0xFF && velocity == 0x00) {
    const unsigned char buf[2] = {'K', 0xFF};
    const int result = sp_blocking_write(g_dev[dev].port, buf, 2, 5);
    if (result != 2) {
      SDL_LogError(SDL_LOG_CATEGORY_SYSTEM, "Error sending keyjazz, code %d", result);
      return -1;
    }
    return 1;
  }

  // Regular note on message
  const unsigned char buf[3] = {'K', note, velocity};
  const int result = sp_blocking_write(g_dev[dev].port, buf, 3, 5);
  if (result != 3) {
    SDL_LogError(SDL_LOG_CATEGORY_SYSTEM, "Error sending keyjazz, code %d", result);
    return -1;
  }
  return 1;
}

int m8_list_devices() {
  struct sp_port **port_list;
  const enum sp_return result = sp_list_ports(&port_list);

  if (result != SP_OK) {
    SDL_LogError(SDL_LOG_CATEGORY_SYSTEM, "sp_list_ports() failed!\n");
    return 1;
  }

  int devices_found = 0;
  for (int i = 0; port_list[i] != NULL; i++) {
    const struct sp_port *port = port_list[i];

    if (detect_m8_serial_device(port, NULL)) {
      SDL_Log("Found M8 device: %s", sp_get_port_name(port));
      devices_found++;
    }
  }
  if (devices_found == 0) {
    SDL_LogInfo(SDL_LOG_CATEGORY_SYSTEM, "No M8 devices found");
    return 0;
  }

  sp_free_port_list(port_list);
  return 0;
}

int m8_reset_display(const int dev) {
  if (dev < 0 || dev >= g_count || g_dev[dev].port == NULL)
    return 0;
  SDL_Log("Reset display (device %d)", dev);
  const unsigned char buf[1] = {'R'};
  const int result = sp_blocking_write(g_dev[dev].port, buf, 1, 5);
  if (result != 1) {
    SDL_LogError(SDL_LOG_CATEGORY_SYSTEM, "Error resetting M8 display, code %d", result);
    return 0;
  }
  return 1;
}

int m8_enable_display(const int dev, const unsigned char reset_display) {
  if (dev < 0 || dev >= g_count || g_dev[dev].port == NULL)
    return 0;
  SDL_Log("Enabling and resetting M8 display (device %d)", dev);

  const char buf_enable[1] = {'E'};
  int result = sp_blocking_write(g_dev[dev].port, buf_enable, 1, 5);
  if (result != 1) {
    SDL_LogError(SDL_LOG_CATEGORY_SYSTEM, "Error enabling M8 display, code %d", result);
    return 0;
  }

  // Wait for things to warm up
  SDL_Delay(500);

  if (reset_display) {
    result = m8_reset_display(dev);
  }

  return result;
}

int m8_process_data(const int dev, const config_params_s *conf) {
  static unsigned int empty_cycles[M8_MAX_DEVICES] = {0};
  static message_batch_s batch;

  if (dev < 0 || dev >= g_count || g_dev[dev].port == NULL) {
    return DEVICE_DISCONNECTED;
  }
  m8_device_s *d = &g_dev[dev];

  if (pop_all_messages(&d->queue, &batch) > 0) {
    empty_cycles[dev] = 0;
    for (unsigned int i = 0; i < batch.count; i++) {
      if (batch.lengths[i] > 0) {
        process_command(batch.messages[i], batch.lengths[i]);
      }
      SDL_free(batch.messages[i]);
    }
  } else {
    empty_cycles[dev]++;
    if (empty_cycles[dev] >= conf->wait_packets) {
      // try checking if the device is still alive
      if (serial_port_connected(d)) {
        if (!send_ping(d)) {
          SDL_LogError(SDL_LOG_CATEGORY_SYSTEM, "Failed to ping device on reconnect");
          disconnect_device(d);
          return DEVICE_DISCONNECTED;
        }
        empty_cycles[dev] = 0;
        return DEVICE_PROCESSING;
      }
      SDL_LogError(SDL_LOG_CATEGORY_SYSTEM,
                   "No messages received for %d cycles, assuming device disconnected",
                   empty_cycles[dev]);
      empty_cycles[dev] = 0;
      disconnect_device(d);
      return DEVICE_DISCONNECTED;
    }
  }
  return DEVICE_PROCESSING;
}

int m8_close() {
  for (int i = 0; i < g_count; i++)
    disconnect_device(&g_dev[i]);
  g_count = 0;
  return 1;
}

// These shouldn't be needed with serial
int m8_pause_processing(void) { return 1; }
int m8_resume_processing(void) { return 1; }
#endif
