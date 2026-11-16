// Was der Lader (resolve_linux_vst.cpp) seinen Teilen bietet: Einstellungen je Plugin, Zustandsordner,
// Skalierungsfaktor und die Frage, welches Programm das ist. Der Log steht in vst2.h (logMessage).
#pragma once

#include <string>
#include <vector>

// Namen, unter denen ein Plugin in plugins.conf steht: pluginName(), bei VST3 dazu der Name
// des Bundles ohne .vst3 (eine Waves-Shell enthält Hunderte Plugins), und der Name mit dem
// Ordner davor (pluginNameWithFolder, bei VST3 der des Bundles).
using Names = std::vector<std::string>;

// Einstellung des Plugins aus plugins.conf, leer wenn nicht gesetzt.
std::string pluginSetting(const Names &plugin, const char *key);

// $XDG_STATE_HOME/resolve-linux-vst, wird angelegt.
std::string stateDir();

// Skalierungsfaktor des Fensters, in das das Plugin zeichnet; ohne Fenster der des Hauptbildschirms.
double editorScale(unsigned long long windowId = 0);

// Läuft die Bibliothek in Resolve, im VstScanner oder in editor-test?
bool isResolveProcess();
