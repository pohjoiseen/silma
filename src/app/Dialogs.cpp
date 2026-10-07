#include "Dialogs.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QSettings>
#include <QSpinBox>
#include <QStandardPaths>
#include <QVBoxLayout>

namespace silma {

ImportDialog::ImportDialog(const QStringList& files, const QList<ImportInfo>& imports, const QString& defaultName, QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Import Photos"));
    auto* l = new QVBoxLayout(this);
    l->addWidget(new QLabel(tr("%n raw file(s) selected.", nullptr, int(files.size())), this));

    auto* g = new QGroupBox(tr("Destination"), this);
    auto* gl = new QGridLayout(g);
    m_new = new QRadioButton(tr("New import:"), g);
    m_name = new QLineEdit(defaultName, g);
    m_existing = new QRadioButton(tr("Existing import:"), g);
    m_imports = new QComboBox(g);
    for (const auto& im : imports)
        m_imports->addItem(QStringLiteral("%1 (%2 photos)").arg(im.name).arg(im.photoCount), im.name);
    gl->addWidget(m_new, 0, 0);
    gl->addWidget(m_name, 0, 1);
    gl->addWidget(m_existing, 1, 0);
    gl->addWidget(m_imports, 1, 1);
    m_split = new QCheckBox(tr("One import per capture date"), g);
    m_split->setToolTip(tr("Files are grouped into imports named after their capture dates (YYYY-MM-DD)"));
    gl->addWidget(m_split, 2, 0, 1, 2);
    l->addWidget(g);

    m_move = new QCheckBox(tr("Move files instead of copying"), this);
    l->addWidget(m_move);

    // An import with the default name already existing: preselect it.
    int existingIdx = m_imports->findData(defaultName);
    if (existingIdx >= 0) {
        m_existing->setChecked(true);
        m_imports->setCurrentIndex(existingIdx);
    } else {
        m_new->setChecked(true);
    }
    m_existing->setEnabled(!imports.isEmpty());
    auto sync = [this] {
        m_name->setEnabled(m_new->isChecked() && !m_split->isChecked());
        m_imports->setEnabled(m_existing->isChecked());
        m_split->setEnabled(m_new->isChecked());
    };
    connect(m_new, &QRadioButton::toggled, this, sync);
    connect(m_split, &QCheckBox::toggled, this, sync);
    sync();

    auto* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    bb->button(QDialogButtonBox::Ok)->setText(tr("Import"));
    connect(bb, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);
    l->addWidget(bb);
}

ImportOptions ImportDialog::options() const
{
    ImportOptions o;
    o.move = m_move->isChecked();
    if (m_existing->isChecked())
        o.importName = m_imports->currentData().toString();
    else if (m_split->isChecked())
        o.splitByDate = true;
    else
        o.importName = m_name->text().trimmed();
    return o;
}

ExportDialog::ExportDialog(int selected, int inImport, int inBatch, QWidget* parent) : QDialog(parent)
{
    setWindowTitle(tr("Export JPEGs"));
    QSettings s;
    auto* l = new QVBoxLayout(this);

    auto* sg = new QGroupBox(tr("Photos"), this);
    auto* sl = new QVBoxLayout(sg);
    m_scope[0] = new QRadioButton(tr("Selected photos (%1)").arg(selected), sg);
    m_scope[1] = new QRadioButton(tr("Current import (%1)").arg(inImport), sg);
    m_scope[2] = new QRadioButton(tr("Whole batch (%1)").arg(inBatch), sg);
    for (auto* r : m_scope)
        sl->addWidget(r);
    m_scope[0]->setEnabled(selected > 0);
    m_scope[selected > 0 ? 0 : 1]->setChecked(true);
    m_rejected = new QCheckBox(tr("Include rejected photos"), sg);
    sl->addWidget(m_rejected);
    l->addWidget(sg);

    auto* fg = new QGroupBox(tr("Output"), this);
    auto* fl = new QFormLayout(fg);
    auto* folderRow = new QHBoxLayout();
    m_folder = new QLineEdit(s.value("export/folder", QStandardPaths::writableLocation(QStandardPaths::PicturesLocation) + "/Silmä").toString(), fg);
    auto* browse = new QPushButton(tr("Browse..."), fg);
    folderRow->addWidget(m_folder, 1);
    folderRow->addWidget(browse);
    fl->addRow(tr("Folder:"), folderRow);
    connect(browse, &QPushButton::clicked, this, [this] {
        QString d = QFileDialog::getExistingDirectory(this, tr("Export Folder"), m_folder->text());
        if (!d.isEmpty())
            m_folder->setText(d);
    });
    m_pattern = new QLineEdit(s.value("export/pattern", "{date}-{name}").toString(), fg);
    m_pattern->setToolTip(tr("Tokens: {date} (YYYYMMDD), {time} (HHMMSS), {name} (original file name), {import}, {seq}, {camera}"));
    fl->addRow(tr("File name:"), m_pattern);
    m_quality = new QSpinBox(fg);
    m_quality->setRange(1, 100);
    m_quality->setValue(s.value("export/quality", 90).toInt());
    fl->addRow(tr("JPEG quality:"), m_quality);
    auto* sizeRow = new QHBoxLayout();
    m_sizeMode = new QComboBox(fg);
    m_sizeMode->addItems({tr("Original size"), tr("Long edge"), tr("Short edge")});
    m_sizeMode->setCurrentIndex(s.value("export/size_mode", 0).toInt());
    m_size = new QSpinBox(fg);
    m_size->setRange(64, 20000);
    m_size->setSuffix(tr(" px"));
    m_size->setValue(s.value("export/size", 2048).toInt());
    sizeRow->addWidget(m_sizeMode, 1);
    sizeRow->addWidget(m_size);
    fl->addRow(tr("Size:"), sizeRow);
    auto sync = [this] { m_size->setEnabled(m_sizeMode->currentIndex() != 0); };
    connect(m_sizeMode, &QComboBox::currentIndexChanged, this, sync);
    sync();
    l->addWidget(fg);

    auto* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    bb->button(QDialogButtonBox::Ok)->setText(tr("Export"));
    connect(bb, &QDialogButtonBox::accepted, this, [this] {
        QSettings s;
        s.setValue("export/folder", m_folder->text());
        s.setValue("export/pattern", m_pattern->text());
        s.setValue("export/quality", m_quality->value());
        s.setValue("export/size_mode", m_sizeMode->currentIndex());
        s.setValue("export/size", m_size->value());
        accept();
    });
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);
    l->addWidget(bb);
    resize(520, sizeHint().height());
}

ExportOptions ExportDialog::options() const
{
    ExportOptions o;
    o.folder = m_folder->text();
    o.pattern = m_pattern->text().isEmpty() ? QStringLiteral("{date}-{name}") : m_pattern->text();
    o.quality = m_quality->value();
    o.sizeMode = ExportSizeMode(m_sizeMode->currentIndex());
    o.size = m_size->value();
    return o;
}

ExportScope ExportDialog::scope() const
{
    for (int i = 0; i < 3; ++i)
        if (m_scope[i]->isChecked())
            return ExportScope(i);
    return ExportScope::Import;
}

bool ExportDialog::includeRejected() const { return m_rejected->isChecked(); }

} // namespace silma
