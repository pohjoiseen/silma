#include "DevelopPanel.h"

#include "Widgets.h"

#include <QCheckBox>
#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QScrollArea>
#include <QSlider>
#include <QSettings>
#include <QToolButton>
#include <QVBoxLayout>

namespace silma {

namespace {

// Compact text button (push buttons in most styles are wide and tall).
QToolButton* smallButton(const QString& text, QWidget* parent)
{
    auto* b = new QToolButton(parent);
    b->setText(text);
    b->setToolButtonStyle(Qt::ToolButtonTextOnly);
    return b;
}

// Buttons sharing a row split its width evenly.
QToolButton* rowButton(const QString& text, QWidget* parent)
{
    auto* b = smallButton(text, parent);
    b->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    return b;
}

// Secondary text that follows the color scheme.
void makeDim(QLabel* l) { l->setForegroundRole(QPalette::PlaceholderText); }

} // namespace

DevelopPanel::DevelopPanel(QWidget* parent) : QWidget(parent)
{
    // Slightly smaller text than the rest of the UI: the panel holds many controls.
    QFont f = font();
    f.setPointSizeF(f.pointSizeF() * 0.9);
    setFont(f);
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(4, 4, 0, 4);
    outer->setSpacing(4);
    m_histogram = new HistogramWidget(this);
    outer->addWidget(m_histogram);

    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto* content = new QWidget(scroll);
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(0, 0, 4, 0);
    layout->setSpacing(0);
    scroll->setWidget(content);
    outer->addWidget(scroll, 1);

    // White balance
    auto* wb = addSection(layout, "wb", tr("White Balance"), true);
    {
        auto* row = new QHBoxLayout();
        row->addWidget(new QLabel(tr("WB:"), wb));
        m_wbMode = new QComboBox(wb);
        m_wbMode->addItem(tr("As Shot"), int(WbMode::AsShot));
        m_wbMode->addItem(tr("Auto"), int(WbMode::Auto));
        m_wbMode->addItem(tr("Custom"), int(WbMode::Custom));
        row->addWidget(m_wbMode, 1);
        m_wbPicker = new QToolButton(wb);
        m_wbPicker->setCheckable(true);
        if (QIcon::hasThemeIcon(QStringLiteral("color-picker")))
            m_wbPicker->setIcon(QIcon::fromTheme(QStringLiteral("color-picker")));
        else
            m_wbPicker->setText(tr("Pick"));
        m_wbPicker->setToolTip(tr("White balance picker (W): click a point in the photo that should be neutral grey"));
        connect(m_wbPicker, &QToolButton::toggled, this, [this](bool on) {
            if (!m_updating)
                emit wbPickerToggled(on);
        });
        row->addWidget(m_wbPicker);
        wb->contentLayout()->addLayout(row);
        connect(m_wbMode, &QComboBox::currentIndexChanged, this, [this] {
            if (m_updating)
                return;
            auto mode = WbMode(m_wbMode->currentData().toInt());
            if (mode == WbMode::Custom) {
                m_settings.temperature = std::round(m_wb.temperature);
                m_settings.tint = std::round(m_wb.tint);
            }
            m_settings.wbMode = mode;
            emitEdit(tr("White Balance: %1").arg(m_wbMode->currentText()), true);
        });
    }
    addSliders(wb, "wb");

    // Tone
    auto* tone = addSection(layout, "tone", tr("Tone"), true);
    auto* autoTone = smallButton(tr("Auto"), tone);
    autoTone->setToolTip(tr("Automatic tone (Ctrl+U)"));
    tone->addHeaderWidget(autoTone);
    connect(autoTone, &QToolButton::clicked, this, &DevelopPanel::autoToneRequested);
    addSliders(tone, "tone");

    auto* presence = addSection(layout, "presence", tr("Presence"), true);
    addSliders(presence, "presence");

    // Tone curve
    auto* curve = addSection(layout, "curve", tr("Tone Curve"), false);
    {
        auto* row = new QHBoxLayout();
        m_curveChannel = new QComboBox(curve);
        m_curveChannel->addItems({tr("RGB"), tr("Red"), tr("Green"), tr("Blue")});
        row->addWidget(m_curveChannel, 1);
        auto* reset = smallButton(tr("Reset"), curve);
        row->addWidget(reset);
        curve->contentLayout()->addLayout(row);
        m_curve = new CurveWidget(curve);
        curve->contentLayout()->addWidget(m_curve);
        connect(m_curveChannel, &QComboBox::currentIndexChanged, m_curve, &CurveWidget::setChannel);
        connect(m_curve, &CurveWidget::curveEdited, this, [this](int ch, const CurvePoints& c, bool final) {
            CurvePoints* t[4] = {&m_settings.curveRgb, &m_settings.curveR, &m_settings.curveG, &m_settings.curveB};
            *t[ch] = c;
            emitEdit(tr("Tone Curve"), final);
        });
        connect(reset, &QToolButton::clicked, this, [this] {
            CurvePoints* t[4] = {&m_settings.curveRgb, &m_settings.curveR, &m_settings.curveG, &m_settings.curveB};
            t[m_curveChannel->currentIndex()]->points.clear();
            m_curve->setSettings(m_settings);
            emitEdit(tr("Reset Tone Curve"), true);
        });
    }

    // Lens
    auto* lens = addSection(layout, "lens", tr("Lens Corrections"), false);
    m_lensInfo = new QLabel(lens);
    m_lensInfo->setWordWrap(true);
    makeDim(m_lensInfo);
    lens->contentLayout()->addWidget(m_lensInfo);
    addSliders(lens, "lens");

    // Transform
    auto* transform = addSection(layout, "transform", tr("Transform"), false);
    {
        auto* row = new QHBoxLayout();
        row->addWidget(new QLabel(tr("Upright:"), transform));
        const QStringList names = {tr("Vertical"), tr("Horizontal"), tr("Both")};
        for (int i = 0; i < 3; ++i) {
            auto* b = rowButton(names[i], transform);
            b->setToolTip(tr("Automatically straighten using detected lines"));
            connect(b, &QToolButton::clicked, this, [this, i] { emit autoUprightRequested(i); });
            row->addWidget(b);
        }
        auto* reset = rowButton(tr("Reset"), transform);
        reset->setToolTip(tr("Reset Vertical, Horizontal, Rotate, Scale and the crop"));
        connect(reset, &QToolButton::clicked, this, &DevelopPanel::transformResetRequested);
        row->addWidget(reset);
        transform->contentLayout()->addLayout(row);

        auto* turns = new QHBoxLayout();
        turns->addWidget(new QLabel(tr("Rotate 90°:"), transform));
        auto* ccw = rowButton(tr("⟲ Left"), transform);
        ccw->setToolTip(tr("Rotate 90° counter-clockwise (Ctrl+[)"));
        connect(ccw, &QToolButton::clicked, this, [this] { emit turnRequested(-1); });
        turns->addWidget(ccw);
        auto* cw = rowButton(tr("⟳ Right"), transform);
        cw->setToolTip(tr("Rotate 90° clockwise (Ctrl+])"));
        connect(cw, &QToolButton::clicked, this, [this] { emit turnRequested(1); });
        turns->addWidget(cw);
        transform->contentLayout()->addLayout(turns);
    }
    addSliders(transform, "transform");
    m_constrainCrop = new QCheckBox(tr("Constrain crop"), transform);
    m_constrainCrop->setToolTip(tr("Crop away the empty borders that transforms and lens corrections bring in"));
    transform->contentLayout()->addWidget(m_constrainCrop);
    connect(m_constrainCrop, &QCheckBox::toggled, this, [this](bool on) {
        if (m_updating)
            return;
        m_settings.constrainCrop = on;
        emitEdit(on ? tr("Constrain Crop") : tr("Don't Constrain Crop"), true);
    });

    // Crop
    auto* crop = addSection(layout, "crop", tr("Crop"), false);
    {
        auto* row = new QHBoxLayout();
        m_cropTool = rowButton(tr("Crop Tool"), crop);
        m_cropTool->setCheckable(true);
        m_cropTool->setToolTip(tr("Edit the crop on the image (R)"));
        row->addWidget(m_cropTool);
        auto* autoCrop = rowButton(tr("Auto"), crop);
        autoCrop->setToolTip(tr("Largest crop without empty borders"));
        row->addWidget(autoCrop);
        auto* reset = rowButton(tr("Reset"), crop);
        row->addWidget(reset);
        crop->contentLayout()->addLayout(row);
        m_cropLock = new QCheckBox(tr("Keep aspect ratio"), crop);
        crop->contentLayout()->addWidget(m_cropLock);
        connect(m_cropTool, &QToolButton::toggled, this, [this](bool on) {
            if (!m_updating)
                emit cropToolToggled(on);
        });
        connect(autoCrop, &QToolButton::clicked, this, &DevelopPanel::autoCropRequested);
        connect(reset, &QToolButton::clicked, this, [this] {
            m_settings.crop = QRectF(0, 0, 1, 1);
            emitEdit(tr("Reset Crop"), true);
        });
        connect(m_cropLock, &QCheckBox::toggled, this, [this](bool on) {
            if (m_updating)
                return;
            m_settings.cropLockAspect = on;
            emitEdit(on ? tr("Lock Crop Aspect") : tr("Unlock Crop Aspect"), true);
        });
    }

    // Heal
    auto* heal = addSection(layout, "heal", tr("AI Heal"), false);
    {
        auto* row = new QHBoxLayout();
        m_healTool = rowButton(tr("AI Heal Tool"), heal);
        m_healTool->setCheckable(true);
        m_healTool->setToolTip(tr("Click, or drag along it, to remove something with generative fill (Q). Alt+click removes a heal."));
        row->addWidget(m_healTool);
        auto* clear = rowButton(tr("Clear All"), heal);
        row->addWidget(clear);
        heal->contentLayout()->addLayout(row);
        auto* sizeRow = new QHBoxLayout();
        m_healSizeLabel = new QLabel(tr("Size"), heal);
        sizeRow->addWidget(m_healSizeLabel);
        m_healSize = new QSlider(Qt::Horizontal, heal);
        makeSliderAbsolute(m_healSize);
        m_healSize->setRange(5, 100); // brush radius in per-mille of the long edge (heals are capped at 20%)
        m_healSize->setValue(QSettings().value("heal/size", 30).toInt());
        sizeRow->addWidget(m_healSize, 1);
        heal->contentLayout()->addLayout(sizeRow);
        // The brush size only matters while the tool is on.
        m_healSize->setEnabled(false);
        m_healSizeLabel->setEnabled(false);
        connect(m_healTool, &QToolButton::toggled, this, [this](bool on) {
            if (!m_updating)
                emit healToolToggled(on);
        });
        connect(m_healSize, &QSlider::valueChanged, this, [this](int v) {
            QSettings().setValue("heal/size", v);
            emit healSizeChanged(v / 1000.0);
        });
        connect(clear, &QToolButton::clicked, this, &DevelopPanel::healClearRequested);
    }

    auto* detail = addSection(layout, "detail", tr("Detail"), false);
    addSliders(detail, "detail");
    auto* ai = smallButton(tr("AI Denoise..."), detail);
    ai->setToolTip(tr("Denoise with a neural network into a new DNG photo"));
    detail->contentLayout()->addWidget(ai);
    connect(ai, &QToolButton::clicked, this, &DevelopPanel::aiDenoiseRequested);

    layout->addStretch(1);
    setMinimumWidth(240);
}

CollapsibleSection* DevelopPanel::addSection(QVBoxLayout* layout, const QString& id, const QString& title, bool expanded)
{
    auto* s = new CollapsibleSection(title, this);
    QSettings qs;
    s->setExpanded(qs.value("panel/" + id, expanded).toBool());
    connect(s, &CollapsibleSection::toggled, this, [id](bool on) { QSettings().setValue("panel/" + id, on); });
    layout->addWidget(s);
    m_sections.insert(id, s);
    return s;
}

void DevelopPanel::addSliders(CollapsibleSection* sec, const char* group)
{
    for (const auto& def : paramDefs()) {
        if (QLatin1String(def.group) != QLatin1String(group))
            continue;
        auto* sl = new ParamSlider(def, sec);
        sec->contentLayout()->addWidget(sl);
        // Small gaps between groups of related sliders.
        static const QStringList gapAfter = {"contrast", "dehaze"};
        if (gapAfter.contains(QLatin1String(def.key)))
            sec->contentLayout()->addSpacing(fontMetrics().height() / 2);
        m_sliders.insert(QString::fromUtf8(def.key), sl);
        QString key = QString::fromUtf8(def.key);
        connect(sl, &ParamSlider::activeChanged, this, [this, key](bool on) { emit sliderActive(key, on); });
        connect(sl, &ParamSlider::edited, this, [this, sl, key](double v, bool final) {
            const ParamDef& d = sl->def();
            m_settings.*d.field = v;
            if (key == QLatin1String("temperature") || key == QLatin1String("tint")) {
                if (m_settings.wbMode != WbMode::Custom) {
                    // Switching to custom: take the other value from the effective WB.
                    if (key == QLatin1String("temperature"))
                        m_settings.tint = std::round(m_wb.tint);
                    else
                        m_settings.temperature = std::round(m_wb.temperature);
                    m_settings.wbMode = WbMode::Custom;
                    m_updating = true;
                    m_wbMode->setCurrentIndex(2);
                    m_updating = false;
                }
            }
            QString val = QString::number(v, 'f', d.decimals);
            if (d.min < 0 && v > 0)
                val.prepend('+');
            emitEdit(QStringLiteral("%1 %2").arg(QString::fromUtf8(d.label), val), final, key);
        });
    }
}

void DevelopPanel::emitEdit(const QString& label, bool final, const QString& mergeKey)
{
    if (m_updating)
        return;
    emit edited(m_settings, label, final, mergeKey);
}

void DevelopPanel::setDefaults(const DevelopSettings& d)
{
    for (ParamSlider* s : std::as_const(m_sliders))
        s->setResetValue(d.*(s->def().field));
}

void DevelopPanel::setSettings(const DevelopSettings& s)
{
    m_updating = true;
    m_settings = s;
    for (auto it = m_sliders.begin(); it != m_sliders.end(); ++it) {
        const ParamDef& d = it.value()->def();
        if (it.key() == QLatin1String("temperature") || it.key() == QLatin1String("tint"))
            continue; // shown from the effective WB
        it.value()->setValue(s.*d.field);
    }
    m_wbMode->setCurrentIndex(int(s.wbMode));
    if (s.wbMode == WbMode::Custom) {
        m_sliders["temperature"]->setValue(s.temperature);
        m_sliders["tint"]->setValue(s.tint);
    }
    m_curve->setSettings(s);
    m_cropLock->setChecked(s.cropLockAspect);
    m_constrainCrop->setChecked(s.constrainCrop);
    m_updating = false;
}

void DevelopPanel::setEffectiveWb(const WbValues& wb)
{
    m_wb = wb;
    if (m_settings.wbMode == WbMode::Custom)
        return;
    m_updating = true;
    m_sliders["temperature"]->setValue(std::round(wb.temperature / 50) * 50);
    m_sliders["tint"]->setValue(std::round(wb.tint));
    m_updating = false;
}

void DevelopPanel::setLensInfo(const QString& text)
{
    m_lensInfo->setText(text.isEmpty() ? tr("No lens profile found") : text);
}

void DevelopPanel::setHistogram(const Histogram& h) { m_histogram->setHistogram(h); }
void DevelopPanel::setPhotoInfo(const QString& text) { m_histogram->setInfo(text); }

void DevelopPanel::setHealToolActive(bool on)
{
    m_updating = true;
    m_healTool->setChecked(on);
    m_healSize->setEnabled(on);
    m_healSizeLabel->setEnabled(on);
    m_updating = false;
}

void DevelopPanel::setWbPickerActive(bool on)
{
    m_updating = true;
    m_wbPicker->setChecked(on);
    m_updating = false;
}

double DevelopPanel::healSize() const { return m_healSize->value() / 1000.0; }

void DevelopPanel::setCropToolActive(bool on)
{
    m_updating = true;
    m_cropTool->setChecked(on);
    m_updating = false;
}

} // namespace silma
