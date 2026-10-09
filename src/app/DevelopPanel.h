#pragma once

#include "Develop.h"
#include "Engine.h"

#include <QHash>
#include <QWidget>

class QComboBox;
class QLabel;
class QToolButton;
class QCheckBox;
class QSlider;
class QVBoxLayout;

namespace silma {

class CollapsibleSection;
class CurveWidget;
class HistogramWidget;
class ParamSlider;

class DevelopPanel : public QWidget {
    Q_OBJECT
public:
    explicit DevelopPanel(QWidget* parent = nullptr);

    // Updates all controls (no signals emitted).
    void setSettings(const DevelopSettings& s);
    // Values the controls reset to (they differ for JPEG/HEIC photos).
    void setDefaults(const DevelopSettings& d);
    void setEffectiveWb(const WbValues& wb);
    void setLensInfo(const QString& text);
    void setHistogram(const Histogram& h);
    void setPhotoInfo(const QString& text);
    void setCropToolActive(bool on);
    void setHealToolActive(bool on);
    void setWbPickerActive(bool on);
    double healSize() const;
    // Adds a widget into a named section (used by tools added later).
    CollapsibleSection* section(const QString& id) const { return m_sections.value(id); }

signals:
    void edited(const DevelopSettings& s, const QString& label, bool final);
    void autoToneRequested();
    void autoUprightRequested(int mode); // 0 vertical, 1 horizontal, 2 both
    void autoCropRequested();
    void cropToolToggled(bool on);
    void aiDenoiseRequested();
    void healToolToggled(bool on);
    void wbPickerToggled(bool on);
    void healSizeChanged(double radius);
    void healClearRequested();
    void sliderActive(const QString& key, bool active); // for transform guide lines

private:
    CollapsibleSection* addSection(QVBoxLayout* layout, const QString& id, const QString& title, bool expanded);
    void addSliders(CollapsibleSection* sec, const char* group);
    void emitEdit(const QString& label, bool final);

    DevelopSettings m_settings;
    WbValues m_wb;
    HistogramWidget* m_histogram;
    QHash<QString, CollapsibleSection*> m_sections;
    QHash<QString, ParamSlider*> m_sliders;
    QComboBox* m_wbMode;
    QComboBox* m_curveChannel;
    CurveWidget* m_curve;
    QLabel* m_lensInfo;
    QToolButton* m_cropTool;
    QCheckBox* m_cropLock;
    QToolButton* m_healTool;
    QToolButton* m_wbPicker;
    QSlider* m_healSize;
    QLabel* m_healSizeLabel;
    bool m_updating = false;
};

} // namespace silma
