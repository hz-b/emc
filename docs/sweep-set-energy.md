# Setting the energy during a sweep

This document describes how EMC in the mono application behaves when a client sends `SPE` or `SPO` while an onfly
sweep is active. 

Some clients send the next `SPE` as soon as `GPE` shows that the end energy has
been passed, without waiting for `GST` to report ready. At that moment the
sweep is still running out and braking. A new move started then would collide
with the sweep, so the mono holds it back until the sweep has ended.

## What the client sees

While a sweep is active (from `SI` until the sweep has fully ended), `SPE` and
`SPO` behave like this:

1. **The reply comes at once.** `t` means the target was accepted, not that the
   move has started.
2. **The sweep is not cut short.** A sweep that is in its run-in or sweeping
   phase (`sweepState` 32 or 64) runs to its end energy first. A sweep that was
   initialised with `SI` but not started with `SR` (`sweepState` 2 or 4) is
   cancelled.
3. **Then the mono moves to the new energy.** This starts once the axes have
   stopped after the sweep.
4. **`GST` stays busy (bit 1) the whole time**, from the accepted `SPE` until the
   move has started, and then while the move runs. Clients that wait for
   `GST` = 0 are safe.
5. **`sweepState` stays at 128 (done) or 255 (cancelled)** after the move.

| Situation | Result |
| --- | --- |
| `SPE` while no sweep is active | Normal move, as before. |
| `SPE` while sweeping | Sweep finishes, then the mono moves to the new energy. |
| `SPE` after `SI`, before `SR` | Prepared sweep is cancelled, then the move. |
| Two `SPE` while sweeping | Only the last energy is used. |
| `SPE`, then `STO` | No move. `STO` also cancels a waiting move. |
| `SPE`, then `SI` | No move; the new sweep is initialised instead. |
| `SPE`, then `SR` (no new `SI`) | `SR` fails: the prepared sweep has ended. Send `SI` again. |
| `SPE` outside the energy limits | `f` at once; the sweep continues. |

## Errors

Only an energy outside the limits, or an uninitialised mono, is reported in the
reply to `SPE`. If the move fails later, when it is executed, the reply was
already `t`: the failure is logged as "Deferred set energy failed" and can be
read with `GLE`.

## Stopping a sweep

`STO` during a sweep stops it and returns the mono to ready (`GST` = 0,
`sweepState` = 255). A new sweep can then be started with `SI`.
