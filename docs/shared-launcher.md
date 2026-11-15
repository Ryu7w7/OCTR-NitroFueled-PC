# Shared Windows and Linux launcher

The maintained launcher UI is `launcher/launcher.cpp`. Both builds use the same ImGui pages,
artwork, navigation, palette, translations, settings, mods, saves, and trophy presentation.
Native services live in `launcher/windows/` and `launcher/linux/`; changes to a page belong in
the shared source. The Windows compatibility entry point includes that source.
Scrollbar and table colors follow the same palette on both platforms. Navigation keeps the
original button height and reserves scrollbar space. Smaller windows scroll the navigation
instead of shrinking its buttons. Control tables wrap labels and stack actions when narrow. Sidebar padding is symmetric, selection stays inside its bounds, and icon detail colors invert for the light selection. The concave Play arrow uses ImGui's concave polygon fill.
All eight launcher tables use `launcher/ui/table.h`. Its `LauncherUI::Table` component owns
the palette, borders, padding, wrapped headers and cells, column layout, and action placement.
Pages provide their data and callbacks; the component closes each table automatically.

Linux uses SDL3's Vulkan renderer (with OpenGL fallbacks) and native file dialogs. Windows keeps Win32 and Direct3D 11.
Both backends wait for events when idle and stop drawing while minimized or hidden. Linux
checks events and settings four times per second when idle; input wakes the window immediately. Active interaction uses display vsync, or a 16 ms wait without it. Linux
uses FreeType for system fonts, including variable CJK fonts; the bundled CTR heading font
and artwork are shared. Native file dialogs and system fallback fonts follow the desktop.

## Build and run on Linux

```sh
git submodule update --init gpu/third_party/imgui
bash launcher/build.sh
bash launcher/bb-launcher.sh
bash launcher/bb-launcher.sh --play
```

The standalone build needs a C++20 compiler, CMake, Ninja, pkg-config, SDL3 3.2 or newer,
libpng, OpenSSL, FreeType, Ayatana AppIndicator GLib and libdbusmenu-glib. `packaging/ayatana-glib.nix` pins the GTK-free tray library for both Nix shells and CI. Python 3 runs the existing game preparation and Linux services.
Fontconfig discovers desktop fonts. `launcher/shell.nix` supplies the build dependencies.
`build.sh` also builds the launcher; the AppImage includes its binary, art and runtime libraries.
The UI and tray do not require GTK or libadwaita. Linux uses Ayatana AppIndicator GLib with GMenu/GAction; it does not load SDL's deprecated GTK tray backend.
It also exports DBusMenu through libdbusmenu-glib for KDE and other hosts that do not support GMenu.
The old GTK script remains in the source tree for comparison and manual recovery during migration.
Release packages always start the shared native launcher; they do not fall back to GTK.

## Data and compatibility

The source/package directory contains scripts and artwork. Mutable files use `BB_DATA_DIR`
when set. Otherwise Linux automatically uses an existing `$XDG_DATA_HOME/sacredfire`
(normally `~/.local/share/sacredfire`) containing `bbport.ini` or `launcher.ini`, including when
started from a new checkout. Without existing XDG settings, a source build uses its checkout;
the packaged launcher defaults to the XDG location. `BB_CONFIG` and `BB_USER_DIR` remain
supported. Saves, backups, DLC and online keys stay in their existing locations.
Cloud save operations use the same configured save directory as local save operations.

Linux records hashes of bundled XML copies in `patches/.bundled.json`. Updates replace only
unchanged managed copies and remove unchanged copies no longer bundled. Edited files and
untracked files that differ from the current bundle remain user overrides. Identical legacy
copies are adopted automatically, so later package updates can refresh them safely.

On first use, Linux imports `~/.config/sacredfire-launcher/settings.json` (or its
`XDG_CONFIG_HOME` equivalent) into `launcher.ini`. Existing native settings take precedence;
the original GTK preferences are preserved. Common game settings remain in `bbport.ini`.
Legacy Pro mode is converted to an internal resolution when no resolution was already selected.
Linux starts `run.sh` in a dedicated process group so Stop also reaches child processes.
SIGINT and SIGTERM also stop the game; unresponsive processes are killed after two seconds.
Arguments are passed directly to processes, without shell evaluation. Game environment overrides
apply only to that launch, preserving the launcher's environment and later launches.
Steam uses the Windows artwork and keeps an existing shortcut's app ID. Shortcut icons are
copied to the XDG icon directory so they remain available after an AppImage unmounts.

## Platform capabilities

| Feature | Windows | Linux |
| --- | --- | --- |
| Shared UI, artwork and translations | Yes | Yes |
| Vulkan engines, mods, saves, DLC, trophies, input mapping | Yes | Yes |
| RyuNET, account login, Discord presence/invites, NAT test | Native Windows services | Existing Linux services and POSIX sockets |
| Desktop and Steam shortcuts | Windows shortcuts and registry | Desktop entries and existing Steam integration |
| Updates | Verified portable Windows ZIP | Verified AppImage; source/package-manager installs update externally |
| EOS bridge | Windows executable | Native POSIX executable; same bridge protocol and UI |
| NVIDIA DLSS | NGX DLL on supported NVIDIA RTX hardware | Native NGX shared library on supported NVIDIA RTX hardware |
| D3D11 game renderer | Experimental | Unavailable; use Vulkan |
| Minimize to tray | Native notification icon | Ayatana GLib tray on desktops with a StatusNotifier/AppIndicator host |
| MangoHud, Linux validation and environment options | Not shown | Linux settings section |

Linux registers a per-user `discord-<app id>://` handler on startup, so an accepted invite can
open the native launcher when it is closed. The handler uses the persistent AppImage command
and preserves the selected data directory and explicit configuration/save paths. Discord delivers
the pending invite over IPC after the launcher subscribes, using the same flow as Windows.
Registration uses GIO, supplied by GLib in the launcher shell and AppImage.

Both EOS builds require the Epic SDK and product configuration. When these are not packaged,
direct RyuNET remains available. Linux uses its own bridge executable; it does not require Wine.
D3D11 remains Windows-specific; adding a compatibility renderer would require a separate project. HDR support still depends on the display,
compositor and driver. Native dialogs require the desktop's SDL dialog backend/portal.

## Live configuration and shared icon

The launcher and both in-game menus share `BB_CONFIG` (`bbport.ini` by default). ImGui edits
save while the menu stays open, with a 150 ms throttle during drags. Native menu edits save
immediately. The launcher checks file changes every 500 ms; the game checks every 250 ms.
Both merge external changes before writing and hold the same stable `.lock` sidecar across
read/merge/replace. Local edits win only for fields edited locally; unrelated fields and comments
survive. Restart rereads the current configuration and rebuilds generated renderer environment.

Native labels refresh from guest input polling, even with no button pressed. Guest objects are
updated only on their owning UI thread and discarded on Back, page transitions and map loads.
Volume, fullscreen, pacing and supported live graphics changes apply without closing either menu.
Startup patches, renderer selection and other engine restart requirements still need a restart;
synchronizing the saved value does not make those engine operations hot-reloadable.

The launcher window, game window, Windows resources, tray and installed shortcuts use the same
rounded transparent 256 px user-supplied source in `launcher/io.github.sacredfire.Launcher.png`. Windows ICO
contains sizes 16 through 256 px. Tray menus provide Open launcher, Start/Quit game and Exit
launcher. Without a supported tray host, Linux minimizes normally so the window remains reachable.
The shared icon has a 24 px transparent margin on its 256 px canvas. Linux refreshes its
installed desktop icon on startup. Both platforms create their tray icon when the launcher opens.
Tray left-click toggles the launcher between visible and hidden; right-click opens its menu.
The first menu action is Minimize launcher while visible and Open launcher while hidden.
The taskbar entry remains until tray registration succeeds; losing the tray connection restores the launcher.

## Optional native libraries in CI

Release jobs fetch a pinned NVIDIA SDK revision through `scripts/fetch_dlss_sdk.py`, build the
platform bridge, and package the runtime and its license. Linux's `dlss_bridge/build.sh` writes
`out/dlss/`; a checkout discovers it next to `out/sacredfire`. Packages place both libraries in
`bin/`. `BB_DLSS_DIR` can select another library directory. NGX cache files live in the writable
user directory, outside the read-only AppImage.

Configure repository secrets `EOS_SDK_URL` (an HTTPS ZIP URL for the licensed Epic SDK) and
`EOS_BRIDGE_INI` (the product configuration) to include EOS in both release packages. The SDK
ZIP must contain `SDK/Include/eos_sdk.h` and the libraries for the target platform. Configuration
is embedded by the existing build script; raw settings and SDK headers are excluded from packages.
Both release jobs and local builds use the same automatic EOS hook from the normal build command.
With no EOS inputs, direct online play remains enabled; incomplete configured inputs fail the build.
The maintainer setup and single-workflow Windows/Linux build are documented in [eos-bridge.md](eos-bridge.md#2-build-the-game-and-eos-together).

## Verification

```sh
cmake -S launcher -B out/launcher -G Ninja -DBUILD_TESTING=ON
cmake --build out/launcher
VK_DRIVER_FILES=/usr/share/vulkan/icd.d/lvp_icd.x86_64.json ctest --test-dir out/launcher --output-on-failure
python3 -m unittest discover -s tests -p 'test_native_launcher.py' -v
```

The process test covers argument quoting, Unicode, stdin, output pressure, exit codes and
process-group timeouts. Settings import tests cover ordering, Unicode and case-sensitive paths;
the NAT test validates protocol parsing without network access. With Xvfb installed, the UI test
renders all ten shared pages into `out/launcher/ui-test/` using isolated data. It checks artwork,
keyboard and virtual gamepad capture (including touchpad), custom cloud save paths, game
termination, environment isolation and failed updates. Mesa lavapipe
also supports GPU-free runs. Migration and Linux service tests never use a real account or the
player's settings.

Local verification covers the native Linux launcher, concurrent settings
saves, idle native refresh, EOS process/protocol fixtures and real Linux NGX initialization on an
RTX 3050 Laptop (driver 595.112.07). EOS service login and DLSS image quality require gameplay checks. Real gameplay,
live account/Discord integration, physical controller behavior, HDR and native Windows execution
still require testing on their target systems. The full Nix game/launcher build and finished
AppImage upgrade check pass locally, including the bundled Linux DLSS bridge ABI check.
The final changes are built only for Linux. On a desktop with a tray host, run the UI test with
`BB_TEST_TRAY=1` to check registration, exported DBusMenu labels and activation, host-loss recovery,
and exit confirmation. The default UI test checks the fallback without a session bus.
For native Wayland, use `BB_TEST_TRAY_ONLY=1 SDL_VIDEODRIVER=wayland` with `launcher-ui-test`
and the same source/data arguments. This exercises minimize, hide, and Open through D-Bus
without the Xvfb screenshot fixtures. Restore clears SDL's saved minimized state before Show.

## Releases and upgrades from GTK

Pull requests and pushes to `main` build the shared launcher on Linux and Windows. Release
packages depend on both launcher jobs and the existing source checks. The Linux job exercises
the legacy updater and a subsequent native update against the finished AppImage before uploading it. A failed upgrade or
migration check blocks the draft release. The release remains a draft until published.
The Windows game build uses the same standalone launcher CMake target as CI, preserving the
portable ZIP's executable path.

Keep the published asset name `SacredFire-<version>-x86_64.AppImage` and publish a newer stable
version in the same GitHub repository. The GTK launcher's existing updater recognizes it,
replaces the AppImage at its existing path and restarts into the native launcher. No separate
launcher installation or manual settings export is needed.

### Release instructions for Ryu

Use the shared C++ launcher for this release and every later Linux release. Merge these changes
before building, then use `.github/workflows/release.yml` or the normal `build.sh` and
`packaging/appimage.sh` sequence. The package entry point in `packaging/default.nix` must keep
starting `bin/sacredfire-launcher`. Do not point it at `launcher/sacredfire_launcher.py` or
replace the release artifact with a package built from the old GTK branch.

Publish the newer stable AppImage in `Ryu7w7/OCTR-NitroFueled-PC`, which existing launchers
already check. Keep the asset naming above and the CI upgrade check enabled for every release.
A draft or prerelease is not offered by the stable updater. The first update replaces GTK with
the native launcher; later updates replace that launcher with the newer native build at the
same AppImage path, preserving desktop and Steam shortcuts.

After a successful download, size/hash verification and replacement, the native launcher saves
its settings and automatically restarts the AppImage with `--updated`, matching GTK and Windows.
If the game starts during the download, restart waits until the game exits. A failed download
keeps the existing AppImage; a failed restart leaves the launcher open with a manual Restart button.
Configure EOS separately using the [maintainer setup](eos-bridge.md#build-the-game-and-eos-together);
SDK secrets must be supplied before CI can include EOS for either platform.

### Settings retained across updates

On first start, missing `settings.json` values are merged into `launcher.ini`; existing native
values win. `gtk_migration=1` prevents reimport after later edits. The original JSON remains
unchanged. Existing INI files receive a `.pre-native.bak` backup before migration changes them.
Game settings, input bindings, mods, saves, trophies, DLC and online keys stay in the same data
directory. Invalid JSON or a write failure stops startup rather than replacing settings with
defaults; the desktop displays the error.

The native updater clears libraries and font paths inherited from the old AppImage before
restarting, while retaining custom data/config/save paths. Already released GTK launchers may
discard custom `BB_*` environment overrides during restart; those users must reopen with their
usual environment. The standard XDG paths are preserved by those old launchers too.

Run the upgrade check against the finished AppImage, or use `--native` for a standalone build:

```sh
python3 packaging/test_launcher_upgrade.py dist/SacredFire-x86_64.AppImage
python3 packaging/test_launcher_upgrade.py out/launcher/sacredfire-launcher --native
```

It covers GTK-to-native and native-to-native replacement, every GTK preference, retained
game/input settings and user files, native edits surviving the next update, and malformed
preferences, using temporary directories and a local download fixture. The UI test checks
automatic restart, deferral while the game runs, and no repeated restart after a failure.
