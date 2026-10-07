#include "Cli.h"

#include "Batch.h"
#include "Importer.h"
#include "Metadata.h"
#include "RawDecoder.h"

#include <QElapsedTimer>
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
