#pragma once
// Raw files used by the tests, under test-raws/ (not in git; see test-raws/README.md).

#include <QDir>
#include <QFileInfo>
#include <QString>

// Path of a test raw relative to test-raws/, or empty when it is not there.
inline QString testRaw(const QString& relPath)
{
    const QString p = QStringLiteral(SILMA_TEST_RAWS "/") + relPath;
    return QFileInfo::exists(p) ? p : QString();
}

// All files of a test-raws/ directory, sorted by name.
inline QStringList testRaws(const QString& dir)
{
    QDir d(QStringLiteral(SILMA_TEST_RAWS "/") + dir);
    QStringList l;
    for (const QString& f : d.entryList(QDir::Files, QDir::Name))
        l << d.filePath(f);
    return l;
}
