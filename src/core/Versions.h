#pragma once
// Versions of the libraries Silmä is built with (for the About box).

#include <QList>
#include <QString>
#include <utility>

namespace silma {

// (name, version) of the major libraries, as reported at run time where they can.
QList<std::pair<QString, QString>> libraryVersions();

} // namespace silma
