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
    // Camera profiles for the photo (empty: only the built-in colour).
    void setProfiles(const QList<ProfileChoice>& choices);
    void setHistogram(const Histogram& h);
    void setPhotoInfo(const QString& text);
    void setCropToolActive(bool on);
    void setHealToolActive(bool on);
    void setWbPickerActive(bool on);
    double healSize() const;
    // Adds a widget into a named section (used by tools added later).
    CollapsibleSection* section(const QString& id) const { return m_sections.value(id); }

signals:
    // `mergeKey` (the parameter) lets quick repeated edits of one slider make one history step.
    void edited(const DevelopSettings& s, const QString& label, bool final, const QString& mergeKey);
    void autoToneRequested();
    void installProfilesRequested();
    void autoUprightRequested(int mode); // 0 vertical, 1 horizontal, 2 both
    void transformResetRequested();
    void turnRequested(int quarterTurnsCw); // 1 clockwise, -1 counter-clockwise
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
    void emitEdit(const QString& label, bool final, const QString& mergeKey = {});

    DevelopSettings m_settings;
    WbValues m_wb;
    HistogramWidget* m_histogram;
    QHash<QString, CollapsibleSection*> m_sections;
    QHash<QString, ParamSlider*> m_sliders;
    QComboBox* m_wbMode;
    QComboBox* m_profile;
    QList<ProfileChoice> m_profiles;
    void fillProfiles();
    QComboBox* m_curveChannel;
    CurveWidget* m_curve;
    QLabel* m_lensInfo;
    QToolButton* m_cropTool;
    QCheckBox* m_cropLock;
    QCheckBox* m_constrainCrop;
    QToolButton* m_healTool;
    QToolButton* m_wbPicker;
    QSlider* m_healSize;
    QLabel* m_healSizeLabel;
    bool m_updating = false;
};

} // namespace silma
