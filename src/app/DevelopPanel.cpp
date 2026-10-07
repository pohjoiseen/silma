#include "DevelopPanel.h"

#include "Widgets.h"

#include <QCheckBox>
#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QSettings>
#include <QVBoxLayout>

namespace silma {

DevelopPanel::DevelopPanel(QWidget* parent) : QWidget(parent)
{
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(4, 4, 4, 4);
    m_histogram = new HistogramWidget(this);
    outer->addWidget(m_histogram);

    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto* content = new QWidget(scroll);
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(0, 0, 6, 0);
    layout->setSpacing(2);
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
    auto* autoTone = new QPushButton(tr("Auto"), tone);
    autoTone->setToolTip(tr("Automatic tone (Ctrl+U)"));
    tone->addHeaderWidget(autoTone);
    connect(autoTone, &QPushButton::clicked, this, &DevelopPanel::autoToneRequested);
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
        auto* reset = new QPushButton(tr("Reset"), curve);
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
        connect(reset, &QPushButton::clicked, this, [this] {
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
    m_lensInfo->setStyleSheet(QStringLiteral("color: palette(mid);"));
    lens->contentLayout()->addWidget(m_lensInfo);
    addSliders(lens, "lens");

    // Transform
    auto* transform = addSection(layout, "transform", tr("Transform"), false);
    {
        auto* row = new QHBoxLayout();
        row->addWidget(new QLabel(tr("Upright:"), transform));
        const QStringList names = {tr("Vertical"), tr("Horizontal"), tr("Both")};
        for (int i = 0; i < 3; ++i) {
            auto* b = new QPushButton(names[i], transform);
            b->setToolTip(tr("Automatically straighten using detected lines"));
            connect(b, &QPushButton::clicked, this, [this, i] { emit autoUprightRequested(i); });
            row->addWidget(b);
        }
        transform->contentLayout()->addLayout(row);
    }
    addSliders(transform, "transform");

    // Crop
    auto* crop = addSection(layout, "crop", tr("Crop"), false);
    {
        auto* row = new QHBoxLayout();
        m_cropTool = new QPushButton(tr("Crop Tool"), crop);
        m_cropTool->setCheckable(true);
        m_cropTool->setToolTip(tr("Edit the crop on the image (R)"));
        row->addWidget(m_cropTool);
        auto* autoCrop = new QPushButton(tr("Auto"), crop);
        autoCrop->setToolTip(tr("Largest crop without empty borders"));
        row->addWidget(autoCrop);
        auto* reset = new QPushButton(tr("Reset"), crop);
        row->addWidget(reset);
        crop->contentLayout()->addLayout(row);
        m_cropLock = new QCheckBox(tr("Keep aspect ratio"), crop);
        crop->contentLayout()->addWidget(m_cropLock);
        connect(m_cropTool, &QPushButton::toggled, this, [this](bool on) {
            if (!m_updating)
                emit cropToolToggled(on);
        });
        connect(autoCrop, &QPushButton::clicked, this, &DevelopPanel::autoCropRequested);
        connect(reset, &QPushButton::clicked, this, [this] {
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

    auto* detail = addSection(layout, "detail", tr("Detail"), false);
    addSliders(detail, "detail");

    layout->addStretch(1);
    setMinimumWidth(280);
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
        m_sliders.insert(QString::fromUtf8(def.key), sl);
        QString key = QString::fromUtf8(def.key);
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
            emit sliderActive(key, !final);
            emitEdit(QStringLiteral("%1 %2").arg(QString::fromUtf8(d.label), val), final);
        });
    }
}

void DevelopPanel::emitEdit(const QString& label, bool final)
{
    if (m_updating)
        return;
    emit edited(m_settings, label, final);
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

void DevelopPanel::setCropToolActive(bool on)
{
    m_updating = true;
    m_cropTool->setChecked(on);
    m_updating = false;
}

} // namespace silma
