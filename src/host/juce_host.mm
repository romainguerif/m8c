// JUCE plugin host — P0: lifecycle + message-loop seam.
// Released under the MIT licence, https://opensource.org/licenses/MIT
#include "juce_host.h"

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_events/juce_events.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <cstdio>

namespace {

// Owns JUCE's GUI/message subsystem for the process lifetime.
std::unique_ptr<juce::ScopedJuceInitialiser_GUI> g_juce;

// One-shot probe: if this fires, JUCE's message loop is alive under SDL's
// Cocoa run loop — the make-or-break P0 validation.
struct ProbeTimer : public juce::Timer {
  void timerCallback() override {
    std::fprintf(stderr,
                 "[juce_host] message loop alive under SDL (timer fired) — P0 seam OK\n");
    stopTimer();
  }
};
std::unique_ptr<ProbeTimer> g_probe;

} // namespace

extern "C" void juce_host_init(void) {
  if (g_juce != nullptr)
    return;
  g_juce = std::make_unique<juce::ScopedJuceInitialiser_GUI>();

  std::fprintf(stderr, "[juce_host] init — JUCE %d.%d.%d\n", JUCE_MAJOR_VERSION,
               JUCE_MINOR_VERSION, JUCE_BUILDNUMBER);

  // Prove that JUCE async messages / timers dispatch while SDL owns the loop.
  g_probe = std::make_unique<ProbeTimer>();
  g_probe->startTimer(500);
  juce::MessageManager::callAsync(
      [] { std::fprintf(stderr, "[juce_host] callAsync dispatched — async messages OK\n"); });
}

extern "C" void juce_host_pump(void) {
  // macOS: JUCE rides the shared Cocoa run loop that SDL already pumps, so no
  // explicit pump is needed here. Kept as a seam for other platforms.
}

extern "C" void juce_host_shutdown(void) {
  g_probe.reset();
  g_juce.reset();
  std::fprintf(stderr, "[juce_host] shutdown\n");
}
