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

Home-row mods on `A S D F G` / `H J K L ;` (Shift · — · Alt · GUI · Ctrl and
mirrored), kept on the NumSym and Func layers like in Oryx.

Combos (Base layer, 50 ms): `E+R` → Esc, `O+P` → Del, `C+V` → Tab, `J+K` → Ctrl+B (tmux prefix).

Additions not in the Oryx layout (Func layer): `Q` → ZMK Studio unlock, `E`/`R` → previous/next underglow effect, `T` → underglow toggle, `Y`/`U` → underglow colour (hue) down/up.

Tap/hold behaviour mimics QMK defaults (200 ms tapping term, tap-preferred,
quick-tap repeat) — see the `&mt` / `&lt` overrides at the top of the keymap.

## Usage

```sh
nix build              # -> result/zmk_{dongle,left,right}.uf2
nix run .#flash        # double-tap reset on each part when prompted
nix run .#flash dongle # flash only one part (dongle | left | right)
nix run .#update     # bump west deps (ZMK fork, halcyon module) + zephyrDepsHash
nix develop          # shell with west/zephyr toolchain
```

Files:

- `flake.nix` — board/shield selection, `zephyrDepsHash`
- `config/west.yml` — pinned splitkb ZMK fork + `zmk-halcyon-module`
- `config/halcyon_ferris.keymap` — the keymap
- `config/halcyon_ferris.conf` — Kconfig options (all parts)
- `config/halcyon_ferris_left.conf` — left-half-only options (peripheral role)
