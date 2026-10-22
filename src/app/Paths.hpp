#pragma once
#include <string>
#include <vector>

namespace lr::app {
std::string configDir();                 // $XDG_CONFIG_HOME/msfs-linuxrebind
std::string configFile();                // .../config.json
std::string imguiIniFile();              // .../imgui.ini
std::string findDataFile(const std::string& name);  // data/<name> next to the exe, in share/, or in the source tree
// Every data file whose name ends with `suffix` (e.g. "_catalog.json"), de-duplicated by file name; the user
// config dir wins over the install dir so a hand-edited catalog overrides the shipped one.
std::vector<std::string> listDataFiles(const std::string& suffix);
}  // namespace lr::app
