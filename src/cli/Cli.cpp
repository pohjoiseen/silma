#include "Cli.h"

#include "Batch.h"
#include "Develop.h"
#include "Engine.h"
#include "Export.h"
#include "Importer.h"
#include "Metadata.h"
#include "RawDecoder.h"
#include "Upright.h"
#include "AiDenoise.h"
#include "Derived.h"
#include "Merge.h"
#include "Heal.h"
#include "Color.h"

#include <QDir>
#include <QElapsedTimer>
#include <QtConcurrent>
#include <atomic>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTextStream>
#include <functional>
#include <map>

namespace silma {

namespace {

QTextStream& out()
{
    static QTextStream s(stdout);
    return s;
}
QTextStream& err()
{
    static QTextStream s(stderr);
    return s;
}

struct CliError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

[[noreturn]] void fail(const QString& msg) { throw CliError(msg.toStdString()); }

// Small argument parser: positional arguments plus --flags and --options with values.
class Args {
public:
    Args(const QStringList& list, const QSet<QString>& valueOptions)
    {
        for (int i = 0; i < list.size(); ++i) {
            const QString& a = list[i];
            if (a == QLatin1String("--")) {
                m_pos += list.mid(i + 1);
                break;
            }
            if (a.startsWith(QLatin1String("--"))) {
                QString name = a.mid(2);
                QString value;
                if (int eq = name.indexOf('='); eq >= 0) {
                    value = name.mid(eq + 1);
                    name = name.left(eq);
                } else if (valueOptions.contains(name)) {
                    if (i + 1 >= list.size())
                        fail("Option --" + name + " needs a value");
                    value = list[++i];
                }
                m_opts.insert(name, value);
            } else if (a.size() == 2 && a[0] == '-' && a[1] != '-' && !a[1].isDigit()) {
                static const std::map<QChar, QString> shortNames = {{'b', "batch"}, {'h', "help"}, {'y', "yes"}};
                auto it = shortNames.find(a[1]);
                if (it == shortNames.end())
                    fail("Unknown option " + a);
                if (valueOptions.contains(it->second)) {
                    if (i + 1 >= list.size())
                        fail("Option " + a + " needs a value");
                    m_opts.insert(it->second, list[++i]);
                } else {
                    m_opts.insert(it->second, QString());
                }
            } else {
                m_pos << a;
            }
        }
    }
    bool has(const QString& o) const { return m_opts.contains(o); }
    QString value(const QString& o, const QString& def = {}) const { return m_opts.value(o, def); }
    const QStringList& positional() const { return m_pos; }
    QStringList optionNames() const { return m_opts.keys(); }

private:
    QStringList m_pos;
    QHash<QString, QString> m_opts;
};

struct Command {
    QString usage;
    QString help;
    QSet<QString> valueOptions;
    QSet<QString> flags;
    bool needsBatch = true;
    std::function<void(const Args&, Batch*, bool json)> run;
};

void printJson(const QJsonValue& v)
{
    QJsonDocument d = v.isArray() ? QJsonDocument(v.toArray()) : QJsonDocument(v.toObject());
    out() << d.toJson(QJsonDocument::Indented);
    out().flush();
}

QJsonObject importToJson(const ImportInfo& im)
{
    return {{"id", im.id}, {"name", im.name}, {"path", im.path}, {"photos", im.photoCount}, {"rejected", im.rejectedCount}, {"created", im.created.toString(Qt::ISODate)}};
}

QJsonObject photoToJson(const PhotoInfo& p, Batch& b)
{
    QString importName;
    if (auto im = b.importById(p.importId))
        importName = im->name;
    return {{"id", p.id},
            {"import", importName},
            {"file", p.fileName},
            {"path", p.path},
            {"captured", p.captureTime.toString(Qt::ISODate)},
            {"camera", (p.make + ' ' + p.model).trimmed()},
            {"lens", p.lens},
            {"iso", p.iso},
            {"exposure_time", p.exposureTime},
            {"f_number", p.fNumber},
            {"focal_length", p.focalLength},
            {"width", p.width},
            {"height", p.height},
            {"orientation", p.orientation},
            {"rejected", p.rejected},
            {"kind", photoKindName(p.kind)},
            {"history_pos", p.historyPos}};
}

QString formatExposure(double t)
{
    if (t <= 0)
        return QStringLiteral("-");
    if (t < 0.5)
        return QStringLiteral("1/%1").arg(qRound(1.0 / t));
    return QString::number(t, 'g', 3) + 's';
}

// Resolves photo references: numeric id, "import/file", unique file name, or "all".
QList<PhotoInfo> resolvePhotos(Batch& b, const QStringList& refs, const QString& importFilter = {})
{
    std::optional<qint64> importId;
    if (!importFilter.isEmpty()) {
        auto im = b.importByName(importFilter);
        if (!im)
            fail("No import named " + importFilter);
        importId = im->id;
    }
    QList<PhotoInfo> all = b.photos(importId);
    if (refs.isEmpty())
        fail("No photos specified");
    QList<PhotoInfo> outList;
    QSet<qint64> seen;
    auto add = [&](const PhotoInfo& p) {
        if (!seen.contains(p.id)) {
            seen.insert(p.id);
            outList << p;
        }
    };
    QHash<qint64, QString> importNames;
    for (const auto& im : b.imports())
        importNames[im.id] = im.name;
    for (const QString& ref : refs) {
        if (ref == QLatin1String("all")) {
            for (const auto& p : all)
                add(p);
            continue;
        }
        bool isNum = false;
        qint64 id = ref.toLongLong(&isNum);
        QList<PhotoInfo> matches;
        for (const auto& p : all) {
            if (isNum ? p.id == id
                      : (p.fileName.compare(ref, Qt::CaseInsensitive) == 0 ||
                         (importNames[p.importId] + '/' + p.fileName).compare(ref, Qt::CaseInsensitive) == 0 ||
                         p.path == QFileInfo(ref).absoluteFilePath()))
                matches << p;
        }
        if (matches.isEmpty())
            fail("No photo matches '" + ref + "'");
        if (matches.size() > 1)
            fail("'" + ref + "' is ambiguous; use the id or import/file form");
        add(matches.first());
    }
    return outList;
}

std::map<QString, Command> commands();

void printUsage()
{
    out() << "Silmä " SILMA_VERSION " - raw photo developer\n\n"
             "Usage: silma                     launch the GUI\n"
             "       silma [-b BATCH] [--json] COMMAND [ARGS...]\n\n"
             "The batch can also be given with the SILMA_BATCH environment variable.\n"
             "Photos are referenced by numeric id, 'import/FILE', a unique FILE name, or 'all'.\n\n"
             "Commands:\n";
    for (const auto& [name, c] : commands())
        out() << "  " << c.usage.leftJustified(46) << c.help << '\n';
    out().flush();
}

std::map<QString, Command> commands()
{
    std::map<QString, Command> m;

    m["create"] = {"create BATCH.silma", "Create a new, empty batch", {}, {}, false, [](const Args& a, Batch*, bool json) {
        if (a.positional().size() != 1)
            fail("create needs exactly one batch file name");
        QString f = a.positional()[0];
        if (!f.endsWith(QLatin1String(".silma")))
            f += QLatin1String(".silma");
        Batch b;
        b.create(f);
        if (json)
            printJson(QJsonObject{{"batch", b.file()}});
        else
            out() << "Created " << b.file() << '\n';
    }};

    m["info"] = {"info", "Summarize the batch", {}, {}, true, [](const Args&, Batch* b, bool json) {
        auto ims = b->imports();
        int photos = 0, rejected = 0;
        for (const auto& im : ims) {
            photos += im.photoCount;
            rejected += im.rejectedCount;
        }
        if (json) {
            QJsonArray arr;
            for (const auto& im : ims)
                arr.append(importToJson(im));
            printJson(QJsonObject{{"batch", b->file()}, {"name", b->name()}, {"schema_version", Batch::kSchemaVersion}, {"photos", photos}, {"rejected", rejected}, {"imports", arr}});
            return;
        }
        out() << "Batch:    " << b->file() << '\n'
              << "Imports:  " << ims.size() << '\n'
              << "Photos:   " << photos << " (" << rejected << " rejected)\n";
    }};

    m["imports"] = {"imports", "List imports in the batch", {}, {}, true, [](const Args&, Batch* b, bool json) {
        auto ims = b->imports();
        if (json) {
            QJsonArray arr;
            for (const auto& im : ims)
                arr.append(importToJson(im));
            printJson(arr);
            return;
        }
        for (const auto& im : ims)
            out() << im.name.leftJustified(20) << QString::number(im.photoCount).rightJustified(6) << " photos  "
                  << QString::number(im.rejectedCount).rightJustified(4) << " rejected  " << im.path << '\n';
    }};

    m["import"] = {"import FILES|DIRS... [--into NAME] [--split-by-date] [--move]",
                   "Copy raw files into the batch (default import: capture date)",
                   {"into"}, {"split-by-date", "move"}, true, [](const Args& a, Batch* b, bool json) {
        QStringList files = collectRawFiles(a.positional());
        if (files.isEmpty())
            fail("No raw files found");
        ImportOptions o;
        o.importName = a.value("into");
        o.splitByDate = a.has("split-by-date");
        o.move = a.has("move");
        auto progress = [&](int done, int total, const QString& f) {
            if (!json && !f.isEmpty())
                err() << QStringLiteral("[%1/%2] %3\n").arg(done + 1).arg(total).arg(QFileInfo(f).fileName()) << Qt::flush;
        };
        ImportResult r = importFiles(*b, files, o, progress);
        if (json) {
            QJsonArray added, skipped, errors, names;
            for (auto id : r.added)
                added.append(id);
            for (const auto& s : r.skipped)
                skipped.append(s);
            for (const auto& s : r.errors)
                errors.append(s);
            for (const auto& s : r.importNames)
                names.append(s);
            printJson(QJsonObject{{"added", added}, {"skipped", skipped}, {"errors", errors}, {"imports", names}});
        } else {
            out() << "Imported " << r.added.size() << " photo(s) into " << r.importNames.join(", ") << "; " << r.skipped.size() << " duplicate(s) skipped\n";
            for (const auto& e : r.errors)
                out() << "error: " << e << '\n';
        }
        if (!r.errors.isEmpty())
            throw CliError("Some files failed to import");
    }};

    m["list"] = {"list [--import NAME] [--rejected|--accepted]", "List photos", {"import"}, {"rejected", "accepted"}, true, [](const Args& a, Batch* b, bool json) {
        std::optional<qint64> importId;
        if (a.has("import")) {
            auto im = b->importByName(a.value("import"));
            if (!im)
                fail("No import named " + a.value("import"));
            importId = im->id;
        }
        QJsonArray arr;
        for (const auto& p : b->photos(importId)) {
            if ((a.has("rejected") && !p.rejected) || (a.has("accepted") && p.rejected))
                continue;
            if (json) {
                arr.append(photoToJson(p, *b));
                continue;
            }
            out() << QString::number(p.id).rightJustified(5) << "  " << (p.rejected ? "X " : "  ")
                  << p.fileName.leftJustified(16) << p.captureTime.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")) << "  "
                  << QStringLiteral("ISO %1 %2 f/%3 %4mm").arg(p.iso).arg(formatExposure(p.exposureTime)).arg(p.fNumber).arg(p.focalLength) << '\n';
        }
        if (json)
            printJson(arr);
    }};

    m["meta"] = {"meta PHOTO [--all]", "Show photo metadata (--all: every tag)", {}, {"all"}, true, [](const Args& a, Batch* b, bool json) {
        auto ps = resolvePhotos(*b, a.positional());
        if (ps.size() != 1)
            fail("meta takes exactly one photo");
        const PhotoInfo& p = ps.first();
        if (!a.has("all")) {
            MetadataSummary s = readMetadataSummary(p.path);
            QJsonObject o = photoToJson(p, *b);
            o["focal_length_35mm"] = s.focalLength35;
            o["software"] = s.software;
            if (s.latitude && s.longitude) {
                o["latitude"] = *s.latitude;
                o["longitude"] = *s.longitude;
            }
            if (s.altitude)
                o["altitude"] = *s.altitude;
            if (json) {
                printJson(o);
                return;
            }
            for (auto it = o.begin(); it != o.end(); ++it)
                out() << it.key().leftJustified(20) << it.value().toVariant().toString() << '\n';
            return;
        }
        auto tags = readAllMetadata(p.path);
        if (json) {
            QJsonArray arr;
            for (const auto& t : tags)
                arr.append(QJsonObject{{"key", t.key}, {"label", t.label}, {"value", t.value}});
            printJson(arr);
            return;
        }
        for (const auto& t : tags)
            out() << t.key.leftJustified(44) << t.value << '\n';
    }};

    auto rejectCmd = [](bool reject) {
        return [reject](const Args& a, Batch* b, bool json) {
            auto ps = resolvePhotos(*b, a.positional(), a.value("import"));
            for (const auto& p : ps)
                b->setRejected(p.id, reject);
            if (json)
                printJson(QJsonObject{{"changed", int(ps.size())}});
            else
                out() << (reject ? "Rejected " : "Unrejected ") << ps.size() << " photo(s)\n";
        };
    };
    m["reject"] = {"reject PHOTO... [--import NAME]", "Mark photos as rejected", {"import"}, {}, true, rejectCmd(true)};
    m["unreject"] = {"unreject PHOTO... [--import NAME]", "Clear the rejected mark", {"import"}, {}, true, rejectCmd(false)};

    m["delete-rejected"] = {"delete-rejected --yes [--import NAME]", "PERMANENTLY delete rejected photos' files", {"import"}, {"yes"}, true, [](const Args& a, Batch* b, bool json) {
        std::optional<qint64> importId;
        if (a.has("import")) {
            auto im = b->importByName(a.value("import"));
            if (!im)
                fail("No import named " + a.value("import"));
            importId = im->id;
        }
        QList<PhotoInfo> rejected;
        for (const auto& p : b->photos(importId))
            if (p.rejected)
                rejected << p;
        if (!a.has("yes")) {
            out() << rejected.size() << " rejected photo(s) would be deleted from disk. Re-run with --yes to delete.\n";
            for (const auto& p : rejected)
                out() << "  " << p.path << '\n';
            return;
        }
        QJsonArray deleted, failed;
        for (const auto& p : rejected) {
            if (QFile::exists(p.path) && !QFile::remove(p.path)) {
                failed.append(p.path);
                continue;
            }
            b->removePhoto(p.id);
            deleted.append(p.path);
        }
        if (json)
            printJson(QJsonObject{{"deleted", deleted}, {"failed", failed}});
        else
            out() << "Deleted " << deleted.size() << " file(s)" << (failed.isEmpty() ? "" : QStringLiteral(", %1 failed").arg(failed.size())) << '\n';
    }};

    m["history"] = {"history PHOTO", "Show the edit history of a photo", {}, {}, true, [](const Args& a, Batch* b, bool json) {
        auto ps = resolvePhotos(*b, a.positional());
        if (ps.size() != 1)
            fail("history takes exactly one photo");
        int pos = b->historyPos(ps[0].id);
        QJsonArray arr;
        for (const auto& h : b->history(ps[0].id)) {
            if (json)
                arr.append(QJsonObject{{"seq", h.seq}, {"label", h.label}, {"current", h.seq == pos}, {"settings", QJsonDocument::fromJson(h.settings.toUtf8()).object()}, {"time", h.time.toString(Qt::ISODate)}});
            else
                out() << (h.seq == pos ? "* " : "  ") << QString::number(h.seq).rightJustified(3) << "  " << h.label << '\n';
        }
        if (json)
            printJson(arr);
    }};

    m["settings"] = {"settings PHOTO", "Show current develop settings (with effective white balance)", {}, {}, true, [](const Args& a, Batch* b, bool json) {
        auto ps = resolvePhotos(*b, a.positional());
        if (ps.size() != 1)
            fail("settings takes exactly one photo");
        DevelopSettings s = DevelopSettings::fromJsonString(b->settings(ps[0].id));
        DevelopEngine e(ps[0].path, {.halfSize = true});
        WbValues wb = e.effectiveWb(s);
        QJsonObject o{{"stored", s.toJson()}, {"wb_mode", wbModeName(s.wbMode)}, {"temperature", qRound(wb.temperature)}, {"tint", qRound(wb.tint)},
                      {"lens_profile", e.lensDescription()}};
        QJsonObject all;
        for (const auto& p : paramDefs())
            all[p.key] = s.*p.field;
        all["temperature"] = qRound(wb.temperature);
        all["tint"] = qRound(wb.tint);
        o["values"] = all;
        QSize sz = e.outputSize(s);
        o["output_size"] = QJsonArray{sz.width(), sz.height()};
        if (json) {
            printJson(o);
            return;
        }
        out() << "White balance: " << wbModeName(s.wbMode) << " (" << qRound(wb.temperature) << " K, tint " << qRound(wb.tint) << ")\n";
        out() << "Lens profile:  " << (e.lensDescription().isEmpty() ? QStringLiteral("none") : e.lensDescription()) << '\n';
        out() << "Output size:   " << sz.width() << "x" << sz.height() << '\n';
        QString group;
        for (const auto& p : paramDefs()) {
            if (QString(p.group) == QLatin1String("wb"))
                continue;
            if (group != QLatin1String(p.group)) {
                group = p.group;
                out() << '[' << group << "]\n";
            }
            double v = s.*p.field;
            out() << "  " << QString(p.key).leftJustified(18) << QString::number(v, 'f', p.decimals) << (std::abs(v - p.def) > 1e-9 ? "  *" : "") << '\n';
        }
        auto curve = [&](const char* name, const CurvePoints& c) {
            if (c.points.isEmpty())
                return;
            QStringList pts;
            for (const auto& pt : c.points)
                pts << QStringLiteral("%1:%2").arg(pt.x()).arg(pt.y());
            out() << "  " << QString(name).leftJustified(18) << pts.join(',') << '\n';
        };
        curve("curve", s.curveRgb);
        curve("curve_r", s.curveR);
        curve("curve_g", s.curveG);
        curve("curve_b", s.curveB);
        if (s.crop != QRectF(0, 0, 1, 1))
            out() << "  crop              " << s.crop.x() << ',' << s.crop.y() << ',' << s.crop.width() << ',' << s.crop.height() << '\n';
    }};

    m["set"] = {"set PHOTO... KEY=VALUE... [--label TEXT]", "Change develop settings (adds a history step)", {"label", "import"}, {}, true, [](const Args& a, Batch* b, bool json) {
        QStringList refs, assigns;
        for (const auto& x : a.positional())
            (x.contains('=') ? assigns : refs) << x;
        if (assigns.isEmpty())
            fail("Nothing to set; use KEY=VALUE (see 'silma keys')");
        auto ps = resolvePhotos(*b, refs, a.value("import"));
        QStringList labels;
        for (const auto& as : assigns) {
            QString k = as.section('=', 0, 0).replace('-', '_');
            const ParamDef* p = findParam(k);
            labels << (p ? QStringLiteral("%1 %2").arg(QString::fromUtf8(p->label), as.section('=', 1)) : as);
        }
        QString label = a.value("label", labels.join(", "));
        QJsonArray results;
        for (const auto& p : ps) {
            DevelopSettings s = DevelopSettings::fromJsonString(b->settings(p.id));
            bool touchesWb = false, setsTemp = false, setsTint = false;
            for (const auto& as : assigns) {
                setsTemp |= as.startsWith(QLatin1String("temperature="));
                setsTint |= as.startsWith(QLatin1String("tint="));
            }
            touchesWb = setsTemp || setsTint;
            if (touchesWb && s.wbMode != WbMode::Custom) {
                // Switching to a custom WB: start from the currently effective values.
                DevelopEngine e(p.path, {.halfSize = true});
                WbValues wb = e.effectiveWb(s);
                s.temperature = std::round(wb.temperature);
                s.tint = std::round(wb.tint);
            }
            for (const auto& as : assigns)
                if (QString e = applyAssignment(s, as); !e.isEmpty())
                    fail(e);
            int pos = b->pushHistory(p.id, label, s.toJsonString());
            results.append(QJsonObject{{"id", p.id}, {"history_pos", pos}, {"settings", s.toJson()}});
        }
        if (json)
            printJson(results);
        else
            out() << "Updated " << ps.size() << " photo(s): " << label << '\n';
    }};

    auto moveHistory = [](int delta) {
        return [delta](const Args& a, Batch* b, bool json) {
            auto ps = resolvePhotos(*b, a.positional(), a.value("import"));
            int steps = a.value("steps", "1").toInt();
            QJsonArray arr;
            for (const auto& p : ps) {
                int pos = b->historyPos(p.id);
                int n = int(b->history(p.id).size());
                int np = std::clamp(pos + delta * steps, 0, n - 1);
                if (a.has("to"))
                    np = std::clamp(a.value("to").toInt(), 0, n - 1);
                b->setHistoryPos(p.id, np);
                arr.append(QJsonObject{{"id", p.id}, {"history_pos", np}});
                if (!json)
                    out() << p.fileName << ": history position " << pos << " -> " << np << '\n';
            }
            if (json)
                printJson(arr);
        };
    };
    m["undo"] = {"undo PHOTO... [--steps N] [--to SEQ]", "Step back in the edit history", {"steps", "to", "import"}, {}, true, moveHistory(-1)};
    m["redo"] = {"redo PHOTO... [--steps N]", "Step forward in the edit history", {"steps", "to", "import"}, {}, true, moveHistory(1)};

    m["reset"] = {"reset PHOTO...", "Reset develop settings to defaults (adds a history step)", {"import"}, {}, true, [](const Args& a, Batch* b, bool json) {
        auto ps = resolvePhotos(*b, a.positional(), a.value("import"));
        for (const auto& p : ps)
            b->pushHistory(p.id, "Reset", DevelopSettings().toJsonString());
        if (json)
            printJson(QJsonObject{{"changed", int(ps.size())}});
        else
            out() << "Reset " << ps.size() << " photo(s)\n";
    }};

    m["sync"] = {"sync FROM TO... [--only KEYS]", "Copy develop settings from one photo to others", {"only", "import"}, {}, true, [](const Args& a, Batch* b, bool json) {
        if (a.positional().size() < 2)
            fail("sync needs a source and at least one target");
        auto src = resolvePhotos(*b, {a.positional()[0]});
        auto dst = resolvePhotos(*b, a.positional().mid(1), a.value("import"));
        DevelopSettings from = DevelopSettings::fromJsonString(b->settings(src[0].id));
        QStringList only = a.value("only").split(',', Qt::SkipEmptyParts);
        int n = 0;
        for (const auto& p : dst) {
            if (p.id == src[0].id)
                continue;
            DevelopSettings s = from;
            if (!only.isEmpty()) {
                s = DevelopSettings::fromJsonString(b->settings(p.id));
                for (const QString& k : only) {
                    if (const ParamDef* d = findParam(k))
                        s.*d->field = from.*d->field;
                    else if (k == QLatin1String("wb")) {
                        s.wbMode = from.wbMode;
                        s.temperature = from.temperature;
                        s.tint = from.tint;
                    } else if (k == QLatin1String("crop"))
                        s.crop = from.crop;
                    else if (k == QLatin1String("curve")) {
                        s.curveRgb = from.curveRgb;
                        s.curveR = from.curveR;
                        s.curveG = from.curveG;
                        s.curveB = from.curveB;
                    } else
                        fail("Unknown key " + k);
                }
            }
            b->pushHistory(p.id, "Sync from " + src[0].fileName, s.toJsonString());
            ++n;
        }
        if (json)
            printJson(QJsonObject{{"changed", n}});
        else
            out() << "Synced settings to " << n << " photo(s)\n";
    }};

    m["auto"] = {"auto PHOTO... [--tone] [--wb]", "Automatic tone and/or white balance (default: both)", {"import"}, {"tone", "wb"}, true, [](const Args& a, Batch* b, bool json) {
        auto ps = resolvePhotos(*b, a.positional(), a.value("import"));
        bool tone = a.has("tone") || !a.has("wb"), wbAuto = a.has("wb") || !a.has("tone");
        QJsonArray arr;
        for (const auto& p : ps) {
            DevelopSettings s = DevelopSettings::fromJsonString(b->settings(p.id));
            DevelopEngine e(p.path, {.halfSize = true});
            if (wbAuto)
                s.wbMode = WbMode::Auto;
            if (tone)
                s = e.autoTone(s);
            QString label = tone && wbAuto ? "Auto tone & WB" : tone ? "Auto tone" : "Auto WB";
            b->pushHistory(p.id, label, s.toJsonString());
            arr.append(QJsonObject{{"id", p.id}, {"settings", s.toJson()}});
            if (!json)
                out() << p.fileName << ": " << QJsonDocument(s.toJson()).toJson(QJsonDocument::Compact) << '\n';
        }
        if (json)
            printJson(arr);
    }};

    m["keys"] = {"keys", "List settable develop keys with ranges", {}, {}, false, [](const Args&, Batch*, bool json) {
        QJsonArray arr;
        for (const auto& p : paramDefs()) {
            if (json)
                arr.append(QJsonObject{{"key", p.key}, {"label", p.label}, {"group", p.group}, {"min", p.min}, {"max", p.max}, {"default", p.def}});
            else
                out() << QString(p.key).leftJustified(18) << QString(p.group).leftJustified(11) << QStringLiteral("%1..%2 (default %3)").arg(p.min).arg(p.max).arg(p.def) << '\n';
        }
        if (json) {
            printJson(arr);
            return;
        }
        out() << "wb                as-shot | auto | custom\n"
                 "crop              x,y,w,h (normalized, of the transformed image) | none\n"
                 "crop_lock_aspect  true | false\n"
                 "curve, curve_r, curve_g, curve_b   x:y,x:y,... (0..1, display-encoded) | linear\n";
    }};

    m["render"] = {"render PHOTO OUT.jpg [--size N|full] [--roi x,y,w,h] [--before] [--quality Q] [--crop-off]",
                   "Render a preview JPEG (long edge N, default 1600; full = 1:1)", {"size", "quality", "roi"}, {"before", "crop-off"}, true, [](const Args& a, Batch* b, bool json) {
        if (a.positional().size() != 2)
            fail("render needs PHOTO and output file");
        auto ps = resolvePhotos(*b, {a.positional()[0]});
        DevelopSettings s = a.has("before") ? DevelopSettings() : DevelopSettings::fromJsonString(b->settings(ps[0].id));
        QElapsedTimer t;
        t.start();
        DevelopEngine e(ps[0].path);
        addHealPatches(e, loadHealPatches(*b, s));
        qint64 decodeMs = t.restart();
        bool full = a.value("size") == QLatin1String("full");
        int size = full ? 0 : a.value("size", "1600").toInt();
        Geometry g = e.geometry(s, a.has("crop-off"));
        QSizeF cs = g.cropSize();
        QRectF roi(0, 0, 1, 1);
        if (a.has("roi")) {
            auto r = a.value("roi").split(',');
            if (r.size() != 4)
                fail("--roi needs x,y,w,h (normalized)");
            roi = QRectF(r[0].toDouble(), r[1].toDouble(), r[2].toDouble(), r[3].toDouble()).intersected(QRectF(0, 0, 1, 1));
            if (roi.isEmpty())
                fail("--roi is empty");
        }
        cs = QSizeF(cs.width() * roi.width(), cs.height() * roi.height());
        double k = full ? 1.0 : std::min(1.0, size / std::max(cs.width(), cs.height()));
        RenderRequest req{.width = std::max(1, int(std::round(cs.width() * k))), .height = std::max(1, int(std::round(cs.height() * k))), .roi = roi, .highQuality = full, .ignoreCrop = a.has("crop-off")};
        ImageF img = e.renderEncoded(s, req);
        qint64 renderMs = t.elapsed();
        writeJpeg(a.positional()[1], img, a.value("quality", "90").toInt(), ps[0].path);
        if (json)
            printJson(QJsonObject{{"file", QFileInfo(a.positional()[1]).absoluteFilePath()}, {"width", img.width}, {"height", img.height}, {"decode_ms", decodeMs}, {"render_ms", renderMs}});
        else
            out() << "Wrote " << a.positional()[1] << " (" << img.width << "x" << img.height << ", decode " << decodeMs << " ms, render " << renderMs << " ms)\n";
    }};

    m["export"] = {"export PHOTO...|--import NAME|--all --to DIR [--pattern P] [--quality Q] [--long-edge N|--short-edge N] [--include-rejected] [--overwrite]",
                   "Export JPEGs (default pattern {date}-{name})", {"to", "pattern", "quality", "long-edge", "short-edge", "import", "jobs"}, {"all", "include-rejected", "overwrite"}, true,
                   [](const Args& a, Batch* b, bool json) {
        if (!a.has("to"))
            fail("export needs --to DIR");
        QList<PhotoInfo> ps;
        if (a.has("all") || (a.positional().isEmpty() && a.has("import"))) {
            std::optional<qint64> importId;
            if (a.has("import")) {
                auto im = b->importByName(a.value("import"));
                if (!im)
                    fail("No import named " + a.value("import"));
                importId = im->id;
            }
            ps = b->photos(importId);
        } else {
            ps = resolvePhotos(*b, a.positional(), a.value("import"));
        }
        if (!a.has("include-rejected"))
            ps.erase(std::remove_if(ps.begin(), ps.end(), [](const PhotoInfo& p) { return p.rejected; }), ps.end());
        ExportOptions o;
        o.folder = QFileInfo(a.value("to")).absoluteFilePath();
        o.pattern = a.value("pattern", o.pattern);
        o.quality = a.value("quality", "90").toInt();
        o.overwrite = a.has("overwrite");
        if (a.has("long-edge")) {
            o.sizeMode = ExportSizeMode::LongEdge;
            o.size = a.value("long-edge").toInt();
        } else if (a.has("short-edge")) {
            o.sizeMode = ExportSizeMode::ShortEdge;
            o.size = a.value("short-edge").toInt();
        }
        QHash<qint64, QString> importNames;
        for (const auto& im : b->imports())
            importNames[im.id] = im.name;
        struct Job {
            PhotoInfo p;
            DevelopSettings s;
            int seq;
            QMap<qint64, HealPatch> patches;
        };
        QList<Job> jobs;
        for (int i = 0; i < ps.size(); ++i) {
            DevelopSettings js = DevelopSettings::fromJsonString(b->settings(ps[i].id));
            jobs.append({ps[i], js, i + 1, loadHealPatches(*b, js)});
        }
        // Each render is internally parallel; run a couple at once to overlap decoding.
        QThreadPool pool;
        pool.setMaxThreadCount(std::max(1, a.value("jobs", "2").toInt()));
        std::atomic<int> done{0};
        QList<QPair<QString, QString>> results = QtConcurrent::blockingMapped(&pool, jobs, [&](const Job& j) {
            QPair<QString, QString> r;
            try {
                r.first = exportPhoto(j.p, j.s, importNames.value(j.p.importId), j.seq, o, j.patches);
            } catch (const std::exception& e) {
                r.second = j.p.fileName + ": " + QString::fromUtf8(e.what());
            }
            int n = ++done;
            if (!json)
                err() << QStringLiteral("[%1/%2] %3\n").arg(n).arg(jobs.size()).arg(r.first.isEmpty() ? r.second : QFileInfo(r.first).fileName()) << Qt::flush;
            return r;
        });
        QJsonArray files, errors;
        for (const auto& r : results)
            r.second.isEmpty() ? files.append(r.first) : errors.append(r.second);
        if (json)
            printJson(QJsonObject{{"files", files}, {"errors", errors}});
        else
            out() << "Exported " << files.size() << " file(s) to " << o.folder << '\n';
        if (!errors.isEmpty())
            throw CliError(QStringLiteral("%1 export(s) failed").arg(errors.size()).toStdString());
    }};

    m["upright"] = {"upright PHOTO... [--mode vertical|horizontal|both] [--no-crop]", "Automatic perspective correction (+ constrained crop)", {"mode", "import"}, {"no-crop"}, true, [](const Args& a, Batch* b, bool json) {
        auto ps = resolvePhotos(*b, a.positional(), a.value("import"));
        QString m = a.value("mode", "both");
        UprightMode mode = m == QLatin1String("vertical") ? UprightMode::Vertical : m == QLatin1String("horizontal") ? UprightMode::Horizontal : UprightMode::Both;
        QJsonArray arr;
        for (const auto& p : ps) {
            DevelopSettings s = DevelopSettings::fromJsonString(b->settings(p.id));
            DevelopEngine e(p.path, {.halfSize = true});
            UprightResult r = autoUpright(e, s, mode);
            if (!r.ok) {
                if (!json)
                    out() << p.fileName << ": not enough straight lines found\n";
                continue;
            }
            s.perspVertical = r.vertical;
            s.perspHorizontal = r.horizontal;
            s.rotate = r.rotate;
            if (!a.has("no-crop")) {
                QSizeF cs = e.geometry(s, true).canvasSize();
                s.crop = autoCropRect(e.geometry(s, true), cs.width() / cs.height());
            }
            b->pushHistory(p.id, "Upright (" + m + ")", s.toJsonString());
            arr.append(QJsonObject{{"id", p.id}, {"vertical", r.vertical}, {"horizontal", r.horizontal}, {"rotate", r.rotate}, {"segments", r.segmentsUsed}});
            if (!json)
                out() << p.fileName << QStringLiteral(": vertical %1, horizontal %2, rotate %3 (%4 lines)\n").arg(r.vertical).arg(r.horizontal).arg(r.rotate).arg(r.segmentsUsed);
        }
        if (json)
            printJson(arr);
    }};

    m["autocrop"] = {"autocrop PHOTO... [--aspect W:H|original]", "Largest crop without empty borders (panoramas: any aspect unless given)", {"aspect", "import"}, {}, true, [](const Args& a, Batch* b, bool json) {
        auto ps = resolvePhotos(*b, a.positional(), a.value("import"));
        QJsonArray arr;
        for (const auto& p : ps) {
            DevelopSettings s = DevelopSettings::fromJsonString(b->settings(p.id));
            DevelopEngine e(p.path, {.halfSize = true});
            Geometry g = e.geometry(s, true);
            QSizeF cs = g.canvasSize();
            double aspect = cs.width() / cs.height();
            QString as = a.value("aspect");
            if (as.contains(':'))
                aspect = as.section(':', 0, 0).toDouble() / std::max(1e-6, as.section(':', 1, 1).toDouble());
            else if (as.isEmpty() && g.hasValidMask())
                aspect = 0; // panorama: the largest covered area, whatever its shape
            else if (as.isEmpty() && s.crop != QRectF(0, 0, 1, 1))
                aspect = s.crop.width() * cs.width() / (s.crop.height() * cs.height());
            s.crop = autoCropRect(g, aspect);
            b->pushHistory(p.id, "Auto Crop", s.toJsonString());
            arr.append(QJsonObject{{"id", p.id}, {"crop", QJsonArray{s.crop.x(), s.crop.y(), s.crop.width(), s.crop.height()}}});
            if (!json)
                out() << p.fileName << ": crop " << s.crop.x() << ',' << s.crop.y() << ',' << s.crop.width() << ',' << s.crop.height() << '\n';
        }
        if (json)
            printJson(arr);
    }};

    m["ai-bench"] = {"ai-bench MODEL RAWFILE OUT.jpg [--roi x,y,w,h]", "Debug: AI-denoise a raw crop and write a before/after JPEG", {"roi"}, {}, false, [](const Args& a, Batch*, bool) {
        if (a.positional().size() != 3)
            fail("ai-bench MODEL RAWFILE OUT.jpg");
        QString model = a.positional()[0];
        if (!aiModelInstalled(model)) {
            err() << "Downloading " << model << "...\n" << Qt::flush;
            downloadAiModel(model);
        }
        RawImage raw = decodeRaw(a.positional()[1]);
        auto r = a.value("roi", "0.4,0.4,0.1,0.15").split(',');
        int x0 = int(r[0].toDouble() * raw.image.width), y0 = int(r[1].toDouble() * raw.image.height);
        int cw = int(r[2].toDouble() * raw.image.width), ch = int(r[3].toDouble() * raw.image.height);
        ImageF crop(cw, ch);
        for (int y = 0; y < ch; ++y)
            std::copy_n(raw.image.px(x0, y0 + y), cw * 3, crop.row(y));
        QElapsedTimer t;
        t.start();
        QString prov;
        ImageF den = aiDenoiseLinear(model, crop, 1.0, {}, nullptr, &prov);
        double mpix = double(cw) * ch / 1e6;
        out() << QStringLiteral("%1: %2x%3 in %4 ms on %5 (%6 s per 24 MP)\n").arg(model).arg(cw).arg(ch).arg(t.elapsed()).arg(prov).arg(t.elapsed() / 1000.0 / mpix * 24, 0, 'f', 1);
        // Quick look: simple gain + sRGB gamma, original left, denoised right.
        float gain = 0.25f / std::max(1e-4f, [&] { double s = 0; for (float v : crop.data) s += v; return float(s / crop.data.size()); }());
        ImageF both(cw * 2, ch);
        for (int y = 0; y < ch; ++y)
            for (int x = 0; x < cw; ++x)
                for (int c = 0; c < 3; ++c) {
                    both.px(x, y)[c] = srgbEncode(std::min(1.0f, crop.px(x, y)[c] * gain));
                    both.px(x + cw, y)[c] = srgbEncode(std::min(1.0f, den.px(x, y)[c] * gain));
                }
        writeJpeg(a.positional()[2], both, 92, QString());
    }};

    m["ai-denoise"] = {"ai-denoise PHOTO... [--model ID] [--strength 0..100]", "AI noise reduction into a new DNG photo (slow on CPU)", {"model", "strength", "import"}, {}, true, [](const Args& a, Batch* b, bool json) {
        auto ps = resolvePhotos(*b, a.positional(), a.value("import"));
        QString model = a.value("model", defaultAiDenoiseModel());
        if (!findAiModel(model))
            fail("Unknown model " + model);
        double strength = a.value("strength", "100").toDouble() / 100.0;
        QJsonArray arr;
        for (const auto& p : ps) {
            QElapsedTimer t;
            t.start();
            QString provider, lastStep;
            qint64 id = createAiDenoised(*b, p.id, model, strength, [&](const QString& step, int d, int total) {
                if (json)
                    return;
                if (step != lastStep || (total > 0 && (d % 10 == 0 || d == total))) {
                    err() << "\r" << p.fileName << ": " << step << (total > 0 ? QStringLiteral(" %1/%2").arg(d).arg(total) : QString()) << "      " << Qt::flush;
                    lastStep = step;
                }
            }, nullptr, &provider);
            if (!json)
                err() << '\n';
            auto np = b->photo(id);
            arr.append(QJsonObject{{"source", p.id}, {"id", id}, {"file", np->path}, {"seconds", t.elapsed() / 1000.0}, {"provider", provider}});
            if (!json)
                out() << p.fileName << " -> " << np->fileName << " (id " << id << ", " << t.elapsed() / 1000 << " s on " << provider << ")\n";
        }
        if (json)
            printJson(arr);
    }};

    m["ai-models"] = {"ai-models [--download ID]", "List AI models and execution providers", {"download"}, {}, false, [](const Args& a, Batch*, bool json) {
        if (a.has("download"))
            downloadAiModel(a.value("download"), [](qint64 r, qint64 t) { err() << "\r" << r / 1048576 << "/" << t / 1048576 << " MB" << Qt::flush; });
        QJsonArray arr;
        for (const auto& m : aiDenoiseModels()) {
            arr.append(QJsonObject{{"id", m.id}, {"name", m.name}, {"license", m.license}, {"installed", aiModelInstalled(m.id)}, {"default", m.id == defaultAiDenoiseModel()}});
            if (!json)
                out() << m.id.leftJustified(20) << m.name.leftJustified(30) << m.license.leftJustified(12) << (aiModelInstalled(m.id) ? "installed" : "not downloaded") << '\n';
        }
        if (json)
            printJson(QJsonObject{{"models", arr}, {"providers", QJsonArray::fromStringList(aiExecutionProviders())}, {"model_dir", aiModelDir("")}});
        else
            out() << "Execution providers: " << aiExecutionProviders().join(", ") << "\nCPU threads: " << aiThreadCount() << "\nModel directory: " << aiModelDir("") << '\n';
    }};

    m["linear-copy"] = {"linear-copy PHOTO", "Debug: re-encode a raw as linear DNG (round-trip test)", {}, {}, true, [](const Args& a, Batch* b, bool) {
        auto ps = resolvePhotos(*b, a.positional());
        for (const auto& p : ps)
            out() << "id " << createLinearCopy(*b, p.id) << '\n';
    }};

    auto mergeCmd = [](bool hdr) {
        return [hdr](const Args& a, Batch* b, bool json) {
            auto ps = resolvePhotos(*b, a.positional(), a.value("import"));
            QList<qint64> ids;
            for (const auto& p : ps)
                ids << p.id;
            QElapsedTimer t;
            t.start();
            auto progress = [&](const QString& step, int d, int total) {
                if (!json)
                    err() << "\r" << step << (total > 0 ? QStringLiteral(" %1/%2").arg(d).arg(total) : QString()) << "          " << Qt::flush;
            };
            qint64 id;
            if (hdr) {
                HdrOptions o;
                o.align = !a.has("no-align");
                o.deghost = !a.has("no-deghost");
                id = createHdrMerge(*b, ids, o, progress);
            } else {
                PanoOptions o;
                o.autoCrop = !a.has("no-crop");
                id = createPanorama(*b, ids, o, progress);
            }
            auto np = b->photo(id);
            if (json)
                printJson(QJsonObject{{"id", id}, {"file", np->path}, {"width", np->width}, {"height", np->height}, {"seconds", t.elapsed() / 1000.0}});
            else
                out() << "\n" << np->fileName << " (id " << id << ", " << np->width << "x" << np->height << ", " << t.elapsed() / 1000 << " s)\n";
        };
    };
    m["hdr"] = {"hdr PHOTO PHOTO... [--no-align] [--no-deghost]", "Merge bracketed exposures into an HDR DNG", {"import"}, {"no-align", "no-deghost"}, true, mergeCmd(true)};
    m["pano"] = {"pano PHOTO PHOTO... [--no-crop]", "Stitch a cylindrical panorama DNG", {"import"}, {"no-crop"}, true, mergeCmd(false)};

    m["heal"] = {"heal PHOTO X,Y[,R]... | --clear | --undo-last", "Generative heal of spots; X,Y,R relative to the rendered output (R of the long edge, default 0.02)",
                 {"import"}, {"clear", "undo-last"}, true, [](const Args& a, Batch* b, bool json) {
        QStringList refs, spots;
        for (const auto& x : a.positional())
            (x.contains(',') ? spots : refs) << x;
        auto ps = resolvePhotos(*b, refs, a.value("import"));
        if (ps.size() != 1)
            fail("heal takes exactly one photo");
        const PhotoInfo& p = ps[0];
        DevelopSettings s = DevelopSettings::fromJsonString(b->settings(p.id));
        if (a.has("clear") || a.has("undo-last")) {
            if (a.has("clear"))
                s.heals.clear();
            else if (!s.heals.isEmpty())
                s.heals.removeLast();
            b->pushHistory(p.id, a.has("clear") ? "Clear Heal" : "Remove Heal Spot", s.toJsonString());
            out() << s.heals.size() << " heal spot(s) left\n";
            return;
        }
        if (spots.isEmpty())
            fail("Give at least one X,Y[,R] spot");
        DevelopEngine e(p.path);
        Geometry g = e.geometry(s);
        QSizeF crop = g.cropSize();
        double sensorLong = std::max(e.raw().fullWidth, e.raw().fullHeight);
        QJsonArray arr;
        QString provider;
        for (const QString& sp : spots) {
            auto v = sp.split(',');
            double x = v[0].toDouble(), y = v[1].toDouble(), r = v.size() > 2 ? v[2].toDouble() : 0.02;
            float src[6], gain[3];
            if (!g.mapCrop(x, y, src, gain))
                fail("Spot " + sp + " is outside the image");
            HealSpot h;
            h.x = src[2];
            h.y = src[3];
            h.radius = r * std::max(crop.width(), crop.height()) / sensorLong / g.lensZoom();
            QElapsedTimer t;
            t.start();
            HealPatch patch = generateHealPatch(e.raw(), h, &provider);
            h.patchId = storeHealPatch(*b, p.id, patch);
            s.heals.append(h);
            arr.append(QJsonObject{{"x", h.x}, {"y", h.y}, {"radius", h.radius}, {"patch", h.patchId}, {"ms", t.elapsed()}, {"provider", provider}});
            if (!json)
                out() << "Healed spot at " << sp << " in " << t.elapsed() << " ms (" << provider << ")\n";
        }
        b->pushHistory(p.id, spots.size() == 1 ? "Heal" : QStringLiteral("Heal (%1 spots)").arg(spots.size()), s.toJsonString());
        if (json)
            printJson(arr);
    }};

    m["show"] = {"show PHOTO", "Make a running GUI display this photo", {}, {}, true, [](const Args& a, Batch* b, bool json) {
        auto ps = resolvePhotos(*b, a.positional());
        if (ps.size() != 1)
            fail("show takes exactly one photo");
        b->setState("gui.goto", QString::number(ps[0].id));
        if (json)
            printJson(QJsonObject{{"id", ps[0].id}});
        else
            out() << "Requested GUI to show " << ps[0].fileName << '\n';
    }};

    m["probe"] = {"probe RAWFILE [--half]", "Decode a raw file and report timing/color data (debug)", {}, {"half"}, false, [](const Args& a, Batch*, bool json) {
        if (a.positional().size() != 1)
            fail("probe takes one file");
        QElapsedTimer t;
        t.start();
        RawImage r = decodeRaw(a.positional()[0], {.halfSize = a.has("half")});
        qint64 ms = t.elapsed();
        auto arr3 = [](const std::array<double, 3>& v) { return QJsonArray{v[0], v[1], v[2]}; };
        QJsonObject o{{"make", r.make}, {"model", r.model}, {"lens", r.lens}, {"width", r.image.width}, {"height", r.image.height},
                      {"full_width", r.fullWidth}, {"full_height", r.fullHeight}, {"orientation", r.orientation}, {"decode_ms", ms},
                      {"as_shot_mul", arr3(r.color.asShotMul)}, {"daylight_mul", arr3(r.color.daylightMul)}, {"applied_mul", arr3(r.color.appliedMul)},
                      {"baseline_exposure", r.color.baselineExposure}, {"crop", QJsonArray{r.cropX, r.cropY, r.cropW, r.cropH}},
                      {"opcodes", QJsonArray{r.opcodes.list1.size(), r.opcodes.list2.size(), r.opcodes.list3.size()}}};
        double sum = 0, mx = 0;
        for (float v : r.image.data) {
            sum += v;
            mx = std::max(mx, double(v));
        }
        o["mean"] = sum / std::max<size_t>(1, r.image.data.size());
        o["max"] = mx;
        QJsonArray m;
        for (const auto& row : r.color.xyzToCam)
            m.append(arr3(row));
        o["xyz_to_cam"] = m;
        if (json)
            printJson(o);
        else
            for (auto it = o.begin(); it != o.end(); ++it)
                out() << it.key().leftJustified(20) << QJsonDocument(QJsonArray{it.value()}).toJson(QJsonDocument::Compact).mid(1).chopped(1) << '\n';
    }};

    return m;
}

} // namespace

bool isCliInvocation(const QStringList& args)
{
    // "silma" and "silma FILE.silma" open the GUI; everything else is a CLI command.
    if (args.size() == 2 && args[1].endsWith(QLatin1String(".silma")))
        return false;
    return args.size() > 1;
}

int runCli(const QStringList& argsIn)
{
    QStringList args = argsIn.mid(1);
    // Global options precede the command.
    QString batchFile = qEnvironmentVariable("SILMA_BATCH");
    bool json = false;
    while (!args.isEmpty() && args[0].startsWith('-')) {
        QString a = args.takeFirst();
        if ((a == QLatin1String("-b") || a == QLatin1String("--batch")) && !args.isEmpty())
            batchFile = args.takeFirst();
        else if (a.startsWith(QLatin1String("--batch=")))
            batchFile = a.mid(8);
        else if (a == QLatin1String("--json"))
            json = true;
        else if (a == QLatin1String("-h") || a == QLatin1String("--help")) {
            printUsage();
            return 0;
        } else if (a == QLatin1String("--version")) {
            out() << "silma " SILMA_VERSION "\n";
            return 0;
        } else {
            err() << "Unknown option " << a << '\n';
            return 2;
        }
    }
    if (args.isEmpty() || args[0] == QLatin1String("help")) {
        printUsage();
        return 0;
    }
    auto cmds = commands();
    auto it = cmds.find(args[0]);
    if (it == cmds.end()) {
        err() << "Unknown command '" << args[0] << "'. Try 'silma help'.\n";
        return 2;
    }
    const Command& c = it->second;
    try {
        QSet<QString> valueOpts = c.valueOptions;
        valueOpts << "batch";
        Args a(args.mid(1), valueOpts);
        if (a.has("json"))
            json = true;
        if (a.has("batch"))
            batchFile = a.value("batch");
        for (const QString& o : a.optionNames())
            if (!c.valueOptions.contains(o) && !c.flags.contains(o) && o != QLatin1String("json") && o != QLatin1String("batch"))
                fail("Unknown option --" + o + " for " + args[0] + ". Usage: silma " + c.usage);
        Batch batch;
        if (c.needsBatch) {
            if (batchFile.isEmpty())
                fail("No batch given (use -b FILE or SILMA_BATCH)");
            batch.open(batchFile);
        }
        c.run(a, c.needsBatch ? &batch : nullptr, json);
        out().flush();
        return 0;
    } catch (const std::exception& e) {
        out().flush();
        if (json)
            err() << QJsonDocument(QJsonObject{{"error", QString::fromUtf8(e.what())}}).toJson(QJsonDocument::Compact) << '\n';
        else
            err() << "error: " << e.what() << '\n';
        return 1;
    }
}

} // namespace silma
