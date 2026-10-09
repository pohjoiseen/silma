#pragma once
// Merge to HDR / Panorama dialog: options and a quick preview of the merge (computed in the
// background when the dialog opens, and again when an option changes the merge itself), so a
// failing or poor merge shows before the real one.

#include "Batch.h"
#include "Merge.h"

#include <QDialog>
#include <QImage>
#include <memory>

class QCheckBox;
class QLabel;

namespace silma {

class MergePreviewView;

class MergeDialog : public QDialog {
    Q_OBJECT
public:
    enum Kind { Hdr, Panorama };
    MergeDialog(Kind kind, const QList<MergeSource>& photos, QWidget* parent = nullptr);
    ~MergeDialog() override;

    HdrOptions hdrOptions() const;
    PanoOptions panoOptions() const;

    // For tests.
    bool previewDone() const { return m_done; }
    QString previewError() const { return m_error; }
    QImage previewImage() const; // as shown (tone per the option), uncropped
    QRectF previewCrop() const { return m_crop; }
    QCheckBox* option(const QString& text) const;

private:
    void startPreview();
    void progress(int gen, const QString& step, int done, int total);
    void finished(int gen, const QImage& toned, const QImage& plain, QRectF crop, const QString& error);
    void updateView();

    Kind m_kind;
    QList<MergeSource> m_photos;
    std::shared_ptr<std::atomic<bool>> m_cancel;
    int m_gen = 0;
    bool m_done = false;
    QString m_error;
    QImage m_toned, m_plain;
    QRectF m_crop;
    MergePreviewView* m_view;
    QLabel* m_status;
    QCheckBox* m_autoTone;
    QCheckBox* m_autoCrop = nullptr;           // panorama
    QCheckBox* m_align = nullptr;              // HDR
    QCheckBox* m_deghost = nullptr;            // HDR
};

} // namespace silma
