#pragma once
#include <string>

namespace lr::app {
std::string configDir();                 // $XDG_CONFIG_HOME/msfs-linuxrebind
std::string configFile();                // .../config.json
std::string imguiIniFile();              // .../imgui.ini
std::string findDataFile(const std::string& name);  // data/<name> next to the exe, in share/, or in the source tree
}  // namespace lr::app
