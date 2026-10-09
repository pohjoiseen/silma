#pragma once
// Merge to Panorama dialog: options and a quick preview of the stitch (computed in the
// background when the dialog opens), so a failing or poor merge shows before the real one.

#include "Batch.h"
#include "Merge.h"

#include <QDialog>
#include <QImage>
#include <memory>

class QCheckBox;
class QLabel;

namespace silma {

class PanoramaPreviewView;

class PanoramaDialog : public QDialog {
    Q_OBJECT
public:
    PanoramaDialog(const QList<PhotoInfo>& photos, QWidget* parent = nullptr);
    ~PanoramaDialog() override;

    PanoOptions options() const;

    // For tests.
    bool previewDone() const { return m_done; }
    QString previewError() const { return m_error; }
    QImage previewImage() const; // as shown (tone per the option), uncropped
    QRectF previewCrop() const { return m_crop; }

private:
    void progress(const QString& step, int done, int total);
    void finished(const QImage& toned, const QImage& plain, QRectF crop, const QString& error);
    void updateView();

    std::shared_ptr<std::atomic<bool>> m_cancel;
    bool m_done = false;
    QString m_error;
    QImage m_toned, m_plain;
    QRectF m_crop;
    PanoramaPreviewView* m_view;
    QLabel* m_status;
    QCheckBox* m_autoCrop;
    QCheckBox* m_autoTone;
};

} // namespace silma
