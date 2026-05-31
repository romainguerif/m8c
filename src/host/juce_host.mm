// JUCE plugin host for m8c: 3 stereo send chains + master, fed by the M8
// track stems, returning a processed master to the M8.
// Released under the MIT licence, https://opensource.org/licenses/MIT
#include "juce_host.h"

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_events/juce_events.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <atomic>
#include <cstdio>
#include <memory>
#include <mutex>
#include <vector>

namespace {

constexpr int kMaxDelay = 16384; // PDC ring size (~371ms @44.1k)
constexpr int kSendCap = 4;
constexpr int kMasterCap = 8;

// ---- A simple per-channel delay line for PDC alignment ----
struct DelayLine {
  juce::AudioBuffer<float> ring;
  int size = 0, writePos = 0, delay = 0;
  void prepare(int channels, int maxDelay) {
    size = maxDelay + 1;
    ring.setSize(channels, size);
    ring.clear();
    writePos = 0;
    delay = 0;
  }
  void setDelay(int d) {
    if (d < 0) d = 0;
    if (d > size - 1) d = size - 1;
    delay = d;
  }
  void process(juce::AudioBuffer<float> &buf) {
    const int n = buf.getNumSamples();
    const int ch = juce::jmin(buf.getNumChannels(), ring.getNumChannels());
    for (int c = 0; c < ch; c++) {
      float *data = buf.getWritePointer(c);
      float *r = ring.getWritePointer(c);
      int w = writePos;
      for (int i = 0; i < n; i++) {
        r[w] = data[i];
        int rd = w - delay;
        if (rd < 0) rd += size;
        data[i] = r[rd];
        if (++w >= size) w = 0;
      }
    }
    writePos += n;
    while (writePos >= size) writePos -= size;
  }
};

struct Slot {
  std::unique_ptr<juce::AudioPluginInstance> plugin;
  juce::PluginDescription desc;
  bool bypassed = false;
  int latency = 0;
};

struct Bus {
  std::vector<Slot> slots;
  int capacity = kSendCap;
  int latency = 0;          // sum of non-bypassed slot latencies
  DelayLine align;          // applied to this bus' wet output (sends only)
  juce::AudioBuffer<float> buf; // stereo work buffer
};

// ---- Host state ----
std::unique_ptr<juce::ScopedJuceInitialiser_GUI> g_juce;
juce::AudioPluginFormatManager g_formats;
juce::KnownPluginList g_known;
std::atomic<bool> g_scanning{false};

std::mutex g_lock;        // guards buses + prepared state
Bus g_buses[JUCE_HOST_NUM_BUSES];
std::atomic<float> g_send[JUCE_HOST_NUM_TRACKS][JUCE_HOST_NUM_SENDS];

bool g_prepared = false;
double g_sr = 44100.0;
int g_maxBlock = 4096;
int g_totalLatency = 0;

DelayLine g_dryDelay;
juce::AudioBuffer<float> g_dry, g_masterIn;

std::vector<std::unique_ptr<juce::DocumentWindow>> g_editorWindows;

// ---- helpers (must hold g_lock) ----
void recompute_latency_locked() {
  int maxSend = 0;
  for (int b = 0; b < JUCE_HOST_NUM_SENDS; b++) {
    int lat = 0;
    for (auto &s : g_buses[b].slots)
      if (!s.bypassed) lat += s.latency;
    g_buses[b].latency = lat;
    maxSend = juce::jmax(maxSend, lat);
  }
  int masterLat = 0;
  for (auto &s : g_buses[JUCE_HOST_BUS_MASTER].slots)
    if (!s.bypassed) masterLat += s.latency;
  g_buses[JUCE_HOST_BUS_MASTER].latency = masterLat;
  g_totalLatency = maxSend + masterLat;
}

void prepare_plugin_locked(Slot &s) {
  if (!s.plugin) return;
  s.plugin->enableAllBuses();
  s.plugin->setPlayConfigDetails(2, 2, g_sr, g_maxBlock);
  s.plugin->prepareToPlay(g_sr, g_maxBlock);
  s.latency = s.plugin->getLatencySamples();
  // Warm up off the audio thread (first call may allocate).
  juce::AudioBuffer<float> tmp(2, g_maxBlock);
  juce::MidiBuffer midi;
  tmp.clear();
  s.plugin->processBlock(tmp, midi);
}

void run_chain_locked(Bus &bus, juce::AudioBuffer<float> &buf) {
  juce::MidiBuffer midi;
  for (auto &s : bus.slots) {
    if (s.bypassed || !s.plugin) continue;
    midi.clear();
    s.plugin->processBlock(buf, midi);
  }
}

} // namespace

// ===========================================================================
// Lifecycle
// ===========================================================================
extern "C" void juce_host_init(void) {
  if (g_juce) return;
  g_juce = std::make_unique<juce::ScopedJuceInitialiser_GUI>();
  // Add formats explicitly (the headless AudioPluginFormatManager deletes
  // addDefaultFormats()).
#if JUCE_PLUGINHOST_VST3
  g_formats.addFormat(new juce::VST3PluginFormat());
#endif
#if JUCE_PLUGINHOST_AU && JUCE_MAC
  g_formats.addFormat(new juce::AudioUnitPluginFormat());
#endif
  for (int b = 0; b < JUCE_HOST_NUM_BUSES; b++)
    g_buses[b].capacity = (b == JUCE_HOST_BUS_MASTER) ? kMasterCap : kSendCap;
  for (int t = 0; t < JUCE_HOST_NUM_TRACKS; t++)
    for (int b = 0; b < JUCE_HOST_NUM_SENDS; b++)
      g_send[t][b].store(0.0f);
  std::fprintf(stderr, "[juce_host] init — JUCE %d.%d.%d, %d formats\n", JUCE_MAJOR_VERSION,
               JUCE_MINOR_VERSION, JUCE_BUILDNUMBER, g_formats.getNumFormats());
}

extern "C" void juce_host_pump(void) {}

extern "C" void juce_host_shutdown(void) {
  {
    std::lock_guard<std::mutex> lk(g_lock);
    g_editorWindows.clear();
    for (int b = 0; b < JUCE_HOST_NUM_BUSES; b++)
      g_buses[b].slots.clear();
    g_prepared = false;
  }
  g_juce.reset();
  std::fprintf(stderr, "[juce_host] shutdown\n");
}

// ===========================================================================
// Prepare
// ===========================================================================
extern "C" void juce_host_prepare(double sample_rate, int max_block_frames) {
  std::lock_guard<std::mutex> lk(g_lock);
  g_sr = sample_rate;
  g_maxBlock = juce::jmax(64, max_block_frames);
  for (int b = 0; b < JUCE_HOST_NUM_BUSES; b++) {
    g_buses[b].buf.setSize(2, g_maxBlock);
    g_buses[b].align.prepare(2, kMaxDelay);
    for (auto &s : g_buses[b].slots) prepare_plugin_locked(s);
  }
  g_dry.setSize(2, g_maxBlock);
  g_masterIn.setSize(2, g_maxBlock);
  g_dryDelay.prepare(2, kMaxDelay);
  recompute_latency_locked();
  g_prepared = true;
  std::fprintf(stderr, "[juce_host] prepared %.0f Hz, max block %d\n", g_sr, g_maxBlock);
}

// ===========================================================================
// Scanning
// ===========================================================================
extern "C" void juce_host_scan(void) {
  g_scanning.store(true);
  std::fprintf(stderr, "[juce_host] scanning plugins...\n");
  juce::File dead = juce::File::getSpecialLocation(juce::File::tempDirectory)
                        .getChildFile("m8c_plugin_scan.tmp");
  for (int i = 0; i < g_formats.getNumFormats(); i++) {
    juce::AudioPluginFormat *fmt = g_formats.getFormat(i);
    juce::PluginDirectoryScanner scanner(g_known, *fmt, fmt->getDefaultLocationsToSearch(),
                                         true, dead, false);
    juce::String name;
    while (scanner.scanNextFile(true, name)) { /* progress: name */ }
  }
  g_scanning.store(false);
  std::fprintf(stderr, "[juce_host] scan done: %d plugins known\n", g_known.getNumTypes());
}

extern "C" bool juce_host_is_scanning(void) { return g_scanning.load(); }

extern "C" int juce_host_known_count(void) { return g_known.getNumTypes(); }

extern "C" void juce_host_known_label(int index, char *out, int out_size) {
  out[0] = '\0';
  auto types = g_known.getTypes();
  if (index < 0 || index >= types.size()) return;
  const auto &d = types.getReference(index);
  juce::String s = d.name + " [" + d.pluginFormatName + "]";
  s.copyToUTF8(out, (size_t)out_size);
}

// ===========================================================================
// Bus / slot management
// ===========================================================================
extern "C" int juce_host_bus_capacity(int bus) {
  if (bus < 0 || bus >= JUCE_HOST_NUM_BUSES) return 0;
  return g_buses[bus].capacity;
}

extern "C" int juce_host_bus_slot_count(int bus) {
  if (bus < 0 || bus >= JUCE_HOST_NUM_BUSES) return 0;
  std::lock_guard<std::mutex> lk(g_lock);
  return (int)g_buses[bus].slots.size();
}

extern "C" int juce_host_bus_add(int bus, int known_index) {
  if (bus < 0 || bus >= JUCE_HOST_NUM_BUSES) return -1;
  auto types = g_known.getTypes();
  if (known_index < 0 || known_index >= types.size()) return -1;
  juce::PluginDescription desc = types.getReference(known_index);

  juce::String err;
  std::unique_ptr<juce::AudioPluginInstance> inst =
      g_formats.createPluginInstance(desc, g_sr, g_maxBlock, err);
  if (!inst) {
    std::fprintf(stderr, "[juce_host] load failed: %s\n", err.toRawUTF8());
    return -1;
  }

  std::lock_guard<std::mutex> lk(g_lock);
  if ((int)g_buses[bus].slots.size() >= g_buses[bus].capacity) return -1;
  Slot s;
  s.plugin = std::move(inst);
  s.desc = desc;
  if (g_prepared) prepare_plugin_locked(s);
  g_buses[bus].slots.push_back(std::move(s));
  recompute_latency_locked();
  std::fprintf(stderr, "[juce_host] bus %d += %s\n", bus, desc.name.toRawUTF8());
  return (int)g_buses[bus].slots.size() - 1;
}

extern "C" void juce_host_bus_remove(int bus, int slot) {
  if (bus < 0 || bus >= JUCE_HOST_NUM_BUSES) return;
  std::lock_guard<std::mutex> lk(g_lock);
  auto &v = g_buses[bus].slots;
  if (slot < 0 || slot >= (int)v.size()) return;
  v.erase(v.begin() + slot);
  recompute_latency_locked();
}

extern "C" void juce_host_bus_move(int bus, int slot, int delta) {
  if (bus < 0 || bus >= JUCE_HOST_NUM_BUSES) return;
  std::lock_guard<std::mutex> lk(g_lock);
  auto &v = g_buses[bus].slots;
  int j = slot + delta;
  if (slot < 0 || slot >= (int)v.size() || j < 0 || j >= (int)v.size()) return;
  std::swap(v[slot], v[j]);
}

extern "C" void juce_host_slot_label(int bus, int slot, char *out, int out_size) {
  out[0] = '\0';
  if (bus < 0 || bus >= JUCE_HOST_NUM_BUSES) return;
  std::lock_guard<std::mutex> lk(g_lock);
  auto &v = g_buses[bus].slots;
  if (slot < 0 || slot >= (int)v.size()) return;
  v[slot].desc.name.copyToUTF8(out, (size_t)out_size);
}

extern "C" void juce_host_slot_set_bypass(int bus, int slot, bool bypassed) {
  if (bus < 0 || bus >= JUCE_HOST_NUM_BUSES) return;
  std::lock_guard<std::mutex> lk(g_lock);
  auto &v = g_buses[bus].slots;
  if (slot < 0 || slot >= (int)v.size()) return;
  v[slot].bypassed = bypassed;
  recompute_latency_locked();
}

extern "C" bool juce_host_slot_is_bypassed(int bus, int slot) {
  if (bus < 0 || bus >= JUCE_HOST_NUM_BUSES) return false;
  std::lock_guard<std::mutex> lk(g_lock);
  auto &v = g_buses[bus].slots;
  if (slot < 0 || slot >= (int)v.size()) return false;
  return v[slot].bypassed;
}

namespace {
struct EditorWindow : public juce::DocumentWindow {
  EditorWindow(const juce::String &name, juce::AudioProcessorEditor *ed)
      : juce::DocumentWindow(name, juce::Colours::black, juce::DocumentWindow::allButtons) {
    setUsingNativeTitleBar(true);
    setContentOwned(ed, true);
    setResizable(ed->isResizable(), false);
    centreWithSize(getWidth(), getHeight());
    setVisible(true);
  }
  void closeButtonPressed() override {
    // Remove self from the open-window list (deletes editor via content owned).
    for (auto it = g_editorWindows.begin(); it != g_editorWindows.end(); ++it)
      if (it->get() == this) { g_editorWindows.erase(it); return; }
  }
};
} // namespace

extern "C" void juce_host_slot_open_editor(int bus, int slot) {
  if (bus < 0 || bus >= JUCE_HOST_NUM_BUSES) return;
  juce::AudioPluginInstance *plugin = nullptr;
  juce::String title;
  {
    std::lock_guard<std::mutex> lk(g_lock);
    auto &v = g_buses[bus].slots;
    if (slot < 0 || slot >= (int)v.size() || !v[slot].plugin) return;
    plugin = v[slot].plugin.get();
    title = v[slot].desc.name;
  }
  juce::AudioProcessorEditor *ed = plugin->createEditorIfNeeded();
  if (ed == nullptr) ed = new juce::GenericAudioProcessorEditor(*plugin);
  g_editorWindows.push_back(std::make_unique<EditorWindow>(title, ed));
}

extern "C" int juce_host_latency_samples(void) { return g_totalLatency; }

// ===========================================================================
// Send gains
// ===========================================================================
extern "C" void juce_host_set_send(int track, int bus, float gain) {
  if (track < 0 || track >= JUCE_HOST_NUM_TRACKS || bus < 0 || bus >= JUCE_HOST_NUM_SENDS) return;
  g_send[track][bus].store(juce::jlimit(0.0f, 1.0f, gain));
}
extern "C" float juce_host_get_send(int track, int bus) {
  if (track < 0 || track >= JUCE_HOST_NUM_TRACKS || bus < 0 || bus >= JUCE_HOST_NUM_SENDS) return 0;
  return g_send[track][bus].load();
}

extern "C" bool juce_host_is_active(void) { return g_prepared; }

// ===========================================================================
// Realtime processing
// ===========================================================================
extern "C" bool juce_host_process(const float *in24, int channels, int frames, float *out) {
  // Dry fallback: master pair = channels 0,1.
  auto passthrough = [&]() {
    for (int i = 0; i < frames; i++) {
      out[2 * i] = in24[i * channels + 0];
      out[2 * i + 1] = in24[i * channels + 1];
    }
  };

  std::unique_lock<std::mutex> lk(g_lock, std::try_to_lock);
  if (!lk.owns_lock() || !g_prepared) {
    passthrough();
    return true;
  }
  if (frames > g_maxBlock) frames = g_maxBlock;

  // Dry = master pair (ch 0,1).
  g_dry.setSize(2, frames, false, false, true);
  for (int i = 0; i < frames; i++) {
    g_dry.getWritePointer(0)[i] = in24[i * channels + 0];
    g_dry.getWritePointer(1)[i] = in24[i * channels + 1];
  }

  // Build + process the 3 send buses from track stems (ch 2..17).
  int maxSend = 0;
  for (int b = 0; b < JUCE_HOST_NUM_SENDS; b++)
    maxSend = juce::jmax(maxSend, g_buses[b].latency);

  for (int b = 0; b < JUCE_HOST_NUM_SENDS; b++) {
    Bus &bus = g_buses[b];
    bus.buf.setSize(2, frames, false, false, true); // logical length = frames, no realloc
    bus.buf.clear();
    float *l = bus.buf.getWritePointer(0);
    float *r = bus.buf.getWritePointer(1);
    for (int t = 0; t < JUCE_HOST_NUM_TRACKS; t++) {
      const float g = g_send[t][b].load();
      if (g <= 0.0f) continue;
      const int cl = 2 + 2 * t, cr = 3 + 2 * t;
      if (cr >= channels) break;
      for (int i = 0; i < frames; i++) {
        l[i] += in24[i * channels + cl] * g;
        r[i] += in24[i * channels + cr] * g;
      }
    }
    run_chain_locked(bus, bus.buf);
    bus.align.setDelay(maxSend - bus.latency);
    bus.align.process(bus.buf);
  }

  // Align dry to the longest send path, then sum into master input.
  g_dryDelay.setDelay(maxSend);
  g_dryDelay.process(g_dry);

  g_masterIn.setSize(2, frames, false, false, true);
  g_masterIn.copyFrom(0, 0, g_dry, 0, 0, frames);
  g_masterIn.copyFrom(1, 0, g_dry, 1, 0, frames);
  for (int b = 0; b < JUCE_HOST_NUM_SENDS; b++) {
    g_masterIn.addFrom(0, 0, g_buses[b].buf, 0, 0, frames);
    g_masterIn.addFrom(1, 0, g_buses[b].buf, 1, 0, frames);
  }

  // Master chain.
  run_chain_locked(g_buses[JUCE_HOST_BUS_MASTER], g_masterIn);

  const float *ml = g_masterIn.getReadPointer(0);
  const float *mr = g_masterIn.getReadPointer(1);
  for (int i = 0; i < frames; i++) {
    out[2 * i] = ml[i];
    out[2 * i + 1] = mr[i];
  }
  return true;
}

// ===========================================================================
// Persistence (XML: bus/slot descriptions + state + send gains)
// ===========================================================================
extern "C" void juce_host_save(const char *path) {
  std::lock_guard<std::mutex> lk(g_lock);
  juce::XmlElement root("M8C_HOST");
  for (int b = 0; b < JUCE_HOST_NUM_BUSES; b++) {
    auto *busXml = root.createNewChildElement("BUS");
    busXml->setAttribute("id", b);
    for (auto &s : g_buses[b].slots) {
      auto *slotXml = busXml->createNewChildElement("SLOT");
      slotXml->setAttribute("bypassed", s.bypassed);
      if (auto descXml = s.desc.createXml())
        slotXml->addChildElement(descXml.release());
      if (s.plugin) {
        juce::MemoryBlock mb;
        s.plugin->getStateInformation(mb);
        slotXml->setAttribute("state", mb.toBase64Encoding());
      }
    }
  }
  auto *sendsXml = root.createNewChildElement("SENDS");
  for (int t = 0; t < JUCE_HOST_NUM_TRACKS; t++)
    for (int b = 0; b < JUCE_HOST_NUM_SENDS; b++)
      sendsXml->setAttribute("s" + juce::String(t) + "_" + juce::String(b), g_send[t][b].load());
  root.writeTo(juce::File(juce::String::fromUTF8(path)));
}

extern "C" void juce_host_load(const char *path) {
  juce::File f(juce::String::fromUTF8(path));
  if (!f.existsAsFile()) return;
  std::unique_ptr<juce::XmlElement> root(juce::XmlDocument::parse(f));
  if (!root || !root->hasTagName("M8C_HOST")) return;

  std::lock_guard<std::mutex> lk(g_lock);
  for (int b = 0; b < JUCE_HOST_NUM_BUSES; b++) g_buses[b].slots.clear();

  for (auto *busXml : root->getChildWithTagNameIterator("BUS")) {
    int b = busXml->getIntAttribute("id", -1);
    if (b < 0 || b >= JUCE_HOST_NUM_BUSES) continue;
    for (auto *slotXml : busXml->getChildWithTagNameIterator("SLOT")) {
      auto *descXml = slotXml->getChildByName("PLUGIN");
      juce::PluginDescription desc;
      if (descXml == nullptr || !desc.loadFromXml(*descXml)) continue;
      juce::String err;
      auto inst = g_formats.createPluginInstance(desc, g_sr, g_maxBlock, err);
      if (!inst) continue;
      Slot s;
      s.plugin = std::move(inst);
      s.desc = desc;
      s.bypassed = slotXml->getBoolAttribute("bypassed", false);
      juce::String state = slotXml->getStringAttribute("state");
      if (state.isNotEmpty()) {
        juce::MemoryBlock mb;
        mb.fromBase64Encoding(state);
        s.plugin->setStateInformation(mb.getData(), (int)mb.getSize());
      }
      if (g_prepared) prepare_plugin_locked(s);
      if ((int)g_buses[b].slots.size() < g_buses[b].capacity)
        g_buses[b].slots.push_back(std::move(s));
    }
  }
  if (auto *sendsXml = root->getChildByName("SENDS"))
    for (int t = 0; t < JUCE_HOST_NUM_TRACKS; t++)
      for (int b = 0; b < JUCE_HOST_NUM_SENDS; b++)
        g_send[t][b].store(
            (float)sendsXml->getDoubleAttribute("s" + juce::String(t) + "_" + juce::String(b), 0.0));
  recompute_latency_locked();
}
