#include "MergeDialog.h"

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

// The preview fitted into the view, the area outside the auto crop dimmed; or a message
// while merging / when it failed.
class MergePreviewView : public QWidget {
public:
    explicit MergePreviewView(QWidget* parent) : QWidget(parent) { setFixedSize(kViewWidth, kViewHeight); }
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
            // Over an (older) image, on a dark plate.
            QRect box = p.fontMetrics().boundingRect(rect().adjusted(40, 40, -40, -40), Qt::AlignCenter | Qt::TextWordWrap, m_message).adjusted(-12, -8, 12, 8);
            if (!m_image.isNull())
                p.fillRect(box, QColor(0, 0, 0, 170));
            p.setPen(Qt::white);
            p.drawText(box, Qt::AlignCenter | Qt::TextWordWrap, m_message);
        }
    }

private:
    QImage m_image;
    QRectF m_crop;
    QString m_message;
};

MergeDialog::MergeDialog(Kind kind, const QList<MergeSource>& photos, QWidget* parent)
    : QDialog(parent), m_kind(kind), m_photos(photos), m_cancel(std::make_shared<std::atomic<bool>>(false))
{
    const int n = int(photos.size());
    setWindowTitle(kind == Hdr ? tr("Merge to HDR") : tr("Merge to Panorama"));
    auto* l = new QVBoxLayout(this);
    l->setSizeConstraint(QLayout::SetFixedSize);
    l->addWidget(new QLabel(kind == Hdr ? tr("Merge %n bracketed exposure(s) into an HDR DNG.", nullptr, n)
                                        : tr("Stitch %n photo(s) into a cylindrical panorama DNG.", nullptr, n), this));
    m_view = new MergePreviewView(this);
    l->addWidget(m_view);
    m_status = new QLabel(this);
    m_status->setWordWrap(true);
    m_status->setFixedWidth(kViewWidth);
    l->addWidget(m_status);
    QList<QCheckBox*> boxes;
    if (kind == Hdr) {
        boxes << (m_align = new QCheckBox(tr("Align photos"), this)) << (m_deghost = new QCheckBox(tr("Remove ghosts (moving objects)"), this));
    } else {
        boxes << (m_autoCrop = new QCheckBox(tr("Auto crop"), this));
    }
    boxes << (m_autoTone = new QCheckBox(tr("Auto tone"), this));
    for (auto* c : boxes) {
        c->setChecked(true);
        l->addWidget(c);
    }
    // Tone and crop only change how the preview is shown; alignment and deghosting change the
    // merge, so it runs again.
    connect(m_autoTone, &QCheckBox::toggled, this, &MergeDialog::updateView);
    if (m_autoCrop)
        connect(m_autoCrop, &QCheckBox::toggled, this, &MergeDialog::updateView);
    for (auto* c : {m_align, m_deghost})
        if (c)
            connect(c, &QCheckBox::toggled, this, &MergeDialog::startPreview);
    auto* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    bb->button(QDialogButtonBox::Ok)->setText(tr("Merge"));
    connect(bb, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);
    l->addWidget(bb);
    startPreview();
}

MergeDialog::~MergeDialog() { *m_cancel = true; }

void MergeDialog::startPreview()
{
    *m_cancel = true; // a running preview is outdated
    m_cancel = std::make_shared<std::atomic<bool>>(false);
    const int gen = ++m_gen;
    m_done = false;
    m_error.clear();
    updateView();

    QPointer<MergeDialog> self(this);
    auto cancel = m_cancel;
    const Kind kind = m_kind;
    const QList<MergeSource> photos = m_photos;
    const HdrOptions hdr = hdrOptions();
    (void)QtConcurrent::run([kind, photos, hdr, self, cancel, gen] {
        // Posts to the dialog on the GUI thread if it still exists.
        auto post = [self](auto f) {
            QMetaObject::invokeMethod(qApp, [self, f]() {
                if (self)
                    f(self.data());
            }, Qt::QueuedConnection);
        };
        const StepProgress progress = [&](const QString& step, int d, int t) {
            post([gen, step, d, t](MergeDialog* dlg) { dlg->progress(gen, step, d, t); });
        };
        QImage toned, plain;
        QRectF crop;
        QString error;
        try {
            if (kind == Hdr) {
                HdrPreview preview(photos, hdr, progress, cancel.get());
                toned = preview.render(true, kRenderEdge);
                plain = preview.render(false, kRenderEdge);
            } else {
                PanoramaPreview preview(photos, progress, cancel.get());
                toned = preview.render(true, kRenderEdge);
                plain = preview.render(false, kRenderEdge);
                crop = preview.autoCrop();
            }
        } catch (const std::exception& e) {
            if (*cancel)
                return;
            error = QString::fromUtf8(e.what());
        }
        post([gen, toned, plain, crop, error](MergeDialog* dlg) { dlg->finished(gen, toned, plain, crop, error); });
    });
}

HdrOptions MergeDialog::hdrOptions() const
{
    HdrOptions o;
    o.align = m_align && m_align->isChecked();
    o.deghost = m_deghost && m_deghost->isChecked();
    o.autoTone = m_autoTone->isChecked();
    return o;
}

PanoOptions MergeDialog::panoOptions() const
{
    PanoOptions o;
    o.autoCrop = m_autoCrop && m_autoCrop->isChecked();
    o.autoTone = m_autoTone->isChecked();
    return o;
}

QImage MergeDialog::previewImage() const { return m_autoTone->isChecked() ? m_toned : m_plain; }

QCheckBox* MergeDialog::option(const QString& text) const
{
    for (auto* c : findChildren<QCheckBox*>())
        if (c->text() == text)
            return c;
    return nullptr;
}

void MergeDialog::progress(int gen, const QString& step, int done, int total)
{
    if (gen != m_gen || m_done)
        return;
    const QString what = m_kind == Hdr ? tr("Merging a preview…") : tr("Stitching a preview…");
    m_view->set(previewImage(), {}, total > 0 ? QStringLiteral("%1\n%2 %3/%4").arg(what, step).arg(done + 1).arg(total) : QStringLiteral("%1\n%2").arg(what, step));
}

void MergeDialog::finished(int gen, const QImage& toned, const QImage& plain, QRectF crop, const QString& error)
{
    if (gen != m_gen)
        return;
    m_done = true;
    m_toned = toned;
    m_plain = plain;
    m_crop = crop;
    m_error = error;
    updateView();
}

void MergeDialog::updateView()
{
    const QString quick = m_kind == Hdr ? tr("Preview: a quick merge at a smaller size; the merge itself works at full resolution.")
                                        : tr("Preview: a quick, smaller stitch; the merge itself works at full resolution.");
    if (!m_done) {
        // Keep showing the previous preview (if any) while the new one is made.
        m_view->set(previewImage(), {}, m_kind == Hdr ? tr("Merging a preview…") : tr("Stitching a preview…"));
        m_status->setText(quick);
        return;
    }
    if (!m_error.isEmpty()) {
        m_view->set({}, {}, (m_kind == Hdr ? tr("The preview could not be merged:\n%1") : tr("The preview could not be stitched:\n%1")).arg(m_error));
        m_status->setText(m_kind == Hdr ? tr("The merge will most likely fail the same way.")
                                        : tr("The merge will most likely fail the same way. Check that neighbouring photos overlap (by about a third)."));
        return;
    }
    const bool cropShown = m_autoCrop && m_autoCrop->isChecked();
    m_view->set(previewImage(), cropShown ? m_crop : QRectF(), QString());
    m_status->setText(quick + (cropShown ? tr(" The dimmed part is cropped away (the DNG keeps it).") : QString()));
}

} // namespace silma
