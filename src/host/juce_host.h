// C facade over the JUCE plugin host. The C side of m8c only ever sees this
// header (opaque handles + POD); JUCE headers stay inside the C++/Obj-C++ unit.
// P0: lifecycle + message-loop seam only (no audio, no plugins yet).
// Released under the MIT licence, https://opensource.org/licenses/MIT
#ifndef JUCE_HOST_H_
#define JUCE_HOST_H_

#ifdef __cplusplus
extern "C" {
#endif

// Initialize the JUCE GUI/message subsystem. Must be called once on the main
// thread (from SDL_AppInit), after SDL has created its NSApplication.
void juce_host_init(void);

// Non-blocking message pump, called once per SDL_AppIterate. On macOS JUCE
// shares the Cocoa run loop that SDL already pumps, so this is currently a
// no-op kept as a seam for other platforms / future use.
void juce_host_pump(void);

// Tear down the JUCE subsystem (from SDL_AppQuit), on the main thread.
void juce_host_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif
