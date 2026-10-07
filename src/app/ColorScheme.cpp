#include "ColorScheme.h"

#include <QActionGroup>
#include <QApplication>
#include <QPalette>
#include <QSettings>
#include <QStyle>
#include <QStyleFactory>

#ifdef SILMA_HAVE_KCOLORSCHEME
#include <KColorSchemeManager>
#include <KColorSchemeModel>
#endif

namespace silma {

namespace {

constexpr auto kSettingsKey = "ui/colorScheme";
constexpr auto kDefaultScheme = "BreezeDark";
// Fallback id when the KDE framework is missing.
constexpr auto kBuiltinDark = "BuiltinDark";

QString savedScheme() { return QSettings().value(kSettingsKey, QString::fromLatin1(kDefaultScheme)).toString(); }

// Breeze Dark colors, for systems without the KDE color scheme framework.
QPalette builtinDarkPalette()
{
    const QColor window(32, 35, 38), view(20, 22, 24), viewAlt(29, 31, 34), button(41, 44, 48), text(252, 252, 252),
        inactiveText(161, 169, 177), highlight(61, 174, 233), link(29, 153, 243), visited(155, 89, 182);
    QPalette p;
    p.setColor(QPalette::Window, window);
    p.setColor(QPalette::WindowText, text);
    p.setColor(QPalette::Base, view);
    p.setColor(QPalette::AlternateBase, viewAlt);
    p.setColor(QPalette::Text, text);
    p.setColor(QPalette::PlaceholderText, inactiveText);
    p.setColor(QPalette::Button, button);
    p.setColor(QPalette::ButtonText, text);
    p.setColor(QPalette::BrightText, Qt::white);
    p.setColor(QPalette::Highlight, highlight);
    p.setColor(QPalette::HighlightedText, text);
    p.setColor(QPalette::ToolTipBase, button);
    p.setColor(QPalette::ToolTipText, text);
    p.setColor(QPalette::Link, link);
    p.setColor(QPalette::LinkVisited, visited);
    p.setColor(QPalette::Light, button.lighter(150));
    p.setColor(QPalette::Midlight, button.lighter(125));
    p.setColor(QPalette::Mid, button.darker(130));
    p.setColor(QPalette::Dark, button.darker(160));
    p.setColor(QPalette::Shadow, QColor(0, 0, 0));
    for (auto role : {QPalette::WindowText, QPalette::Text, QPalette::ButtonText})
        p.setColor(QPalette::Disabled, role, QColor(110, 113, 117));
    return p;
}

#ifdef SILMA_HAVE_KCOLORSCHEME
KColorSchemeManager* manager()
{
    auto* m = KColorSchemeManager::instance();
    m->setAutosaveChanges(false); // remembered in our own settings
    return m;
}
#endif

void activate(const QString& id)
{
#ifdef SILMA_HAVE_KCOLORSCHEME
    auto* m = manager();
    if (id.isEmpty() || m->indexForSchemeId(id).isValid()) {
        m->activateSchemeId(id);
        return;
    }
    // The saved scheme is gone (e.g. uninstalled): fall back to the default.
    if (m->indexForSchemeId(QString::fromLatin1(kDefaultScheme)).isValid()) {
        m->activateSchemeId(QString::fromLatin1(kDefaultScheme));
        return;
    }
#endif
    static const QPalette systemPalette = QApplication::palette();
    static const QString systemStyle = QApplication::style()->name();
    if (id.isEmpty()) {
        QApplication::setStyle(QStyleFactory::create(systemStyle));
        QApplication::setPalette(systemPalette);
    } else {
        // Styles that ignore the palette (e.g. GTK-themed) would stay light.
        QApplication::setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
        QApplication::setPalette(builtinDarkPalette());
    }
}

} // namespace

void applyColorScheme() { activate(savedScheme()); }

QMenu* createColorSchemeMenu(QWidget* parent)
{
    auto* menu = new QMenu(QObject::tr("Color &Scheme"), parent);
    auto* group = new QActionGroup(menu);
    const QString current = savedScheme();
    auto add = [&](const QString& name, const QString& id, const QIcon& icon = {}) {
        QAction* a = menu->addAction(icon, name);
        a->setCheckable(true);
        a->setChecked(id == current);
        group->addAction(a);
        QObject::connect(a, &QAction::triggered, menu, [id] {
            QSettings().setValue(kSettingsKey, id);
            activate(id);
        });
    };
    add(QObject::tr("System Default"), QString());
    menu->addSeparator();
#ifdef SILMA_HAVE_KCOLORSCHEME
    QAbstractItemModel* model = manager()->model();
    for (int r = 0; r < model->rowCount(); ++r) {
        QModelIndex i = model->index(r, 0);
        QString id = i.data(KColorSchemeModel::IdRole).toString();
        if (!id.isEmpty())
            add(i.data(KColorSchemeModel::NameRole).toString(), id, i.data(KColorSchemeModel::IconRole).value<QIcon>());
    }
#else
    add(QObject::tr("Dark"), QString::fromLatin1(kBuiltinDark));
#endif
    return menu;
}

} // namespace silma
