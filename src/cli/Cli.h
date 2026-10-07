#pragma once

#include <QStringList>

namespace silma {

// True if argv names a CLI subcommand (as opposed to launching the GUI).
bool isCliInvocation(const QStringList& args);
// Runs the CLI; returns the process exit code.
int runCli(const QStringList& args);

} // namespace silma
