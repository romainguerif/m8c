// MIDI Control Change input for the recorder trigger.
// Released under the MIT licence, https://opensource.org/licenses/MIT
#include "midi_cc.h"
#include "juce_host.h"
#include "recorder.h"

#include <SDL3/SDL.h>

// Send-amount CCs: channel 1-8 = M8 track, CC20/21/22 = send bus 0/1/2.
#define SEND_CC_BASE 20

#ifdef __APPLE__
// ---------------------------------------------------------------------------
// CoreMIDI implementation (macOS)
// ---------------------------------------------------------------------------
#include <CoreMIDI/CoreMIDI.h>

static MIDIClientRef g_client = 0;
static MIDIPortRef g_port = 0;
static MIDIEndpointRef g_source = 0;

// Dispatch one complete channel-voice MIDI message: forward it to the plugin
// host (so instruments play, routed by channel) and apply our arm/send CCs.
static void dispatch_channel_message(uint8_t status, uint8_t d0, uint8_t d1, int ndata) {
  uint8_t bytes[3] = {status, d0, d1};
  juce_host_push_midi(bytes, 1 + ndata); // routed to instrument lanes by channel

  if ((status & 0xF0) == 0xB0) { // Control Change
    const int channel = (status & 0x0F) + 1, cc = d0, value = d1;
    if (cc == recorder_arm_cc() &&
        (recorder_arm_channel() == 0 || channel == recorder_arm_channel())) {
      recorder_set_armed(value >= 64);
    }
    // Per-track send amounts: channel 1-8 = track, CC20/21/22 = send bus.
    const int bus = cc - SEND_CC_BASE;
    if (bus >= 0 && bus < JUCE_HOST_NUM_SENDS && channel >= 1 && channel <= JUCE_HOST_NUM_TRACKS) {
      juce_host_set_send(channel - 1, bus, (float)value / 127.0f);
    }
  }
}

// Parse a run of raw MIDI bytes (running status aware) into complete messages.
static void parse_midi_bytes(const uint8_t *data, int len) {
  static uint8_t status = 0, d[2] = {0, 0};
  static int expect = 0, have = 0;

  for (int i = 0; i < len; i++) {
    const uint8_t b = data[i];
    if (b & 0x80) {
      // Real-time (0xF8-0xFF) may interleave without breaking running status.
      if (b >= 0xF8) {
        if (b == 0xFA || b == 0xFB)
          recorder_set_playing(true); // transport Start/Continue
        else if (b == 0xFC)
          recorder_set_playing(false); // transport Stop
        continue;
      }
      const uint8_t hi = b & 0xF0;
      if (b >= 0xF0) { // system common: drop, reset running status
        status = 0;
        expect = have = 0;
      } else {
        status = b;
        have = 0;
        expect = (hi == 0xC0 || hi == 0xD0) ? 1 : 2;
      }
      continue;
    }
    // Data byte.
    if (status == 0 || expect == 0)
      continue;
    d[have++] = b;
    if (have >= expect) {
      have = 0;
      dispatch_channel_message(status, d[0], expect == 2 ? d[1] : 0, expect);
    }
  }
}

static void midi_read_block(const MIDIPacketList *pktlist, void *src_conn_ref) {
  (void)src_conn_ref;
  const MIDIPacket *packet = &pktlist->packet[0];
  for (unsigned int i = 0; i < pktlist->numPackets; i++) {
    parse_midi_bytes(packet->data, packet->length);
    packet = MIDIPacketNext(packet);
  }
}

static bool source_name_matches_m8(MIDIEndpointRef src) {
  CFStringRef name = NULL;
  if (MIDIObjectGetStringProperty(src, kMIDIPropertyDisplayName, &name) != noErr || name == NULL) {
    if (MIDIObjectGetStringProperty(src, kMIDIPropertyName, &name) != noErr || name == NULL)
      return false;
  }
  char buf[128] = {0};
  CFStringGetCString(name, buf, sizeof(buf), kCFStringEncodingUTF8);
  CFRelease(name);
  return SDL_strstr(buf, "M8") != NULL;
}

bool midi_cc_open(void) {
  if (g_client != 0)
    return true;

  OSStatus err = MIDIClientCreate(CFSTR("m8c"), NULL, NULL, &g_client);
  if (err != noErr) {
    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "midi_cc: MIDIClientCreate failed (%d)", (int)err);
    return false;
  }

  err = MIDIInputPortCreateWithBlock(g_client, CFSTR("m8c_cc_in"), &g_port,
                                     ^(const MIDIPacketList *pktlist, void *srcConnRefCon) {
                                       midi_read_block(pktlist, srcConnRefCon);
                                     });
  if (err != noErr) {
    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "midi_cc: input port create failed (%d)", (int)err);
    midi_cc_close();
    return false;
  }

  // Connect ALL MIDI sources: the M8 (arm/transport/sends + its musical MIDI)
  // and any hardware keyboard, so instruments can be played from either.
  const ItemCount n = MIDIGetNumberOfSources();
  int connected = 0;
  for (ItemCount i = 0; i < n; i++) {
    MIDIEndpointRef src = MIDIGetSource(i);
    if (src == 0)
      continue;
    char name[128] = {0};
    CFStringRef cn = NULL;
    if (MIDIObjectGetStringProperty(src, kMIDIPropertyDisplayName, &cn) == noErr && cn) {
      CFStringGetCString(cn, name, sizeof(name), kCFStringEncodingUTF8);
      CFRelease(cn);
    }
    if (MIDIPortConnectSource(g_port, src, NULL) == noErr) {
      connected++;
      SDL_Log("midi_cc: connected source '%s'%s", name,
              source_name_matches_m8(src) ? " (M8)" : "");
    }
  }
  if (connected == 0) {
    SDL_Log("midi_cc: no MIDI sources found");
    midi_cc_close();
    return false;
  }

  SDL_Log("midi_cc: listening on %d source(s) (arm CC %d, transport, instruments)", connected,
          recorder_arm_cc());
  return true;
}

void midi_cc_close(void) {
  if (g_port != 0 && g_source != 0)
    MIDIPortDisconnectSource(g_port, g_source);
  if (g_port != 0)
    MIDIPortDispose(g_port);
  if (g_client != 0)
    MIDIClientDispose(g_client);
  g_port = 0;
  g_client = 0;
  g_source = 0;
}

#else
// ---------------------------------------------------------------------------
// Stub for non-Apple platforms (TODO: ALSA seq / rtmidi backend).
// ---------------------------------------------------------------------------
bool midi_cc_open(void) {
  SDL_Log("midi_cc: not implemented on this platform; use the keyboard record key");
  return false;
}
void midi_cc_close(void) {}
#endif
