# WindGame

A 2D retro-style action/adventure engine with an integrated HTML map editor. Targets **Windows / Linux (SDL2)** and the **ZX Spectrum Next (z88dk)** from a single shared codebase via a Hardware Abstraction Layer.

Repo: <https://github.com/omakuda/windgame>

---

## Quick Start

**Windows:**
```cmd
build.bat
build.bat run
```

**Linux:**
```sh
./build.sh
./build/out_linux/windgame
```

The build script auto-detects vcpkg. If `VCPKG_ROOT` is set in the environment, it's used directly; otherwise common install locations are searched. See [Building](#building) below.

---

## Project Layout

```
windgame/
  README.md                Engine & editor documentation (this file)
  build.bat / build.sh     Auto-configuring build scripts
  build/
    CMakeLists.txt         Cross-platform build config
    vcpkg.json             SDL2 + SDL2_mixer manifest
    Makefile.next          ZX Spectrum Next build (z88dk)
  src/
    hal/                   Hardware Abstraction Layer
      hal.h                Public API used by all game code
      hal_types.h          Screen/tile/sprite constants, color_t
      hal_keybinds.h       Key remapping, .cfg file format
      win64/hal_win64.c    SDL2 backend (Windows / Linux)
      next/hal_next.c      z88dk backend (ZX Spectrum Next)
    game/
      main.c               Scenes, player, action physics, debug menu
      maps.h               World/action/dungeon map data
      tunables.h           Runtime parameter struct + console table
      debug_menu.h         Debug menu defines, action registry
      overworld_events.c/h Event spawn AI, traveling encounters
      rng.h                16-bit xorshift PRNG
  tools/
    map_editor.html        Standalone browser-based map editor
```

---

## Engine Features

### Scenes
- **Overworld**: top-down free movement on tile-based maps. Multiple world maps registered in `world_maps[]` table; switchable from debug menu.
- **Action**: Zelda 2-style sidescroller with platforming, ladders, sword attacks, projectile weapons.
- **Scene transitions**: tile-triggered (transition tiles, dungeon entrances), with re-entry immunity to prevent immediate re-trigger.

### Action-Scene Physics
Ported from the Zelda 2 disassembly (FiendsOfTheElements/z2disassembly), scaled from NES 60fps to ~50fps:

- **Variable-height jump**: holding the jump button while rising applies lower gravity (`fy_grav_held`). Releasing applies normal gravity (`fy_grav`).
- **Running jump boost**: horizontal speed > 25% of max adds extra impulse (~0.75 tiles of height).
- **Air control**: independent air acceleration (`fx_air_accel`) lets the player steer mid-jump without exceeding the captured launch speed.
- **Ladders**:
  - Grab by pressing up/down while overlapping a ladder tile (zeroes horizontal motion).
  - Climbing to the top auto-dismounts onto the surface.
  - Standing on a ladder top and pressing down re-enters climbing mode.
  - Jump-off works from any rung.
- **Crouch**:
  - Hold down on the ground to crouch.
  - Disables horizontal input but preserves a small slide on entry that decays via friction.
  - Hitbox shrinks to one tile (10x14) aligned with the bottom sprite — duck under threats.
  - Hard landings (vspeed > threshold) force a brief crouch animation.
- **Ice tiles**: tile 7 in action scenes uses lower friction.
- **Terminal velocity** capped at `fy_max_fall`.

### Tile System
26 tile types covering both world and action scenes. Tile 7 (path/ice), tile 11 (town), tile 13 (forest), tile 24 (world_water) and tile 25 (world_road) are seamless tileable patterns.

- **Town and forest** patterns are loaded only on the overworld and blanked when entering action mode (`action_clear_ow_tiles()` / `ow_restore_tiles()`).
- **Water (tile 10)** is bidirectional/passable on the overworld.
- **Tile collision flags**: SOLID, PLATFORM, LADDER, DAMAGE, EXIT, TRANSITION, WATER, EMPTY.
- **Background tile layer**: `hal_tilemap_set_bg(data, w, h, repeat)` draws behind the foreground tilemap, visible where the foreground tile pixel is 0. `repeat=1` wraps the bg pattern across the entire map.

### Combat
- **Sword attack**: directional slash with adjustable duration.
- **Bow**: shoots arrows that travel in 8 directions, configurable speed and lifetime.
- **Equipment HUD** with consumable items (potions, ladder placement, etc.).
- **Invulnerability frames** after taking damage (`invuln_time`).
- **Health system**: 0..`player_hp` HP, restored at safe maps and via potions.

### Overworld Event System
Random encounters appear as walking sprites that pursue the player:
- **Spawn margin**: events spawn no closer than 3 tiles from the map edge.
- **Player-biased spawning**: 70% chance of spawning within ~8 tiles of the player.
- **Movement AI**: 60% chance to move toward the player, 40% random cardinal direction.
- **Never stop**: events always have a direction, even after collisions.
- **Edge bouncing**: at the 3-tile margin, events turn perpendicular to the edge, preferring the direction toward the player.
- **Encounter types**: combat (varying enemy strength), safe (lone NPC, caravan, oasis), discovery, dungeon entry.

### Debug Menu (open with BTN3 while paused)
Hierarchical 3-level navigation with scrolling support for long lists:
1. **Main menu**: Load Map / Character Settings / Back to Overworld / Controls / Heal Full / Exit Game.
2. **Load Map → category**: World Maps / Action Maps / Dungeons / Events.
3. **Category → item**: lists all available maps; selecting loads that scene.

For dungeons, a third level lists each room's labeled entry points (e.g. `left_door`, `right_ladder`); selecting one spawns the player directly there.

### Character Settings Console (Debug → Character Settings, or BTN3 from debug menu)
In-game runtime parameter editor with 17 tunables organized by category:

**Vertical physics**: `fy_grav`, `fy_grav_held`, `fy_jump`, `fy_max_fall`, `fy_climb`, `land_crouch_thr`, `land_crouch_frm`
**Horizontal physics**: `fx_max_speed`, `fx_accel`, `fx_air_accel`, `fx_air_max`
**Combat**: `attack_dur`, `invuln_time`, `player_hp`, `arrow_speed`, `arrow_lifetime`
**Overworld**: `ow_move_speed`

Controls: Up/Down to select, Left/Right to adjust by step, BTN1+arrows for fine +/-1 adjust, BTN2 to reset to default, BTN3 to exit.

### Keybind Editor
Remap any input via the controls submenu. Saved to `keybinds.cfg` next to the executable.

---

## Map Editor (`tools/map_editor.html`)

Open the file in any modern browser. No install or server required — uses File System Access API for direct file save/open when supported.

### Project Model
- **Project file (.wgp)**: JSON with all maps, manifest, settings.
- **Maps**: each has tiles, collision layer, optional spawn zones, env layer, NPCs, enemies, and parallax layers.
- **Roles**: each map can be assigned a role (overworld, field_combat, safe_lone, dungeon_rooms, etc.) which determines how the C export wires it into the game.
- **Save filename**: editable textbox in sidebar, with auto-increment toggle that adds/increments a 4-digit suffix on save.

### Editing Layers
Toggle between layers with the layer bar:
- **Tiles**: standard tile placement; tile palette is context-filtered (world maps hide ladder/platform/water; action maps hide town/forest/world_water/world_road).
- **Collision**: paint tile collision types separately from visuals.
- **Spawn**: zone painting for event spawn restrictions.
- **Environment**: per-tile environment ID (forest/desert/snow) for visual variants.

### Tools
- **Paint**: place tiles or collision IDs.
- **Select**: rectangle marquee, copy/paste with Ctrl+C/V.
- **NPC**: 6 NPC types with procedural sprites, place by clicking; enforces valid placement (not in walls, no overlaps).
- **Enemy**: 6 enemy types with procedural sprites; mirrors the NPC tool but for combat-side placement. Right-click removes.

### Dungeon View
For dungeon-type maps containing multiple rooms:
- **Overview mode**: full middle panel shows each room as a freeform draggable mini-map.
- **Scroll wheel zoom** (1-8x) shows room contents.
- **Double-click a room** to enter detailed editing.
- **Dungeon View** button returns to overview.
- **Right-click a room** starts a transition link: line follows the cursor, target rooms highlight green if they have transition tiles or red if not.
- **Transition Editor modal**: shows side-by-side mini-maps of both rooms with transition tile dropdowns. Existing bindings listed at the bottom; OK commits, Cancel discards.
- Transitions exported to C as `dungeon_transition_t[]` arrays.

### Parallax Scrolling
Toggle the **Parallax** checkbox in the toolbar to enable multi-layer mode for the current map:
- **4 layers**: L1 (foreground/`m.tiles`), L2, L3, L4 (background). L1 overlaps L2, etc.
- **Layer-edit buttons** (L2/L3/L4) appear in the layer bar.
- **Per-layer scroll speed**: L1=100% (fixed), L2/L3/L4 editable 0-100%. Defaults: L2=75%, L3=50%, L4=25%.
- **ShowID toggle**: overlays the layer number on each tile (only the topmost non-empty layer's number per cell).
- When editing one layer, others dim to 45% for visual focus.

### Parallax Preview Window
Click the **Preview** button (visible when parallax is on) to open a modal that auto-scrolls all layers at their configured speeds:
- Pause / Play toggle
- Left / Right direction toggle
- Speed input + Apply button
- Close button
- Yellow markers every 256px show screen-edge boundaries
- The map wraps horizontally for continuous scroll demo

### Day/Night Preview (dayNightExperiment branch)
Dropdown in the Environment section: Day / Transition 1-4 / Night. Applies a blue-shifted darkening overlay on the canvas so you can preview how tiles will look at any time of day.

### Export
- **Save** / **Save As**: writes `.wgp` JSON.
- **Export C**: generates `maps_data.h` consumed by `main.c`. Includes tile data, collision layers, NPC/enemy placements, room transitions, parallax settings, and a manifest mapping roles to their map data.

### Browser Storage
The editor uses the File System Access API (Chrome/Edge) for direct save. In other browsers it falls back to download dialogs. No data is sent over the network — everything stays local.

---

## Day/Night Cycle (experimental branch only)

Available on the `dayNightExperiment` branch. To try it:
```sh
git checkout dayNightExperiment
build.bat   # or build.sh
```

### Schedule
24 game-hours (1 hour = 30 real seconds at normal speed, full cycle = 12 minutes):
- **0400-0800**: night to day transition
- **0800-1600**: full day (8 hours)
- **1600-2000**: day to night transition
- **2000-0400**: full night (8 hours)

### Implementation
- A 256-entry ARGB32 LUT is rebuilt whenever brightness changes (cached, only when `s_dn_brightness` differs).
- Applied in `hal_frame_end`: every pixel goes through the LUT.
- R and G scale linearly with brightness; B retains a small floor at night for a subtle blue shift.
- **UI exemption**: `hal_daynight_ui_begin()` / `hal_daynight_ui_end()` mark UI pixels in a mask buffer; those pixels use the base palette so menus/HUD stay readable.
- **Per-tile night variants**: `hal_tiles_load_night(data, first, count)` loads alternate tile patterns that replace base patterns when brightness < 128. Only tiles with loaded variants swap.
- **Action scene slowdown**: time advances at 1/10 speed during `SCENE_ACTION` (frame accumulator).
- **HUD clock**: shows `H:MM` with always-two-digit minutes (8:00, 8:10), 10-minute increments, blinking colon every 0.5s.
- **dn_speed tunable** in the debug console: 0=frozen, 1=normal, up to 50x fast.

---

## Building

### Windows (Visual Studio + vcpkg)

**One-time setup:**
```cmd
git clone https://github.com/microsoft/vcpkg.git C:\vcpkg
C:\vcpkg\bootstrap-vcpkg.bat
setx VCPKG_ROOT C:\vcpkg
```
*(Restart your shell after `setx`, or set the var manually for the current session.)*

**Build:**
```cmd
build.bat              :: Release build
build.bat debug        :: Debug build
build.bat run          :: Build and launch
build.bat clean        :: Remove build artifacts
build.bat next         :: ZX Spectrum Next build (requires z88dk)
build.bat all          :: Windows + Next, package zip
build.bat help         :: Show all options
```

**vcpkg location detection:**
The script searches in this order:
1. `%VCPKG_ROOT%` environment variable (preferred, set by user)
2. `vcpkg.cmd` on `%PATH%`
3. `C:\vcpkg`
4. `C:\src\vcpkg`
5. `%USERPROFILE%\vcpkg`
6. `%USERPROFILE%\source\vcpkg`
7. `D:\vcpkg`

If none found, the script prints install instructions and exits.

**Manual SDL2 (no vcpkg):** set `SDL2_DIR` to your SDL2 SDK root before running `build.bat`.

### Linux (apt + system SDL2)

```sh
sudo apt install build-essential cmake libsdl2-dev libsdl2-mixer-dev
./build.sh
```

The Linux script uses system SDL2 by default; vcpkg is supported but optional.

### ZX Spectrum Next (z88dk)

Install z88dk from <https://z88dk.org> or your package manager and ensure `zcc` is on PATH, then:
```sh
make -f build/Makefile.next
```
or `build.bat next`.

---

## Game Controls (default)

| Action | Default Key |
|---|---|
| Move | Arrow keys |
| Jump (BTN1) | Z |
| Attack (BTN2) | X |
| Menu (BTN3) | Esc |
| Inventory | Tab |

Remap from the debug menu - Controls. Saved to `keybinds.cfg`.

---

## Branches

- **master**: stable trunk with all engine and editor features.
- **dayNightExperiment**: adds the day/night cycle, time-of-day preview, and timer HUD. To merge later: `git checkout master && git merge dayNightExperiment`.

---

## Credits

- Sidescrolling physics ported from <https://github.com/FiendsOfTheElements/z2disassembly>
- SDL2 for Windows/Linux backend
- z88dk for ZX Spectrum Next backend
