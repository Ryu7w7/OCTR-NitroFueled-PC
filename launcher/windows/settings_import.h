// Bringing the player's settings from another installation of the port on this PC: a release
// unzipped into a new folder (instead of the launcher's own update, which keeps them) starts
// with the defaults, and the player had to set the controls, the nickname and the game folder
// again. Every launcher notes its folder in %APPDATA%\CTR-NitroFueled-PC\installs.txt; a new
// folder looks there, and next to itself for folders of releases older than that list, and
// offers to copy the previous installation's settings and data (never the game itself).
#pragma once
#include <filesystem>
#include <string>
#include <vector>

namespace SettingsImport {

/// A folder of the port that has been used: run.py next to bbport.ini or launcher.ini.
bool IsUsedInstall(const std::filesystem::path& dir);

/// Notes this installation in the list (installs_file: installs.txt), once.
void Remember(const std::filesystem::path& installs_file, const std::filesystem::path& root);

/// Other used installations, the most recently played first: those in the list that still
/// exist, and folders next to this one (its parent's and grandparent's subfolders, and their
/// CTR-NitroFueled-PC-portable folder, as the release ZIPs unpack).
std::vector<std::filesystem::path> Previous(const std::filesystem::path& installs_file,
                                            const std::filesystem::path& root);

/// What is copied (relative paths): settings, the online key, patch choices, and the user\
/// (saves, trophies, the player's patches; not the shader cache), dlc\ and backups\ folders.
std::vector<std::filesystem::path> Items(const std::filesystem::path& from);

/// Copies Items(from) into to, replacing what is there. "" when done, else the first error.
std::string Copy(const std::filesystem::path& from, const std::filesystem::path& to);

}  // namespace SettingsImport
