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
#include <QTreeWidget>
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

    m_new->setChecked(true);
    // Empty name: named after the capture date when importing.
    m_name->setPlaceholderText(tr("Capture date"));
    setDefaultName(defaultName);
    for (QAbstractButton* b : {static_cast<QAbstractButton*>(m_new), static_cast<QAbstractButton*>(m_existing), static_cast<QAbstractButton*>(m_split)})
        connect(b, &QAbstractButton::clicked, this, [this] { m_destinationChosen = true; });
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

void ImportDialog::setDefaultName(const QString& name)
{
    if (name.isEmpty() || m_destinationChosen || m_name->isModified())
        return;
    m_name->setText(name);
    // An import with that name already existing: preselect it.
    int existingIdx = m_imports->findData(name);
    if (existingIdx >= 0) {
        m_existing->setChecked(true);
        m_imports->setCurrentIndex(existingIdx);
    }
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

PasteSettingsDialog::PasteSettingsDialog(int photoCount, QWidget* parent) : QDialog(parent)
{
    setWindowTitle(tr("Paste Settings"));
    auto* l = new QVBoxLayout(this);
    l->addWidget(new QLabel(photoCount == 1 ? tr("Paste these settings:") : tr("Paste these settings to %1 photos:").arg(photoCount), this));
    m_tree = new QTreeWidget(this);
    m_tree->setHeaderHidden(true);
    m_tree->setRootIsDecorated(true);
    const QHash<QString, QString> groupLabels = {
        {"tone", tr("Tone")}, {"presence", tr("Presence")}, {"lens", tr("Lens Corrections")},
        {"transform", tr("Transform")}, {"sharpening", tr("Sharpening")}, {"noise", tr("Noise Reduction")},
    };
    QHash<QString, QTreeWidgetItem*> groups;
    for (const SettingsPart& p : settingsParts()) {
        const QString group = QString::fromLatin1(p.group);
        QTreeWidgetItem* item;
        if (groupLabels.contains(group)) {
            // Sections with several settings: a parent item checking all of them.
            QTreeWidgetItem*& parentItem = groups[group];
            if (!parentItem) {
                parentItem = new QTreeWidgetItem(m_tree, {groupLabels[group]});
                parentItem->setFlags(parentItem->flags() | Qt::ItemIsUserCheckable | Qt::ItemIsAutoTristate);
            }
            item = new QTreeWidgetItem(parentItem, {QString::fromUtf8(p.label)});
        } else {
            item = new QTreeWidgetItem(m_tree, {QString::fromUtf8(p.label)});
        }
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setData(0, Qt::UserRole, QString::fromLatin1(p.key));
        item->setCheckState(0, Qt::Checked);
    }
    l->addWidget(m_tree, 1);

    setParts(savedParts());

    auto* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    bb->button(QDialogButtonBox::Ok)->setText(tr("Paste"));
    QPushButton* all = bb->addButton(tr("Check &All"), QDialogButtonBox::ResetRole);
    QPushButton* none = bb->addButton(tr("Check &None"), QDialogButtonBox::ResetRole);
    auto setAll = [this](Qt::CheckState st) {
        for (int i = 0; i < m_tree->topLevelItemCount(); ++i)
            m_tree->topLevelItem(i)->setCheckState(0, st); // auto-tristate parents set their children
    };
    connect(all, &QPushButton::clicked, this, [setAll] { setAll(Qt::Checked); });
    connect(none, &QPushButton::clicked, this, [setAll] { setAll(Qt::Unchecked); });
    connect(bb, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);
    // Nothing to paste: disable the button.
    connect(m_tree, &QTreeWidget::itemChanged, this, [this, bb] { bb->button(QDialogButtonBox::Ok)->setEnabled(!parts().isEmpty()); });
    l->addWidget(bb);
    resize(std::max(420, sizeHint().width()), 520);
}

QSet<QString> PasteSettingsDialog::savedParts()
{
    // Unchecked parts are remembered, so parts added in later versions start checked.
    // Crop is off by default: it rarely suits other photos.
    const QStringList excluded = QSettings().value("paste/excluded", QStringList{"crop"}).toStringList();
    QSet<QString> parts;
    for (const SettingsPart& p : settingsParts())
        if (!excluded.contains(QString::fromLatin1(p.key)))
            parts.insert(QString::fromLatin1(p.key));
    return parts;
}

QSet<QString> PasteSettingsDialog::parts() const
{
    QSet<QString> out;
    for (QTreeWidgetItemIterator it(m_tree); *it; ++it)
        if ((*it)->childCount() == 0 && (*it)->checkState(0) == Qt::Checked)
            out.insert((*it)->data(0, Qt::UserRole).toString());
    return out;
}

void PasteSettingsDialog::setParts(const QSet<QString>& parts)
{
    for (QTreeWidgetItemIterator it(m_tree); *it; ++it)
        if ((*it)->childCount() == 0)
            (*it)->setCheckState(0, parts.contains((*it)->data(0, Qt::UserRole).toString()) ? Qt::Checked : Qt::Unchecked);
}

void PasteSettingsDialog::accept()
{
    const QSet<QString> chosen = parts();
    QStringList excluded;
    for (const SettingsPart& p : settingsParts())
        if (!chosen.contains(QString::fromLatin1(p.key)))
            excluded << QString::fromLatin1(p.key);
    QSettings().setValue("paste/excluded", excluded);
    QDialog::accept();
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
