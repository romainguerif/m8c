// MIDI Control Change input for the recorder trigger.
// Released under the MIT licence, https://opensource.org/licenses/MIT
#include "midi_cc.h"
#include "recorder.h"

#include <SDL3/SDL.h>

#ifdef __APPLE__
// ---------------------------------------------------------------------------
// CoreMIDI implementation (macOS)
// ---------------------------------------------------------------------------
#include <CoreMIDI/CoreMIDI.h>

static MIDIClientRef g_client = 0;
static MIDIPortRef g_port = 0;
static MIDIEndpointRef g_source = 0;

// Parse a run of raw MIDI bytes, tracking running status. Dispatches the arm
// CC and transport Start/Continue/Stop to the recorder.
static void parse_midi_bytes(const uint8_t *data, int len) {
  static uint8_t status = 0;
  static uint8_t d1 = 0;
  static int expect = 0; // remaining data bytes for current message
  static int have = 0;   // data bytes collected so far

  for (int i = 0; i < len; i++) {
    const uint8_t b = data[i];
    if (b & 0x80) {
      // Real-time messages (0xF8-0xFF) may interleave without breaking running
      // status. Transport: Start=0xFA, Continue=0xFB, Stop=0xFC.
      if (b >= 0xF8) {
        if (b == 0xFA || b == 0xFB) {
          SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "midi_cc: transport %s",
                       b == 0xFA ? "START" : "CONTINUE");
          recorder_set_playing(true);
        } else if (b == 0xFC) {
          SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "midi_cc: transport STOP");
          recorder_set_playing(false);
        }
        continue;
      }
      status = b;
      have = 0;
      const uint8_t hi = b & 0xF0;
      expect = (hi == 0xC0 || hi == 0xD0) ? 1 : 2;
      if (b >= 0xF0)
        expect = 0; // system common: not tracked here
      continue;
    }
    // Data byte.
    if ((status & 0xF0) != 0xB0) {
      if (expect > 0 && ++have >= expect)
        have = 0;
      continue;
    }
    if (have == 0) {
      d1 = b;
      have = 1;
    } else {
      const int channel = (status & 0x0F) + 1;
      const int cc = d1, value = b;
      if (cc == recorder_arm_cc() &&
          (recorder_arm_channel() == 0 || channel == recorder_arm_channel())) {
        recorder_set_armed(value >= 64);
      }
      have = 0; // running status: next pair is another CC
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

  const ItemCount n = MIDIGetNumberOfSources();
  for (ItemCount i = 0; i < n; i++) {
    MIDIEndpointRef src = MIDIGetSource(i);
    if (src != 0 && source_name_matches_m8(src)) {
      g_source = src;
      break;
    }
  }
  if (g_source == 0) {
    SDL_Log("midi_cc: no M8 MIDI source found (CC trigger disabled, keyboard still works)");
    midi_cc_close();
    return false;
  }

  err = MIDIPortConnectSource(g_port, g_source, NULL);
  if (err != noErr) {
    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "midi_cc: connect source failed (%d)", (int)err);
    midi_cc_close();
    return false;
  }

  SDL_Log("midi_cc: listening on the M8 MIDI port (arm CC %d + transport)", recorder_arm_cc());
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
