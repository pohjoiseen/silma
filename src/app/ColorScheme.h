#pragma once
// Application color scheme. Defaults to a dark scheme (Breeze Dark when the KDE color
// scheme framework is available) regardless of the desktop's scheme, because photos are
// best judged against a neutral dark surround.

#include <QMenu>

namespace silma {

// Applies the saved scheme (or the dark default). Call once after QApplication exists.
void applyColorScheme();

// Menu listing the available schemes; selecting one applies and remembers it.
QMenu* createColorSchemeMenu(QWidget* parent);

} // namespace silma
