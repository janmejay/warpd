# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this project is

`warpd` is a modal keyboard-driven mouse pointer controller written in C (plus Objective-C on macOS). It runs as a background daemon that grabs activation hotkeys and then takes over keyboard input to move/click the mouse via one of several modes (hint, hint2, grid, normal, history, screen-selection, hintspec).

## Build / run

The top-level `Makefile` auto-detects the platform from `uname -s` (Darwin → macOS) and includes the matching fragment from `mk/`:

- **Linux**: `make` builds `bin/warpd` with both X11 and Wayland support. Set `DISABLE_X=1` or `DISABLE_WAYLAND=1` to drop one backend. Linux dev deps are listed in `README.md`.
- **macOS**: `make` produces an ad-hoc–signed `bin/warpd` linked against Cocoa/Carbon/ApplicationServices. `make rel` cross-builds a universal (arm64 + x86_64) binary and packages a tarball under `dist/`. Recent commits removed all calls to `codesign/sign.sh` because signing breaks the Nix build on macOS — do not re-introduce signing steps.
- **Windows**: `PLATFORM=windows make` cross-compiles with `x86_64-w64-mingw32-gcc` (see `mk/windows.mk`).
- **Install**: `sudo make install` (Linux uses `$PREFIX`, default `/usr/local`; macOS also installs `files/com.warpd.warpd.plist` as a launchd agent).
- **Man page**: `make man` regenerates `files/warpd.1.gz` from `warpd.1.md` via `scdoc`.

There is no test suite. Verify changes by running `bin/warpd -f -d` (foreground + debug to stderr) and exercising the affected mode interactively. `WARPD_DEBUG=1` in the environment is equivalent to `-d`.

Useful runtime flags while developing:

- `-f` foreground, `-d` debug logging to stderr
- `-c <file>` use a non-default config (default lives under `$XDG_CONFIG_HOME/warpd/config` or `~/.config/warpd/config`)
- `--hint`, `--hint2`, `--grid`, `--normal`, `--screen`, `--history` run a single mode and exit (combine with `--oneshot` for scripting). Wayland has no global hotkeys, so the compositor invokes these one-shot flags.
- `-l` lists every key name the active platform recognizes; `--list-options` dumps every config key with its default.

## Architecture

### Platform abstraction (the key idea)

All OS-specific work goes through a single function-pointer vtable, `struct platform` (`src/platform.h`). Cross-platform code in `src/*.c` only ever calls `platform->mouse_move(...)`, `platform->input_wait(...)`, `platform->hint_draw(...)`, etc. A backend implements the struct and hands it to `platform_run(main_fn)`, which then calls the supplied entry point (`oneshot_main`, `daemon_main`, or `print_keys_main` in `src/warpd.c`). This is why almost no `#ifdef __APPLE__` exists outside the platform directories.

Backends live under `src/platform/`:

- `linux/` — `linux.c` plus subtrees `X/` (X11 backend, `WARPD_X=1`) and `wayland/` (`WARPD_WAYLAND=1`). The Linux binary picks the backend at runtime based on whether `$WAYLAND_DISPLAY` etc. is set; both can be compiled in.
- `macos/` — Objective-C `.m` files using Cocoa/Carbon. `macos.m` provides `platform_run`; `input.m`, `mouse.m`, `screen.m`, `hint.m`, `window.m`, `filemon.m` implement the vtable. macOS requires Accessibility permission at runtime.
- `windows/` — minimal Win32 backend (`windows.c`, `winscreen.c`, `filemon.c`). On Windows the build also pulls `src/windows/main.c` and `stubs.c` instead of `src/warpd.c` (see `mk/windows.mk`'s `OBJFILES` filter that excludes `warpd.c`).

When adding a feature that needs OS services, extend `struct platform` in `platform.h` and implement the new method in every backend — do not call OS APIs directly from cross-platform code.

### Daemon and mode loop

`main()` in `src/warpd.c` parses flags, takes an exclusive `flock` on `/tmp/warpd_<uid>.lock`, daemonizes (unless `-f`), and calls `platform_run(daemon_main)`. `daemon_main` parses config and enters `daemon_loop` (`src/daemon.c`).

`daemon_loop` uses `platform->input_wait()` (the only "efficient ungrabbed wait" primitive) to listen for one of the configured `*_activation_key` events. When one fires, it maps it to a mode constant and calls `mode_loop()` (`src/mode-loop.c`). `mode_loop` is the state machine that runs the chosen mode, then transitions to whichever other mode the mode's exit event requests, until the user exits.

The mode implementations (`hint.c`, `grid.c`, `normal.c`, `history.c`, plus `hintspec_mode` and `screen_selection_mode`) each grab the keyboard via `platform->input_grab_keyboard()`, consume events via `platform->input_next_event()`, draw via `platform->hint_draw` / `screen_draw_box`, and return either to `mode_loop` or directly when one-shot.

`platform->monitor_file(config_path)` causes `input_wait` to return `NULL` when the config file changes — `daemon_loop` treats that as a reload signal and re-parses without restarting.

### Config

`src/config.c` owns a static table of every option (name, default, description, type) and a linked list `config` of currently-effective values. `parse_config()` populates it from disk; `config_get()` / `config_get_int()` read it. `config_input_match(ev, "key_name")` is the standard way mode code checks "did the user press the configured key for X" — it's whitelist-aware so unrelated keys can be matched without colliding with activation keys.

Keys are written in a small DSL parsed by `input_parse_string` (e.g. `A-M-x` = Alt+Meta+x). Modifier bits are defined in `platform.h` (`PLATFORM_MOD_*`). The per-platform `input_lookup_code`/`input_lookup_name` translate between string names and the platform's keycodes.

### Other notable bits

- `mouse.c` / `scroll.c` implement the kinematic model used by normal mode (accel/decel, repeat).
- `history.c` keeps an in-memory ring of recent pointer targets; `histfile.c` persists them to `$XDG_DATA_DIR/warpd/` (or `~/.local/share/warpd/`) for `history_hint_mode`.
- `grid_drw.c` and `hint.c` are the cross-platform drawing logic that builds box geometry; the actual pixels are pushed via `platform->hint_draw` / `screen_draw_box` / `commit`.
- Object files (`.o`) are committed into `src/` — `make clean` only removes the ones produced by the current platform's `OBJECTS` list, so stale objects from a different platform can hang around.

## Conventions in this codebase

- C99 with `-Wall -Wextra -pedantic`. `warpd.h` is the umbrella header that every translation unit includes; new shared declarations belong there. The macOS backend additionally includes `src/platform/macos/macos.h` from its `.m` files.
- Debug output uses an inline `DEBUG_PRINT` macro (see `daemon.c`, `mode-loop.c`) gated on the global `warpd_debug_enabled`. Stick to that pattern instead of bare `fprintf(stderr, ...)`.
- The header comment in `warpd.h` explicitly warns that the code is rough and platform-ugly. When tidying, prefer pushing platform-specific code behind the vtable rather than scattering `#ifdef`s through cross-platform files.
