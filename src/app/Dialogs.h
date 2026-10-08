#pragma once

#include "Batch.h"
#include "Export.h"
#include "Importer.h"

#include <QDialog>

class QButtonGroup;
class QCheckBox;
class QComboBox;
class QLineEdit;
class QRadioButton;
class QSpinBox;

namespace silma {

class ImportDialog : public QDialog {
    Q_OBJECT
public:
    ImportDialog(const QStringList& files, const QList<ImportInfo>& imports, const QString& defaultName, QWidget* parent = nullptr);
    ImportOptions options() const;
    // Suggested name for a new import (it may arrive after the dialog opened); ignored
    // when the user already typed a name or picked a destination.
    void setDefaultName(const QString& name);

private:
    bool m_destinationChosen = false;
    QRadioButton* m_new;
    QRadioButton* m_existing;
    QLineEdit* m_name;
    QComboBox* m_imports;
    QCheckBox* m_split;
    QCheckBox* m_move;
};

enum class ExportScope { Selected, Import, Batch };

class ExportDialog : public QDialog {
    Q_OBJECT
public:
    ExportDialog(int selected, int inImport, int inBatch, QWidget* parent = nullptr);
    ExportOptions options() const;
    ExportScope scope() const;
    bool includeRejected() const;

private:
    QRadioButton* m_scope[3];
    QLineEdit* m_folder;
    QLineEdit* m_pattern;
    QSpinBox* m_quality;
    QComboBox* m_sizeMode;
    QSpinBox* m_size;
    QCheckBox* m_rejected;
};

} // namespace silma
