#include "PanoramaDialog.h"

#include <QApplication>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QPushButton>
#include <QVBoxLayout>
#include <QtConcurrent>

namespace silma {

namespace {

constexpr int kViewWidth = 760, kViewHeight = 380;
constexpr int kRenderEdge = 1600; // long edge of the preview renders (device pixels)

} // namespace

// The stitched preview fitted into the view, the area outside the auto crop dimmed; or a
// message while stitching / when it failed.
class PanoramaPreviewView : public QWidget {
public:
    explicit PanoramaPreviewView(QWidget* parent) : QWidget(parent) { setFixedSize(kViewWidth, kViewHeight); }
    void set(const QImage& img, const QRectF& crop, const QString& message)
    {
        m_image = img;
        m_crop = crop;
        m_message = message;
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.fillRect(rect(), QColor(40, 40, 40));
        if (!m_image.isNull()) {
            p.setRenderHint(QPainter::SmoothPixmapTransform);
            const QSizeF s = QSizeF(m_image.size()).scaled(size(), Qt::KeepAspectRatio);
            const QRectF r(QPointF((width() - s.width()) / 2, (height() - s.height()) / 2), s);
            p.drawImage(r, m_image);
            if (m_crop.isValid() && m_crop != QRectF(0, 0, 1, 1)) {
                const QRectF c(r.x() + m_crop.x() * r.width(), r.y() + m_crop.y() * r.height(), m_crop.width() * r.width(), m_crop.height() * r.height());
                QPainterPath outside;
                outside.addRect(r);
                outside.addRect(c);
                p.fillPath(outside, QColor(0, 0, 0, 150));
                p.setPen(QPen(QColor(255, 255, 255, 180), 1));
                p.drawRect(c);
            }
        }
        if (!m_message.isEmpty()) {
            p.setPen(Qt::white);
            p.drawText(rect().adjusted(20, 20, -20, -20), Qt::AlignCenter | Qt::TextWordWrap, m_message);
        }
    }

private:
    QImage m_image;
    QRectF m_crop;
    QString m_message;
};

PanoramaDialog::PanoramaDialog(const QList<PhotoInfo>& photos, QWidget* parent)
    : QDialog(parent), m_cancel(std::make_shared<std::atomic<bool>>(false))
{
    setWindowTitle(tr("Merge to Panorama"));
    auto* l = new QVBoxLayout(this);
    l->setSizeConstraint(QLayout::SetFixedSize);
    l->addWidget(new QLabel(tr("Stitch %n photo(s) into a cylindrical panorama DNG.", nullptr, int(photos.size())), this));
    m_view = new PanoramaPreviewView(this);
    l->addWidget(m_view);
    m_status = new QLabel(this);
    m_status->setWordWrap(true);
    m_status->setFixedWidth(kViewWidth);
    l->addWidget(m_status);
    m_autoCrop = new QCheckBox(tr("Auto crop"), this);
    m_autoTone = new QCheckBox(tr("Auto tone"), this);
    for (auto* c : {m_autoCrop, m_autoTone}) {
        c->setChecked(true);
        l->addWidget(c);
        connect(c, &QCheckBox::toggled, this, &PanoramaDialog::updateView);
    }
    auto* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    bb->button(QDialogButtonBox::Ok)->setText(tr("Merge"));
    connect(bb, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);
    l->addWidget(bb);
    updateView();

    QPointer<PanoramaDialog> self(this);
    auto cancel = m_cancel;
    (void)QtConcurrent::run([photos, self, cancel] {
        // Posts to the dialog on the GUI thread if it still exists.
        auto post = [self](auto f) {
            QMetaObject::invokeMethod(qApp, [self, f]() {
                if (self)
                    f(self.data());
            }, Qt::QueuedConnection);
        };
        QImage toned, plain;
        QRectF crop;
        QString error;
        try {
            PanoramaPreview preview(photos, [&](const QString& step, int d, int t) {
                post([step, d, t](PanoramaDialog* dlg) { dlg->progress(step, d, t); });
            }, cancel.get());
            toned = preview.render(true, kRenderEdge);
            plain = preview.render(false, kRenderEdge);
            crop = preview.autoCrop();
        } catch (const std::exception& e) {
            if (*cancel)
                return;
            error = QString::fromUtf8(e.what());
        }
        post([toned, plain, crop, error](PanoramaDialog* dlg) { dlg->finished(toned, plain, crop, error); });
    });
}

PanoramaDialog::~PanoramaDialog() { *m_cancel = true; }

PanoOptions PanoramaDialog::options() const
{
    PanoOptions o;
    o.autoCrop = m_autoCrop->isChecked();
    o.autoTone = m_autoTone->isChecked();
    return o;
}

QImage PanoramaDialog::previewImage() const { return m_autoTone->isChecked() ? m_toned : m_plain; }

void PanoramaDialog::progress(const QString& step, int done, int total)
{
    if (m_done)
        return;
    m_view->set({}, {}, total > 0 ? tr("Stitching a preview…\n%1 %2/%3").arg(step).arg(done + 1).arg(total) : tr("Stitching a preview…\n%1").arg(step));
}

void PanoramaDialog::finished(const QImage& toned, const QImage& plain, QRectF crop, const QString& error)
{
    m_done = true;
    m_toned = toned;
    m_plain = plain;
    m_crop = crop;
    m_error = error;
    updateView();
}

void PanoramaDialog::updateView()
{
    if (!m_done) {
        m_view->set({}, {}, tr("Stitching a preview…"));
        m_status->setText(tr("The preview is a quick, smaller stitch; the merge itself works at full resolution."));
        return;
    }
    if (!m_error.isEmpty()) {
        m_view->set({}, {}, tr("The preview could not be stitched:\n%1").arg(m_error));
        m_status->setText(tr("The merge will most likely fail the same way. Check that neighbouring photos overlap (by about a third)."));
        return;
    }
    m_view->set(previewImage(), m_autoCrop->isChecked() ? m_crop : QRectF(), QString());
    m_status->setText(tr("Preview: a quick, smaller stitch; the merge itself works at full resolution.%1")
                          .arg(m_autoCrop->isChecked() ? tr(" The dimmed part is cropped away (the DNG keeps it).") : QString()));
}

} // namespace silma
