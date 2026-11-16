// Einstellungen je Plugin aus plugins.conf. Vom Lader, der Editorgröße und der Wine-DPI gelesen.
#pragma once

#include <string>
#include <vector>

// Namen, unter denen ein Plugin in plugins.conf steht: pluginName(), bei VST3 dazu der Name
// des Bundles ohne .vst3 (eine Waves-Shell enthält Hunderte Plugins), und der Name mit dem
// Ordner davor (pluginNameWithFolder, bei VST3 der des Bundles).
using Names = std::vector<std::string>;

// Einstellung des Plugins aus plugins.conf, leer wenn nicht gesetzt.
std::string pluginSetting(const Names &plugin, const char *key);
