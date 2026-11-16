// Editorgröße: hängt sich zwischen Resolve und ein geladenes Plugin und rechnet die Fenstergrößen
// um (Erklärung in editor_size.cpp). Vom Lader und von der Wine-DPI genutzt.
#pragma once

#include "plugin_conf.h"
#include "vst2.h"

// Skalierungsfaktor des Fensters, in das das Plugin zeichnet; ohne Fenster der des Hauptbildschirms.
double editorScale(unsigned long long windowId = 0);

// Der Callback, den Resolve dem Lader beim Laden gibt; editorCallback leitet an ihn weiter.
void setEditorHost(audioMasterCallback callback);

// Callback für das Plugin: rechnet audioMasterSizeWindow um, alles andere geht an Resolve.
long editorCallback(AEffect *effect, int opcode, int index, long value, void *ptr, float opt);

// Setzt den Dispatcher des Plugins ein; `zoom` ist die Vergrößerung durch Wine (syncWineDpi).
void wrapEditor(AEffect *effect, const Names &names, double zoom);
