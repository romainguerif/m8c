// MIDI Control Change input for the recorder trigger.
// Opens the M8's USB MIDI port (free, since the display uses the serial
// backend) and forwards CC messages to recorder_handle_cc().
// Released under the MIT licence, https://opensource.org/licenses/MIT
#ifndef MIDI_CC_H_
#define MIDI_CC_H_

#include <stdbool.h>

// Open the M8 MIDI source and start routing CC to the recorder. Returns false
// if no port was found or on unsupported platforms (no-op stub elsewhere).
bool midi_cc_open(void);

// Stop and release the MIDI input.
void midi_cc_close(void);

#endif
