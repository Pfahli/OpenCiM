# Installer design (owner's request 2026-11-17)

OpenCiM ships like OpenRCT2: a download that holds only the port's own files and needs the original Cities in Motion
installed. The installer should look like part of Cities in Motion, and while the files go in it shows the biggest
things the port adds. This page is the design; nothing is built yet (`playable-todo.md` §9 has the release items).

## 1. What it has to do

* Find Cities in Motion: Steam registry key `HKCU\Software\Valve\Steam` → `SteamPath`, then every library in
  `steamapps\libraryfolders.vdf`, app id 73010; then the GOG/retail registry keys; then a folder picker. Check the
  archives the port needs (`regs-tool vfs <game> stats` already does this) and list the DLCs it finds. Without the game
  it stops with "OpenCiM needs Cities in Motion" and a link to the Steam store page.
* Install per user without admin rights into `%LOCALAPPDATA%\Programs\OpenCiM` (a folder of the player's choice as an
  option): `regs.exe`, `regs-tool.exe`, `data\regs`, `data\mods`, licences, readme. Nothing goes into the game's folder,
  and saves and settings stay in `Documents\OpenCiM` (README, "Where data lives"), so an uninstall or update never
  touches them.
* Store the game folder it found in a small `install.cfg` next to `regs.exe`, so the first start needs no search.
* Start menu and optional desktop shortcut, an entry under Apps & features (`HKCU\...\Uninstall\OpenCiM`), an uninstaller
  that removes exactly what it installed (and asks before touching `Documents\OpenCiM`, default: keep).
* Update in place: a newer installer finds the old install, keeps the chosen options and only replaces files.
* Options page: shortcuts, "start OpenCiM when done", language (German/English, written to the first-start settings),
  and later the optional mods it ships with (weather) and the high-res texture generation (texture-upscaling branch),
  which would run as the last step.

## 2. How it gets the game's look without shipping game art

The look of Cities in Motion is its menu art: the dark brown leather-and-metal panel on the left, the bevelled button
bars with small coloured icons, white capitals in a plain sans serif, and the painted, sepia-tinted city street behind
(the screenshot `run/editor/menu-900.png` of the port's main menu). All of that is game data, which the port may not
ship (README, "Game data"). Two ways out, used together:

1. **Load the art from the player's own install at run time.** The game has to be found first anyway (§1), and the
   port's VFS reads its archives. So the installer's first step, finding the game, takes well under a second, and
   from the second page on it draws with the game's own menu background, panel textures, button bars and fonts, as
   the port's main menu does. Nothing of it is in the download.
2. **Own art for the parts that are shown before the game is found or when it is missing:** the OpenCiM title
   (`data/regs/ui/opencim-title.dds`, the port's own), a dark panel drawn from gradients in the game's colours
   (charcoal brown `#2b211b` → `#15100c`, bevel highlight `#6b5646`, text `#f1ece4`, accent the menu's blue
   `#2f6db5` and orange `#d9772b`), and a painted city scene of our own in the same style (sepia, blue sky, 1920s-60s
   facades, a tram), made once for the port and kept in `data/regs/ui/`.

That rules out off-the-shelf installer skins as the main route: Inno Setup or NSIS can show bitmaps and custom pages,
but they cannot read the game's archives, so they would need either shipped art or a helper that extracts it to temp
files first.

## 3. Technology: a setup program built from the port

**Decision (proposed): `opencim-setup.exe`, a small program in this repository** (`src/setup/`), built by CMake next
to `regs.exe`:

* The window, drawing and text use the port's own code (`platform/window_win32`, `render/renderer2d`, the VFS, the
  game's fonts once found), so pages look exactly like the game's menus and scale with DPI as the game's UI does.
* The game search is the same code the port needs for its own first start (`playable-todo.md` §9, "Find the Steam
  install by itself"), written once in `engine/` and used by both.
* The payload (the release folder) is a zip appended to the exe; extraction uses miniz (MIT, one file in
  `third_party/`). Shortcuts through `IShellLinkW`, the uninstall entry through the registry; `opencim-setup.exe
  --uninstall` is copied into the install folder as the uninstaller.
* Signed with the same certificate as `regs.exe` once there is one (unsigned self-extractors get SmartScreen warnings
  and antivirus false alarms; until then the zip release stays available as an alternative).

The alternative, **Inno Setup 6** with a fully custom-drawn wizard and a helper that extracts the game's menu art
into temp files before the wizard shows, gives the install mechanics (uninstall, upgrade, signing support) for free
but looks like a skinned Windows wizard, not like the game, and the art extraction is a fragile extra step. Keep it as
the fallback if the own program turns out to cost too much.

## 4. Pages

All pages share the game's main-menu layout: the dark panel on the left (OpenCiM title on top, the page's buttons as
the menu's bevelled bars below), the painted city filling the right side, the version in the bottom right corner as on
the main menu.

1. **Welcome.** "OPENCiM – a new engine for Cities in Motion", a line that it is unofficial and needs the original,
   buttons INSTALL, OPTIONS, CANCEL. The game search runs here in the background and its result shows as a line under
   the title ("Cities in Motion found: Steam, 4 DLCs").
2. **Game not found** (only then). Folder picker bar, "Buy on Steam" bar, a short explanation.
3. **Options.** Install folder, shortcuts, language, start when done, optional extras; the menu's checkbox art.
4. **Installing: the feature showcase.** The progress as the game's loading bar along the bottom of the picture; the
   right side becomes a slideshow of the biggest new features, one every 6-8 seconds with a cross-fade, each a
   picture with a heading and two lines of text in a caption bar in the panel style. Clicking the picture moves on;
   the slideshow keeps going after the copy finishes until the player clicks CONTINUE, so a fast install still shows
   a few.
5. **Done.** PLAY (starts `regs.exe`), "What's new" (opens the release notes), CLOSE.

The uninstaller uses the same window: one page with "keep my saves and maps" (on by default) and UNINSTALL.

## 5. The features to show

Picked from what is on `main` today and ordered by how much they change the game (update the list per release):

| # | Feature | Heading | Two lines |
|---|---------|---------|-----------|
| 1 | OSM import | Your own city | Build a map of any real town from OpenStreetMap: streets, rail, rivers, parks and buildings. |
| 2 | Curved roads | Free road building | Curves, roundabouts, one-way streets, avenues, tunnels, bridges and express ramps. |
| 3 | Weather and seasons | Rain, snow and autumn | Weather that changes the streets, seasons that colour the trees (weather mod). |
| 4 | Night lights | Cities at night | Street lamps, lit windows, headlights and lit trams after dark. |
| 5 | Trams on rail | More ways to run lines | Trams on railway tracks, tram stops at stations, metro trains turning at end stations. |
| 6 | Big maps and editor | Room to build | Maps up to 2048 cells, map templates, autosave and new building tools. |
| 7 | Graphics | A sharper look | Better shadows, HDR, lens blur and tilt-shift, UI scaling for big screens. |
| 8 | Mods | Made to be extended | A mod manager for new content, loaded beside the game, never changing its files. |

Later releases add Vulkan with raytracing and upscaling, and the high-res textures, when they are done.

**The pictures:** screenshots of the port show game art (buildings, vehicles, terrain), so shipping them in the
installer would ship game data in another form. Two ways, owner's decision:

* **Rendered on the player's machine (preferred):** the installer starts `regs.exe` with a showcase scene list
  (`--showcase`, the `--at` camera actions and `shot` the test runs already use) right after the files are in place,
  hidden, and shows the pictures as they come, the first slides with own illustrations while it renders. Nothing of
  the game is in the download and the pictures always match the version installed. Cost: about 20-40 seconds of
  rendering and a fixed showcase map shipped with the port (an OSM import of a free place, made from our data only).
* **Own illustrations** in the painted style of the menu (made once, kept in `data/regs/ui/installer/`): no game
  content, no render step, but they show the idea rather than the real game.

Screenshots of the game itself in the installer (what store pages do) are left out because of the project rule.

## 6. Steps

1. The shared game search in `engine/` (also for the port's own first start), with tests on Steam libraries in other
   folders and a missing game.
2. `src/setup/` with the payload, install, shortcuts, uninstall entry and uninstaller, plain pages first.
3. The game's menu look (panel, bars, fonts, background from the install) and the own fallback art.
4. The showcase: slides, cross-fade, progress bar; then the rendered pictures (`--showcase`).
5. The release script (`tools/make-release.ps1`): build, collect the files, licences (the port's own, miniaudio, miniz,
   later Vulkan-Headers and glslang), zip, append to the setup exe, sign.
