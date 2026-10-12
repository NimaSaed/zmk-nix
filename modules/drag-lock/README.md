# drag-lock — ZMK module

Provides the `zmk,input-processor-drag-lock` input processor (`&drag_lock`
in `config/halcyon_ferris_dongle.overlay`). See the binding in
`dts/bindings/input_processors/` for the properties and the top of
`src/input_processor_drag_lock.c` for how it works.

It is wired into the build via `extraCmakeFlags` (`-DZMK_EXTRA_MODULES`) in
`flake.nix`.
