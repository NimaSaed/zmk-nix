# ZMK module: drag-lock input processor (see ../../flake.nix for how it is included).
zephyr_include_directories(${APPLICATION_SOURCE_DIR}/include)
zephyr_library_sources_ifdef(CONFIG_ZMK_INPUT_PROCESSOR_DRAG_LOCK src/input_processor_drag_lock.c)
