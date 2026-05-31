// JUCE plugin host for m8c: 3 stereo send chains + master, fed by the M8
// track stems, returning a processed master to the M8.
// Released under the MIT licence, https://opensource.org/licenses/MIT
#include "juce_host.h"

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_events/juce_events.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <vector>

// Command-line token used when m8c relaunches itself as an out-of-process
// plugin-scanner worker (so a crashing plugin only kills the worker).
#define M8C_SCANNER_TOKEN "m8c-plugin-scan-worker"

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
  int proc_channels = 2; // jmax(total in, out): buffer size processBlock needs
  int quick_param[3] = {-1, -1, -1}; // assigned plugin parameter index, or -1
  int quick_cc[3] = {-1, -1, -1};    // bound MIDI CC number, or -1
  int quick_chan[3] = {-1, -1, -1};  // bound MIDI channel (1-16), or -1 = any
};

struct Bus {
  std::vector<Slot> slots;
  int capacity = kSendCap;
  int latency = 0;          // sum of non-bypassed slot latencies
  DelayLine align;          // applied to this bus' wet output (sends only)
  juce::AudioBuffer<float> buf; // stereo work buffer
  int midi_channel = 0;     // 1-16 = play instruments on this channel; 0 = off
  std::mutex midi_mutex;                       // guards midi_pending
  std::vector<juce::MidiMessage> midi_pending; // MIDI thread -> audio thread
};

// ---- Host state ----
std::unique_ptr<juce::ScopedJuceInitialiser_GUI> g_juce;
juce::AudioPluginFormatManager g_formats;
juce::KnownPluginList g_known;
std::mutex g_known_lock; // guards g_known (UI reads, scan driver writes)
std::atomic<bool> g_scanning{false};

} // namespace (helpers continue below)

static void add_formats(juce::AudioPluginFormatManager &fm); // defined in Scanning section

namespace {

std::mutex g_lock;        // guards buses + prepared state
Bus g_buses[JUCE_HOST_NUM_BUSES];
std::atomic<float> g_send[JUCE_HOST_NUM_TRACKS][JUCE_HOST_NUM_SENDS];

// MIDI-learn target for quick params (set on main thread, read on MIDI thread).
std::atomic<int> g_learn_bus{-1}, g_learn_slot{-1}, g_learn_quick{-1};

bool g_prepared = false;
double g_sr = 44100.0;
int g_maxBlock = 4096;
int g_totalLatency = 0;

DelayLine g_dryDelay;
juce::AudioBuffer<float> g_dry, g_masterIn;
// Scratch for plugins whose bus layout needs more than 2 channels (sidechain,
// etc.): processBlock requires a buffer of jmax(totalIn,totalOut) channels.
juce::AudioBuffer<float> g_plugin_scratch;

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
  // Force a plain stereo in/out layout (do NOT enableAllBuses: that can turn on
  // sidechain/aux input buses the host won't feed, which crashes some AUs).
  s.plugin->setPlayConfigDetails(2, 2, g_sr, g_maxBlock);
  s.plugin->prepareToPlay(g_sr, g_maxBlock);
  s.latency = s.plugin->getLatencySamples();
  // processBlock requires a buffer with jmax(totalIn,totalOut) channels — some
  // plugins keep >2 (sidechain/aux) even after setPlayConfigDetails(2,2).
  s.proc_channels = juce::jlimit(2, 64, juce::jmax(s.plugin->getTotalNumInputChannels(),
                                                    s.plugin->getTotalNumOutputChannels()));
  // No warm-up processBlock: rendering before the host feeds proper buffers
  // crashes some plugins (e.g. AUs reading a null input in renderGetInput).
}

void run_chain_locked(Bus &bus, juce::AudioBuffer<float> &buf, juce::MidiBuffer &midi) {
  const int n = buf.getNumSamples();
  for (auto &s : bus.slots) {
    if (s.bypassed || !s.plugin) continue;
    if (s.proc_channels <= 2) {
      s.plugin->processBlock(buf, midi);
    } else {
      // Widen to the plugin's required channel count; extra channels = silence.
      g_plugin_scratch.setSize(s.proc_channels, n, false, false, true);
      g_plugin_scratch.clear();
      g_plugin_scratch.copyFrom(0, 0, buf, 0, 0, n);
      g_plugin_scratch.copyFrom(1, 0, buf, 1, 0, n);
      s.plugin->processBlock(g_plugin_scratch, midi);
      buf.copyFrom(0, 0, g_plugin_scratch, 0, 0, n);
      buf.copyFrom(1, 0, g_plugin_scratch, 1, 0, n);
    }
  }
}

} // namespace

// ===========================================================================
// Lifecycle
// ===========================================================================
extern "C" void juce_host_init(void) {
  if (g_juce) return;
  g_juce = std::make_unique<juce::ScopedJuceInitialiser_GUI>();
  add_formats(g_formats);

  // Reload the cached plugin list so we don't have to rescan every launch.
  {
    juce::File kf = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
                        .getChildFile("m8c/known_plugins.xml");
    if (kf.existsAsFile()) {
      if (auto xml = juce::parseXML(kf)) {
        const std::lock_guard<std::mutex> lk(g_known_lock);
        g_known.recreateFromXml(*xml);
        std::fprintf(stderr, "[juce_host] loaded %d cached plugins\n", g_known.getNumTypes());
      }
    }
  }
  for (int b = 0; b < JUCE_HOST_NUM_BUSES; b++) {
    g_buses[b].capacity = (b == JUCE_HOST_BUS_MASTER) ? kMasterCap : kSendCap;
    // Default: send 1/2/3 play MIDI on channels 1/2/3; master off.
    g_buses[b].midi_channel = (b < JUCE_HOST_NUM_SENDS) ? (b + 1) : 0;
  }
  for (int t = 0; t < JUCE_HOST_NUM_TRACKS; t++)
    for (int b = 0; b < JUCE_HOST_NUM_SENDS; b++)
      g_send[t][b].store(0.0f);
  std::fprintf(stderr, "[juce_host] init — JUCE %d.%d.%d, %d formats\n", JUCE_MAJOR_VERSION,
               JUCE_MINOR_VERSION, JUCE_BUILDNUMBER, g_formats.getNumFormats());

  // Dev convenience: auto-start a scan at launch for testing without the UI.
  if (std::getenv("M8C_AUTOSCAN") != nullptr)
    juce_host_scan();
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
    {
      std::lock_guard<std::mutex> mlk(g_buses[b].midi_mutex);
      g_buses[b].midi_pending.clear();
      g_buses[b].midi_pending.reserve(256);
    }
    for (auto &s : g_buses[b].slots) prepare_plugin_locked(s);
  }
  g_dry.setSize(2, g_maxBlock);
  g_masterIn.setSize(2, g_maxBlock);
  g_plugin_scratch.setSize(64, g_maxBlock); // headroom so run_chain never reallocs
  g_dryDelay.prepare(2, kMaxDelay);
  recompute_latency_locked();
  g_prepared = true;
  std::fprintf(stderr, "[juce_host] prepared %.0f Hz, max block %d\n", g_sr, g_maxBlock);
}

// ===========================================================================
// Scanning — OUT OF PROCESS (like Element): m8c relaunches itself as an
// isolated scanner worker, so a plugin that crashes on instantiation only
// kills the worker, not the host.
// ===========================================================================
static void add_formats(juce::AudioPluginFormatManager &fm) {
#if JUCE_PLUGINHOST_VST3
  fm.addFormat(new juce::VST3PluginFormat());
#endif
#if JUCE_PLUGINHOST_AU && JUCE_MAC
  fm.addFormat(new juce::AudioUnitPluginFormat());
#endif
}

namespace {

static void wlog(const char *msg) {
  FILE *f = std::fopen("/tmp/m8c_worker.log", "a");
  if (f) { std::fprintf(f, "%s\n", msg); std::fclose(f); }
}

// Set false by the worker when the coordinator disconnects, to end its loop.
std::atomic<bool> g_worker_alive{true};

// ---- Worker side (child process): scans one plugin file at a time ----
class ScanWorker : public juce::ChildProcessWorker, private juce::AsyncUpdater {
public:
  ScanWorker() {
    // A plugin crash should just terminate the worker quietly.
    juce::SystemStats::setApplicationCrashHandler([](void *) { std::_Exit(1); });
    add_formats(formats);
    wlog("[worker] constructed");
  }

  void handleConnectionMade() override { wlog("[worker] connection made"); }

  void handleMessageFromCoordinator(const juce::MemoryBlock &mb) override {
    if (mb.isEmpty())
      return;
    const std::lock_guard<std::mutex> lock(mutex);
    pending.add(mb);
    triggerAsyncUpdate(); // do the (un-blockable) scan on the message thread
  }

  void handleAsyncUpdate() override {
    for (;;) {
      juce::MemoryBlock mb;
      {
        const std::lock_guard<std::mutex> lock(mutex);
        if (pending.isEmpty())
          return;
        mb = pending.getReference(0);
        pending.remove(0);
      }
      sendResults(doScan(mb));
    }
  }

  void handleConnectionLost() override {
    wlog("[worker] connection lost, ending");
    g_worker_alive.store(false);
  }

private:
  juce::OwnedArray<juce::PluginDescription> doScan(const juce::MemoryBlock &block) {
    juce::MemoryInputStream in(block, false);
    const juce::String formatName = in.readString();
    const juce::String identifier = in.readString();
    juce::OwnedArray<juce::PluginDescription> results;
    {
      char m[1100];
      std::snprintf(m, sizeof(m), "[worker] doScan fmt=%s id=%s", formatName.toRawUTF8(),
                    identifier.toRawUTF8());
      wlog(m);
    }
    for (int i = 0; i < formats.getNumFormats(); i++) {
      auto *f = formats.getFormat(i);
      if (f->getName() == formatName) {
        f->findAllTypesForFile(results, identifier);
        break;
      }
    }
    {
      char m[128];
      std::snprintf(m, sizeof(m), "[worker] doScan -> %d descriptions", results.size());
      wlog(m);
    }
    return results;
  }

  void sendResults(const juce::OwnedArray<juce::PluginDescription> &results) {
    juce::XmlElement xml("LIST");
    for (auto *d : results)
      xml.addChildElement(d->createXml().release());
    const auto s = xml.toString();
    sendMessageToCoordinator({s.toRawUTF8(), s.getNumBytesAsUTF8()});
  }

  juce::AudioPluginFormatManager formats;
  std::mutex mutex;
  juce::Array<juce::MemoryBlock> pending;
};

// ---- Coordinator side (host process): drives the worker ----
class ScanCoordinator : public juce::ChildProcessCoordinator {
public:
  enum class State { timeout, gotResult, connectionLost };
  struct Response { State state; std::unique_ptr<juce::XmlElement> xml; };

  bool launch() {
    auto exe = juce::File::getSpecialLocation(juce::File::currentExecutableFile);
    return launchWorkerProcess(exe, M8C_SCANNER_TOKEN, 0, 0);
  }

  Response getResponse() {
    std::unique_lock<std::mutex> lock(mutex);
    if (!condvar.wait_for(lock, std::chrono::milliseconds(50),
                          [&] { return gotResult || connectionLost; }))
      return {State::timeout, nullptr};
    const auto st = connectionLost ? State::connectionLost : State::gotResult;
    connectionLost = gotResult = false;
    return {st, std::move(result)};
  }

  void handleMessageFromWorker(const juce::MemoryBlock &mb) override {
    const std::lock_guard<std::mutex> lock(mutex);
    result = juce::parseXML(mb.toString());
    gotResult = true;
    condvar.notify_one();
  }
  void handleConnectionLost() override {
    const std::lock_guard<std::mutex> lock(mutex);
    connectionLost = true;
    condvar.notify_one();
  }

private:
  std::mutex mutex;
  std::condition_variable condvar;
  std::unique_ptr<juce::XmlElement> result;
  bool gotResult = false, connectionLost = false;
};

// ---- Driver thread: enumerate files, feed worker, collect descriptions ----
class ScanDriver : public juce::Thread {
public:
  ScanDriver() : juce::Thread("m8c_plugin_scan") {}

  void run() override {
    std::unique_ptr<ScanCoordinator> coord = std::make_unique<ScanCoordinator>();
    if (!coord->launch()) {
      std::fprintf(stderr, "[juce_host] could not launch scanner worker\n");
      g_scanning.store(false);
      return;
    }

    for (int fi = 0; fi < g_formats.getNumFormats() && !threadShouldExit(); fi++) {
      auto *fmt = g_formats.getFormat(fi);
      const juce::String formatName = fmt->getName();
      auto ids = fmt->searchPathsForPlugins(fmt->getDefaultLocationsToSearch(), true, false);
      std::fprintf(stderr, "[juce_host] format '%s': %d files to scan\n", formatName.toRawUTF8(),
                   ids.size());
      for (const auto &id : ids) {
        if (threadShouldExit())
          break;

        juce::MemoryBlock req;
        {
          juce::MemoryOutputStream os(req, false);
          os.writeString(formatName);
          os.writeString(id);
        }
        const bool sent = coord->sendMessageToWorker(req);
        if (!sent)
          std::fprintf(stderr, "[juce_host] sendMessageToWorker FAILED for %s\n", id.toRawUTF8());

        bool handled = false;
        int waited = 0;
        while (!handled && !threadShouldExit()) {
          auto r = coord->getResponse();
          if (r.state == ScanCoordinator::State::gotResult) {
            int added = 0;
            if (r.xml) {
              const std::lock_guard<std::mutex> lock(g_known_lock);
              for (auto *child : r.xml->getChildIterator()) {
                juce::PluginDescription d;
                if (d.loadFromXml(*child)) {
                  g_known.addType(d);
                  added++;
                }
              }
            }
            std::fprintf(stderr, "[juce_host] scanned %s (+%d) [%d known]\n", id.toRawUTF8(),
                         added, (int)g_known.getNumTypes());
            handled = true;
          } else if (r.state == ScanCoordinator::State::connectionLost ||
                     (++waited > 120 /* ~6s */)) {
            // Worker crashed or hung on this plugin: relaunch and skip it.
            std::fprintf(stderr, "[juce_host] scanner lost on %s (skipped)\n", id.toRawUTF8());
            coord = std::make_unique<ScanCoordinator>();
            coord->launch();
            handled = true; // skip this id
          }
        }
      }
    }
    // Persist the discovered plugin list for next launch.
    {
      const std::lock_guard<std::mutex> lock(g_known_lock);
      juce::File kf = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
                          .getChildFile("m8c/known_plugins.xml");
      kf.create();
      if (auto xml = g_known.createXml())
        xml->writeTo(kf);
    }
    std::fprintf(stderr, "[juce_host] scan done: %d plugins known\n",
                 (int)g_known.getNumTypes());
    g_scanning.store(false);
  }
};

std::unique_ptr<ScanDriver> g_driver;

} // namespace

extern "C" void juce_host_scan(void) {
  if (g_scanning.load())
    return;
  g_scanning.store(true);
  std::fprintf(stderr, "[juce_host] scanning plugins (out of process)...\n");
  g_driver = std::make_unique<ScanDriver>();
  g_driver->startThread();
}

extern "C" bool juce_host_run_scanner_if_worker(int argc, char **argv) {
  juce::String cmd;
  for (int i = 1; i < argc; i++) {
    cmd << " " << juce::String(argv[i]);
  }
  if (!cmd.contains(M8C_SCANNER_TOKEN))
    return false; // normal app launch

  // We are the isolated scanner worker.
  wlog("[worker] entered worker mode");
  new juce::ScopedJuceInitialiser_GUI(); // process exits when done; leak is fine
  auto *worker = new ScanWorker();
  if (worker->initialiseFromCommandLine(cmd, M8C_SCANNER_TOKEN)) {
    wlog("[worker] initialiseFromCommandLine OK, pumping");
    // runDispatchLoop() returns immediately in this non-JUCEApplication context,
    // so pump in slices and stay alive until the coordinator disconnects.
    while (g_worker_alive.load())
      juce::MessageManager::getInstance()->runDispatchLoopUntil(100);
    wlog("[worker] exiting");
    std::exit(0);
  }
  wlog("[worker] initialiseFromCommandLine returned FALSE (not a worker)");
  delete worker;
  return false;
}

extern "C" bool juce_host_is_scanning(void) { return g_scanning.load(); }

extern "C" int juce_host_known_count(void) {
  const std::lock_guard<std::mutex> lk(g_known_lock);
  return g_known.getNumTypes();
}

extern "C" void juce_host_known_label(int index, char *out, int out_size) {
  out[0] = '\0';
  const std::lock_guard<std::mutex> lk(g_known_lock);
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
  juce::PluginDescription desc;
  {
    const std::lock_guard<std::mutex> lk(g_known_lock);
    auto types = g_known.getTypes();
    if (known_index < 0 || known_index >= types.size()) return -1;
    desc = types.getReference(known_index);
  }

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
// MIDI routing (instruments)
// ===========================================================================
extern "C" int juce_host_bus_midi_channel(int bus) {
  if (bus < 0 || bus >= JUCE_HOST_NUM_BUSES) return 0;
  return g_buses[bus].midi_channel;
}

extern "C" void juce_host_bus_set_midi_channel(int bus, int channel) {
  if (bus < 0 || bus >= JUCE_HOST_NUM_BUSES) return;
  g_buses[bus].midi_channel = juce::jlimit(0, 16, channel);
}

// Resolve a quick param's target AudioProcessorParameter (caller holds g_lock).
static juce::AudioProcessorParameter *quick_target_locked(Slot &s, int quick) {
  if (quick < 0 || quick >= JUCE_HOST_NUM_QUICK || !s.plugin)
    return nullptr;
  const int p = s.quick_param[quick];
  const auto &params = s.plugin->getParameters();
  if (p < 0 || p >= params.size())
    return nullptr;
  return params[p];
}

extern "C" void juce_host_push_midi(const unsigned char *data, int len) {
  if (!g_prepared || len <= 0)
    return;
  juce::MidiMessage msg((const void *)data, len, 0.0);
  const int ch = msg.getChannel(); // 1-16, or 0 for non-channel messages

  // Route notes/CC to instrument lanes on the matching channel (1-16; 0=off).
  for (int b = 0; b < JUCE_HOST_NUM_SENDS; b++) {
    if (g_buses[b].midi_channel != 0 && g_buses[b].midi_channel == ch) {
      std::lock_guard<std::mutex> mlk(g_buses[b].midi_mutex);
      if (g_buses[b].midi_pending.size() < 1024)
        g_buses[b].midi_pending.push_back(msg);
    }
  }

  // Quick-param control: bind on MIDI-learn, else drive bound params.
  if (msg.isController()) {
    const int cc = msg.getControllerNumber();
    const float v = msg.getControllerValue() / 127.0f;
    std::lock_guard<std::mutex> lk(g_lock);
    const int lb = g_learn_bus.load(), ls = g_learn_slot.load(), lq = g_learn_quick.load();
    if (lq >= 0 && lb >= 0 && lb < JUCE_HOST_NUM_BUSES && ls >= 0 &&
        ls < (int)g_buses[lb].slots.size()) {
      g_buses[lb].slots[ls].quick_cc[lq] = cc;
      g_buses[lb].slots[ls].quick_chan[lq] = ch;
      g_learn_quick.store(-1);
      g_learn_bus.store(-1);
      g_learn_slot.store(-1);
    } else {
      for (int b = 0; b < JUCE_HOST_NUM_BUSES; b++)
        for (auto &s : g_buses[b].slots)
          for (int q = 0; q < JUCE_HOST_NUM_QUICK; q++)
            if (s.quick_cc[q] == cc && (s.quick_chan[q] < 0 || s.quick_chan[q] == ch))
              if (auto *p = quick_target_locked(s, q))
                p->setValueNotifyingHost(v);
    }
  }
}

extern "C" int juce_host_slot_param_count(int bus, int slot) {
  if (bus < 0 || bus >= JUCE_HOST_NUM_BUSES) return 0;
  std::lock_guard<std::mutex> lk(g_lock);
  auto &v = g_buses[bus].slots;
  if (slot < 0 || slot >= (int)v.size() || !v[slot].plugin) return 0;
  return v[slot].plugin->getParameters().size();
}

extern "C" void juce_host_slot_param_name(int bus, int slot, int param, char *out, int out_size) {
  out[0] = '\0';
  if (bus < 0 || bus >= JUCE_HOST_NUM_BUSES) return;
  std::lock_guard<std::mutex> lk(g_lock);
  auto &v = g_buses[bus].slots;
  if (slot < 0 || slot >= (int)v.size() || !v[slot].plugin) return;
  const auto &params = v[slot].plugin->getParameters();
  if (param < 0 || param >= params.size()) return;
  params[param]->getName(out_size - 1).copyToUTF8(out, (size_t)out_size);
}

extern "C" int juce_host_slot_quick_param(int bus, int slot, int quick) {
  if (bus < 0 || bus >= JUCE_HOST_NUM_BUSES || quick < 0 || quick >= JUCE_HOST_NUM_QUICK) return -1;
  std::lock_guard<std::mutex> lk(g_lock);
  auto &v = g_buses[bus].slots;
  if (slot < 0 || slot >= (int)v.size()) return -1;
  return v[slot].quick_param[quick];
}

extern "C" void juce_host_slot_quick_assign(int bus, int slot, int quick, int param) {
  if (bus < 0 || bus >= JUCE_HOST_NUM_BUSES || quick < 0 || quick >= JUCE_HOST_NUM_QUICK) return;
  std::lock_guard<std::mutex> lk(g_lock);
  auto &v = g_buses[bus].slots;
  if (slot < 0 || slot >= (int)v.size()) return;
  v[slot].quick_param[quick] = param;
}

extern "C" void juce_host_slot_quick_label(int bus, int slot, int quick, char *out, int out_size) {
  std::snprintf(out, (size_t)out_size, "--");
  if (bus < 0 || bus >= JUCE_HOST_NUM_BUSES) return;
  std::lock_guard<std::mutex> lk(g_lock);
  auto &v = g_buses[bus].slots;
  if (slot < 0 || slot >= (int)v.size()) return;
  if (auto *p = quick_target_locked(v[slot], quick))
    p->getName(out_size - 1).copyToUTF8(out, (size_t)out_size);
}

extern "C" float juce_host_slot_quick_value(int bus, int slot, int quick) {
  if (bus < 0 || bus >= JUCE_HOST_NUM_BUSES) return 0;
  std::lock_guard<std::mutex> lk(g_lock);
  auto &v = g_buses[bus].slots;
  if (slot < 0 || slot >= (int)v.size()) return 0;
  if (auto *p = quick_target_locked(v[slot], quick)) return p->getValue();
  return 0;
}

extern "C" void juce_host_slot_quick_nudge(int bus, int slot, int quick, float delta) {
  if (bus < 0 || bus >= JUCE_HOST_NUM_BUSES) return;
  std::lock_guard<std::mutex> lk(g_lock);
  auto &v = g_buses[bus].slots;
  if (slot < 0 || slot >= (int)v.size()) return;
  if (auto *p = quick_target_locked(v[slot], quick))
    p->setValueNotifyingHost(juce::jlimit(0.0f, 1.0f, p->getValue() + delta));
}

extern "C" int juce_host_slot_quick_cc(int bus, int slot, int quick) {
  if (bus < 0 || bus >= JUCE_HOST_NUM_BUSES || quick < 0 || quick >= JUCE_HOST_NUM_QUICK) return -1;
  std::lock_guard<std::mutex> lk(g_lock);
  auto &v = g_buses[bus].slots;
  if (slot < 0 || slot >= (int)v.size()) return -1;
  return v[slot].quick_cc[quick];
}

extern "C" void juce_host_begin_learn(int bus, int slot, int quick) {
  g_learn_bus.store(bus);
  g_learn_slot.store(slot);
  g_learn_quick.store(quick);
}

extern "C" bool juce_host_is_learning(void) { return g_learn_quick.load() >= 0; }

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
    juce::MidiBuffer midi;
    { // drain queued MIDI for this lane (notes for instruments), at block start
      std::lock_guard<std::mutex> mlk(bus.midi_mutex);
      for (auto &m : bus.midi_pending)
        midi.addEvent(m, 0);
      bus.midi_pending.clear();
    }
    run_chain_locked(bus, bus.buf, midi);
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

  // Master chain (no MIDI input).
  juce::MidiBuffer masterMidi;
  run_chain_locked(g_buses[JUCE_HOST_BUS_MASTER], g_masterIn, masterMidi);

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
    busXml->setAttribute("midi_channel", g_buses[b].midi_channel);
    for (auto &s : g_buses[b].slots) {
      auto *slotXml = busXml->createNewChildElement("SLOT");
      slotXml->setAttribute("bypassed", s.bypassed);
      for (int q = 0; q < JUCE_HOST_NUM_QUICK; q++) {
        slotXml->setAttribute("q" + juce::String(q) + "p", s.quick_param[q]);
        slotXml->setAttribute("q" + juce::String(q) + "cc", s.quick_cc[q]);
        slotXml->setAttribute("q" + juce::String(q) + "ch", s.quick_chan[q]);
      }
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
    g_buses[b].midi_channel = juce::jlimit(0, 16, busXml->getIntAttribute("midi_channel",
                                                                          g_buses[b].midi_channel));
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
      for (int q = 0; q < JUCE_HOST_NUM_QUICK; q++) {
        s.quick_param[q] = slotXml->getIntAttribute("q" + juce::String(q) + "p", -1);
        s.quick_cc[q] = slotXml->getIntAttribute("q" + juce::String(q) + "cc", -1);
        s.quick_chan[q] = slotXml->getIntAttribute("q" + juce::String(q) + "ch", -1);
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
