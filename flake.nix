{
  description = "ZMK firmware for my splitkb Halcyon Ferris (ported from my ZSA Voyager layout)";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixpkgs-unstable";

    zmk-nix = {
      url = "github:lilyinstarlight/zmk-nix";
      inputs.nixpkgs.follows = "nixpkgs";
    };
  };

  outputs = { self, nixpkgs, zmk-nix }: let
    forAllSystems = nixpkgs.lib.genAttrs (nixpkgs.lib.attrNames zmk-nix.packages);
  in {
    packages = forAllSystems (system: rec {
      default = firmware;

      firmware = zmk-nix.legacyPackages.${system}.buildSplitKeyboard {
        name = "firmware";

        src = nixpkgs.lib.sourceFilesBySuffices self [ ".board" ".cmake" ".conf" ".defconfig" ".dts" ".dtsi" ".json" ".keymap" ".overlay" ".shield" ".yml" "_defconfig" ];

        # Three parts: the dongle is the central (USB/BT host), both halves are
        # peripherals. The left half additionally needs CONFIG_ZMK_SPLIT_ROLE_CENTRAL=n
        # (see config/halcyon_ferris_left.conf) since the shield defaults it to central.
        parts = [ "dongle" "left" "right" ];
        centralPart = "dongle";

        board = {
          dongle = "halcyon_dongle//zmk";
          _      = "halcyon_wireless//zmk";
        };

        # Coin cell battery boards, e-paper display module on the left, Cirque
        # touchpad module on the right (its input is forwarded to the dongle,
        # hence mod_cirque_central on the dongle).
        # E-paper artwork: mod_display_epaper_{forest,mountain,cityscape}
        shield = {
          dongle = "halcyon_ferris_dongle mod_cirque_central";
          left   = "halcyon_ferris_left mod_battery_coincell mod_display_epaper_forest";
          right  = "halcyon_ferris_right mod_battery_coincell mod_cirque_hw_right";
        };

        zephyrDepsHash = "sha256-EDv4GgSurJs+ei42LOQIoKjeMJjJBg9g4WeUD4usDcQ=";

        meta = {
          description = "ZMK firmware for the Halcyon Ferris";
          license = nixpkgs.lib.licenses.mit;
          platforms = nixpkgs.lib.platforms.all;
        };
      };

      flash = zmk-nix.packages.${system}.flash.override { inherit firmware; };
      update = zmk-nix.packages.${system}.update;
    });

    devShells = forAllSystems (system: {
      default = zmk-nix.devShells.${system}.default;
    });
  };
}
