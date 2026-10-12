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
layer; double-tap-and-hold drags (handled by the Cirque chip). While the
Mouse layer is held, dragging on the touchpad scrolls and tap = right click
instead (`config/halcyon_ferris_dongle.overlay`, using ZMK's built-in input
processors).

`config/halcyon_ferris_right.overlay` raises the touch sensitivity to the
maximum (`sensitivity = "1x"`, shield default `"2x"`) so light taps register
(drop back to `"2x"` if the cursor jitters).

`modules/drag-lock/` is a small in-repo ZMK module providing `&drag_lock`,
the first input processor in every chain of `&trackpad_listener`. It does
two things:

- Forwards only real button *transitions*. Zephyr's `cirque,pinnacle` driver
  repeats the button bit in every packet while ZMK reference-counts mouse
  button presses, so without it a drag stayed stuck after lifting the finger
  until as many release packets had arrived (a tap could not clear it).
- Software drag lock: lifting the finger mid-drag keeps the button held for
  `timeout-ms` (500 ms); touch down and move again within that time and the
  drag continues, otherwise it releases. Plain taps are forwarded immediately,
  so clicks / double-clicks are not delayed. The chip only reports the lift
  for its own tap-drag, so a *continued* drag ends after `soft-drag-idle-ms`
  (1 s) without motion - keep the finger moving, or lift and wait.

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

## Usage

```sh
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
- `config/halcyon_ferris_dongle.overlay` — dongle-only devicetree (touchpad speed, scroll / right-click on the Mouse layer, `&drag_lock`)
- `config/halcyon_ferris_right.overlay` — right-half-only devicetree (touchpad sensitivity)
- `modules/drag-lock/` — in-repo ZMK module with the `&drag_lock` input processor (included via `extraCmakeFlags` in `flake.nix`)
