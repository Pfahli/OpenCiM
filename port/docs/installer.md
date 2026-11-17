# Installer design (owner's request 2026-11-17)

OpenCiM ships like OpenRCT2: a download that holds only the port's own files and needs the original Cities in Motion
installed. The installer should look like part of Cities in Motion, and while the files go in it shows the biggest
things the port adds. This page is the design; nothing is built yet (`playable-todo.md` §9 has the release items).

## 1. What it has to do

* Find Cities in Motion, the player's own copy (owner's rule 2026-11-17, for the legal side: the game's files always
  come from the player's installation, never from us). Either the installer detects it from Steam (registry key
  `HKCU\Software\Valve\Steam` → `SteamPath`, then every library in `steamapps\libraryfolders.vdf`, app id 73010; then
  the GOG/retail registry keys), or the player points it at the game folder. Check the archives the port needs
  (`regs-tool vfs <game> stats` already does this) and list the DLCs it finds; a folder without them is refused.
  Without the game it stops with "OpenCiM needs Cities in Motion" and a link to the Steam store page. The game's files
  are only read, never copied, changed or uploaded.
* Install per user without admin rights into `%LOCALAPPDATA%\Programs\OpenCiM` (a folder of the player's choice as an
  option): `regs.exe`, `regs-tool.exe`, `data\regs`, `data\mods`, licences, readme. Nothing goes into the game's folder,
  and saves and settings stay in `Documents\OpenCiM` (README, "Where data lives"), so an uninstall or update never
  touches them.
* Store the game folder it found in a small `install.cfg` next to `regs.exe`, so the first start needs no search.
* Start menu and optional desktop shortcut, an entry under Apps & features (`HKCU\...\Uninstall\OpenCiM`), an uninstaller
  that removes exactly what it installed (and asks before touching `Documents\OpenCiM`, default: keep).
* Get the newest build online, and update or uninstall later (owner's request 2026-11-17, §3a): the setup program is
  small, downloads the current release from the project's GitHub releases and, run again, offers UPDATE, REPAIR and
  UNINSTALL.
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
* The payload (the release folder) is a zip, downloaded at install time (§3a) or, for the offline installer, appended
  to the exe; extraction uses miniz (MIT, one file in `third_party/`). Shortcuts through `IShellLinkW`, the uninstall entry through the registry; `opencim-setup.exe
  --uninstall` is copied into the install folder as the uninstaller.
* Signed with the same certificate as `regs.exe` once there is one (unsigned self-extractors get SmartScreen warnings
  and antivirus false alarms; until then the zip release stays available as an alternative).

The alternative, **Inno Setup 6** with a fully custom-drawn wizard and a helper that extracts the game's menu art
into temp files before the wizard shows, gives the install mechanics (uninstall, upgrade, signing support) for free
but looks like a skinned Windows wizard, not like the game, and the art extraction is a fragile extra step. Keep it as
the fallback if the own program turns out to cost too much.

## 3a. Newest build from GitHub, updates and uninstall

Once the project is on GitHub (it has no remote yet), every release is a GitHub release with three files:
`OpenCiM-<version>-win64.zip` (the install folder), `OpenCiM-<version>-win64.zip.sha256` and `release.json` (version,
build number, date, minimum setup version, the feature list for the showcase, §5). There is a `stable` channel (GitHub
releases) and later a `nightly` channel (pre-releases from `main`).

* **Download:** the setup program asks `https://api.github.com/repos/<owner>/<repo>/releases/latest` (pre-releases for
  nightly), picks the win64 zip, downloads it with WinHTTP (no extra library; system proxy settings, resume after a
  dropped connection, progress on the showcase page's bar) and checks it against the `.sha256` before unpacking.
  Without a network it says so and offers the offline installer (the same exe with the zip appended), which is
  attached to each release too.
* **Run again = maintenance:** the setup program copies itself into the install folder (`opencim-setup.exe`, also the
  Apps & features "Modify/Uninstall" entry and a Start menu item). Started with an install present it opens the
  maintenance page: the installed version and the newest one, then
  * **UPDATE** (only when a newer release exists): downloads it, shows the showcase with only the features that are
    new since the installed version (`release.json` of each release in between), replaces the files listed in the old
    install's manifest, keeps options and the game folder;
  * **REPAIR:** downloads the installed version again and puts back missing or changed files;
  * **CHANGE GAME FOLDER:** the search or the folder picker again (another Steam library, a moved game);
  * **UNINSTALL:** removes exactly the files in the manifest, the shortcuts and the registry entry; saves, maps and
    settings in `Documents\OpenCiM` stay unless the player unticks "keep my saves and maps".
* **Manifest:** `install.json` in the install folder lists every installed file with its hash, the version, the channel
  and the game folder. Update and uninstall touch only those files, so mods or texture caches the player added to the
  folder survive an update.
* **Update check in the game:** `regs.exe` asks the same GitHub URL at most once a day in the background (an option on
  the Game page, on by default, off for offline play) and shows "OpenCiM <version> is available" in the main menu's
  version corner; clicking it starts `opencim-setup.exe --update` and quits the game. Nothing is downloaded without
  the player's click.
* **Self-update of the setup program:** when `release.json` asks for a newer setup version, the old one downloads the
  new `opencim-setup.exe` first and hands over to it.
* **Trust:** HTTPS only, hash check, later code signing of the setup program and `regs.exe`; the setup program never
  runs anything it downloaded except the new setup program from the same release.

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

6. **Maintenance** (the setup program started with an install present, §3a): installed and newest version, the
   game folder in use, and UPDATE, REPAIR, CHANGE GAME FOLDER, UNINSTALL; UNINSTALL asks once more with "keep my saves
   and maps" (on by default).

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

**The pictures (owner's decision 2026-11-17): screenshots, added later.** No pictures are rendered on the player's
machine. Each slide gets a screenshot of the port showing its feature, made when the release is put together; until
then the slides show the own painted street (§2) as a stand-in. The screenshots go with each GitHub release next to
`release.json` (`showcase/<n>.jpg`, about 1280 × 800, a few hundred KB each), so the setup program stays small, every
release can show its own new features, and the offline installer carries the same files appended. Screenshots show
game art (buildings, vehicles, terrain); the owner chose them over own illustrations, as store pages do.

## 6. Steps

1. The shared game search in `engine/` (also for the port's own first start), with tests on Steam libraries in other
   folders and a missing game.
2. `src/setup/` with the payload, install, shortcuts, uninstall entry, manifest and maintenance page, plain pages
   first; then the GitHub download, update, repair and the in-game update check (needs the GitHub repository).
3. The game's menu look (panel, bars, fonts, background from the install) and the own fallback art.
4. The showcase: slides, cross-fade, progress bar, the stand-in picture; the screenshots once they are made.
5. The release script (`tools/make-release.ps1`): build, collect the files, licences (the port's own, miniaudio, miniz,
   later Vulkan-Headers and glslang), zip, `.sha256`, `release.json`, the offline installer, sign; later a GitHub
   Actions workflow that does the same and publishes the release (and the nightly pre-release from `main`).
