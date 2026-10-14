# zmk-nix — Halcyon Ferris firmware (`vanilla` branch)

This branch builds what splitkb releases for the wireless Halcyon Ferris
(dongle, coin-cell halves, e-paper on the left, Cirque touchpad on the right)
with only the keymap replaced by mine. No touchpad tuning, no in-repo
modules, no logging: the touchpad behaves exactly as in the stock images.
It exists to compare against `main`, which carries the touchpad work.

| Part   | Board                   | Shields (as in splitkb's `build.yaml`)                                 |
| ------ | ----------------------- | ----------------------------------------------------------------------- |
| dongle | `halcyon_dongle//zmk`   | `halcyon_ferris_dongle mod_cirque_central` (+ ZMK Studio over USB)      |
| left   | `halcyon_wireless//zmk` | `halcyon_ferris_left mod_battery_coincell mod_display_epaper_forest`    |
| right  | `halcyon_wireless//zmk` | `halcyon_ferris_right mod_battery_coincell mod_cirque_hw_right`         |

Differences from the stock images:

- the keymap (`config/halcyon_ferris.keymap`, see the top of the file for the
  layout and the tap/hold settings);
- `CONFIG_ZMK_POINTING` / `CONFIG_ZMK_POINTING_SMOOTH_SCROLLING` enabled on
  all parts (`config/halcyon_ferris.conf`) for the keymap's mouse keys; the
  Cirque shields already imply both on the dongle and the right half;
- `config/halcyon_ferris_left.conf` makes the left half a peripheral, which
  splitkb does with a cmake argument;
- the dependency pins in `config/west.yml` (splitkb builds from the moving
  `main+halcyon-fixes` branch).
- `config/halcyon_ferris_right.overlay`: the touchpad's taps are turned off
  (the chip's tap-to-click comes with a tap-and-drag gesture that misfires
  on short strokes); click and drag with the MB1/MB2/MB3 keys on the Mouse
  layer;
- wheel scrolling on the `&msc` keys at 100 units/s (top of the keymap).

```sh
nix build              # -> result/zmk_{dongle,left,right}.uf2
nix run .#flash        # double-tap reset on each part when prompted
nix run .#flash dongle # flash only one part (dongle | left | right)
git checkout main      # back to the tuned firmware
```
