# Per-output insert chains (console model)

Turn m8c into a full console: in addition to the 3 send chains + master chain,
**every M8 audio output gets its own dedicated insert FX chain**, and the master
is rebuilt from the (insert-processed) outputs.

## Bus layout (15 buses)
| id | role | input |
|----|------|-------|
| 0-2 | send 1-3 | summed post-insert tracks × CC send gains |
| 3 | master | sum of inserts + sends |
| 4-11 | track inserts 1-8 | M8 ch (2+2t, 3+2t) |
| 12-14 | FX-return inserts (modfx/delay/reverb) | M8 ch (18+2f, 19+2f) |

Constants: `JUCE_HOST_INSERT_BASE = 4`, `JUCE_HOST_NUM_FXRET = 3`,
`JUCE_HOST_NUM_INSERTS = 11`, `JUCE_HOST_NUM_BUSES = 15`.

## Signal flow (console)
Chosen model: **rebuild the mix from the individual M8 outputs** (the M8's own
ch0-1 master mix is no longer the monitor source). Per block:
1. **Inserts**: each of the 8 track pairs + 3 FX-return pairs runs through its
   own chain, then is aligned to `maxInsert` (PDC stage 1).
2. **Sends**: each send = Σ over tracks of (post-insert, aligned track × send
   gain), through its chain, aligned to `maxSend` (PDC stage 2).
3. **Master** = (Σ all 11 insert outputs, delayed by `maxSend`) + (Σ sends),
   through the master chain → output.

PDC: total latency = `maxInsert + maxSend + masterLatency`.

## Why console (vs parallel)
User choice: full per-channel control. Trade-off: with empty inserts the master
= sum of the M8's individual outputs, which approximates (not bit-identical to)
the M8's own ch0-1 master mix — levels/master processing may differ slightly.

## Phases
1. **Engine** — 15 buses, console routing, 2-stage PDC, prepare/latency/init.
   Sends + master keep working (regression-safe); empty inserts = passthrough.
2. **UI** — the overlay must show 15 chains. Plan: a horizontally-scrollable
   strip (4 columns visible) grouped T1-8 / modfx-delay-reverb / S1-3 / MASTER,
   with labelled headers; arrows scroll the window.
3. **Recorder (later)** — optionally capture post-insert per-track stems.

## Notes
- Inserts host audio FX only (MIDI off); instruments stay on send lanes.
- Persistence iterates all buses by id → inserts auto save/load; old songs
  (4 buses) still load (extra buses just empty).
