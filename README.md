# zmk-nix — Halcyon Ferris firmware

ZMK firmware for my [splitkb Halcyon Ferris](https://splitkb.com/products/halcyon-ferris),
built reproducibly with Nix via [zmk-nix](https://github.com/lilyinstarlight/zmk-nix).

The keymap is a 1:1 port of my ZSA Voyager Oryx layout
[*"Nima's 34 keys"*](https://configure.zsa.io/voyager/layouts/eVJKy/latest)
(`eVJKy`, ported from revision `pjMjbv`). Only the 34 bound Voyager keys
are used, so the layout maps straight onto the Ferris.

## Hardware

| Part   | Board                   | Shields                                                                 |
| ------ | ----------------------- | ----------------------------------------------------------------------- |
| dongle | `halcyon_dongle//zmk`   | `halcyon_ferris_dongle mod_cirque_central`                              |
| left   | `halcyon_wireless//zmk` | `halcyon_ferris_left mod_battery_coincell mod_display_epaper_forest`    |
| right  | `halcyon_wireless//zmk` | `halcyon_ferris_right mod_battery_coincell mod_cirque_hw_right`         |

The dongle is the central (USB/BT host); both halves are BLE peripherals
(`config/halcyon_ferris_left.conf` sets `CONFIG_ZMK_SPLIT_ROLE_CENTRAL=n`
for the left half). The right half's Cirque touchpad is forwarded to the dongle
(`mod_cirque_central`). The e-paper artwork can be switched to
`mod_display_epaper_mountain` / `mod_display_epaper_cityscape` in `flake.nix`
(or a custom image, see the module's README).

## Layers

| # | Name   | Hold key            |
| - | ------ | ------------------- |
| 0 | Base   | —                   |
| 1 | NumSym | right outer thumb (Backspace) |
| 2 | Nav    | left outer thumb (Space)      |
| 3 | Func   | left inner thumb (Enter)      |
| 4 | Mouse  | right inner thumb (Enter)     |

The touchpad moves the cursor (2× speed) and tap = left click on every
layer; **tap, then tap again and hold** drags. While the Mouse layer is
held, dragging on the touchpad scrolls and tap = right click instead
(`config/halcyon_ferris_dongle.overlay`: `&drag_lock` takes the button to
emit as its parameter, left on the base chain, right on the Mouse layer's).
The chip reports a short stroke (under ~250 ms) as a tap and the next touch
as a drag, so flick-scrolling right-clicked (and pasted), short strokes
clicked and "lift, re-centre, continue" dragged; `&drag_lock` swallows a
button that arrives within 320 ms after such a short stroke. A fast
double-tap-and-hold drags; right after a quick flick, pause a moment before
tapping or dragging.

`config/halcyon_ferris_right.overlay` sets the touch sensitivity (`"2x"`,
the shield default; `"1x"` is the most sensitive but makes the single-touch
pad react to a hovering second finger or the palm, which shows up as jumps).

The Cirque driver sends every packet as three BLE notifications (X, Y,
button+sync) at up to 100 packets/s. The stock ZMK buffers could not carry
that: the dongle acknowledged one or two packets per connection event and
queued them in a 5-entry queue that drops silently, so packets got torn
apart (cursor jumps, frames moving the wrong way). `config/halcyon_ferris_dongle.conf`
raises the dongle's radio receive buffers and queues, and
`config/halcyon_ferris_right.conf` raises the right half's BLE transmit
buffers and send queue. (The ~250 ms gaps seen during continuous movement
are the pad losing the finger near its rim, not the link: lowering the
split links' slave latency to 0 changed nothing and would cost both halves
most of their idle battery, so ZMK's default is kept.)

`modules/drag-lock/` is a small in-repo ZMK module providing `&drag_lock`,
the first input processor in every chain of `&trackpad_listener`. It does
two things:

- Forwards only real button *transitions*. Zephyr's `cirque,pinnacle` driver
  repeats the button bit in every packet while ZMK reference-counts mouse
  button presses, so without it a drag stayed stuck after lifting the finger
  until as many release packets had arrived (a tap could not clear it).
- Software drag lock: lifting the finger mid-drag keeps the button held for
  `timeout-ms` (300 ms, libinput's value); touch down and move again within
  that time and the drag continues, otherwise it releases. Plain taps are
  forwarded immediately, so clicks / double-clicks are not delayed; a press
  only counts as a drag after `drag-threshold` (8 counts) of motion, so a
  slight finger roll during a tap does not arm the lock. The chip only
  reports the lift for its own tap-drag, so a *continued* drag ends after
  `soft-drag-idle-ms` (1 s) without motion, or immediately when you tap
  (a motion-less press while motion is still flowing is the chip's late tap
  verdict on a short reposition and is ignored; a tap reported up to
  `tap-after-drag-ms` (350) after the timeout already released the drag is
  swallowed rather than clicked). (The 500 ms window plus "any touch
  continues" turned "click, move, lift, move again" into a stuck drag: hand
  cursor, everything being dragged until you stopped for a second.)
- Misread strokes: a chip button that arrives within `tap-after-stroke-ms`
  (320) after a stroke of at most `short-stroke-ms` (250) that moved at
  least `tap-max-stroke` (24) counts is swallowed: no click, no drag, the
  motion passes. Presses within `tap-drag-window-ms` (500) after a
  forwarded tap are always accepted.
- The button code is the processor's parameter and is latched at the press,
  so a layer change mid-gesture cannot leave ZMK's per-button press count
  unbalanced (a stuck button).

Home-row mods on `A S D F G` / `H J K L ;` (Shift · — · Alt · GUI · Ctrl and
mirrored), kept on the NumSym and Func layers like in Oryx.

Combos (Base layer, 50 ms): `E+R` → Esc, `O+P` → Del, `C+V` → Tab, `J+K` → Ctrl+B (tmux prefix).
`E+R` and `O+P` sit on common bigrams, so they only fire after a 100 ms typing pause (`require-prior-idle-ms`).

Additions not in the Oryx layout (Func layer): `Q` → ZMK Studio unlock, `E`/`R` → previous/next underglow effect, `T` → underglow toggle, `Y`/`U` → underglow colour (hue) down/up.

Tap/hold behaviour (see the top of the keymap):

- Home-row mods use ZMK's recommended ["timeless" configuration](https://zmk.dev/docs/keymaps/behaviors/hold-tap#home-row-mods)
  (`&hml` / `&hmr`): `balanced` flavour, 280 ms tapping term, 150 ms
  `require-prior-idle-ms`, positional — a left-hand mod only activates together
  with a right-hand key or a thumb (and vice versa), `hold-trigger-on-release`
  so mods on one hand can be stacked. Shortcuts like Ctrl+C (hold `H`, tap `C`)
  work instantly; same-hand rolls never produce modifiers.
- Layer-taps on the thumbs are `balanced` (200 ms), so e.g. hold `Space` + tap
  `J` gives Down right away. Tap-then-hold within 175 ms repeats Space/Backspace.
- The two `&mt` keys on the Nav layer (GUI/Ctrl on `F`/`G`) are `hold-preferred`.

## Getting logs

Both the dongle and the right half currently build with `CONFIG_ZMK_USB_LOGGING=y`
(`config/halcyon_ferris_dongle.conf`, `config/halcyon_ferris_right.conf`), so
each exposes a USB serial port with the log (ZMK itself at ERROR level on the
dongle, INFO on the right half; the drag-lock module at INFO). The dongle
prints a `drag_lock stats:` line every 5 s of touchpad traffic (packets, torn
packets, stalls, big deltas, direction flips) and one line per press saying
how it was classified. To capture:

```sh
scripts/zmk-log.sh            # writes ~/zmk-logs/<serial>-<if>.log, Ctrl-C to stop
```

Plug the right half in by USB as well if its log is wanted. Note the clock
time when something goes wrong; the script prefixes every line with it.
Comment the logging lines out again when done (one less USB port, less work
for the dongle).

## Usage

```sh
git add <new files>    # the flake only sees git-tracked files: `git add` any new .conf / .overlay first
nix build              # -> result/zmk_{dongle,left,right}.uf2
nix run .#flash        # double-tap reset on each part when prompted
nix run .#flash dongle # flash only one part (dongle | left | right)
nix run .#update     # bump west deps (ZMK fork, halcyon module) + zephyrDepsHash
nix develop          # shell with west/zephyr toolchain
```

Files:

- `flake.nix` — board/shield selection, in-repo modules, `zephyrDepsHash`
- `config/west.yml` — pinned splitkb ZMK fork + `zmk-halcyon-module`
- `config/halcyon_ferris.keymap` — the keymap
- `config/halcyon_ferris.conf` — Kconfig options (all parts)
- `config/halcyon_ferris_left.conf` — left-half-only options (peripheral role)
- `config/halcyon_ferris_right.conf` — right-half-only options (BLE transmit buffers / send queue for the touchpad)
- `config/halcyon_ferris_dongle.conf` — dongle-only options (radio receive buffers / queues for the touchpad, optional USB logging)
- `config/halcyon_ferris_dongle.overlay` — dongle-only devicetree (touchpad speed, scroll / right-click on the Mouse layer, `&drag_lock`)
- `config/halcyon_ferris_right.overlay` — right-half-only devicetree (touchpad sensitivity)
- `modules/drag-lock/` — in-repo ZMK module with the `&drag_lock` input processor (included via `extraCmakeFlags` in `flake.nix`)
