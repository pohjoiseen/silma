#pragma once
// Camera profiles installed by the user: DNG camera profiles (.dcp, e.g. Adobe Standard from a
// Lightroom or DNG Converter installation) and Camera Raw "look" profiles (.xmp, e.g. Adobe Color,
// a look table and a tone curve applied over a base .dcp). Silmä ships none: their makers'
// licences do not allow it. They are copied into profileDir() and chosen per photo by name.
//
// Rendering follows the DNG specification: white-balanced camera RGB -> ForwardMatrix (XYZ D50)
// -> linear ProPhoto RGB -> HueSatMap -> exposure -> LookTable (a look profile's replaces the
// DCP's) -> back to Rec.2020; the look's tone curve is applied to display-encoded values before the user's point curves.

#include "Color.h"
#include "Develop.h"

#include <QList>
#include <QString>
#include <memory>
#include <vector>

namespace silma {

// A hue/saturation/value delta table (ProfileHueSatMap / ProfileLookTable).
struct HueSatTable {
    int hues = 0, sats = 0, vals = 0;
    bool srgbValues = false; // value axis indexed by sRGB-encoded value (LookTableEncoding 1)
    std::vector<float> data; // (hue shift in degrees, saturation scale, value scale) per entry,
                             // value divisions outermost, then hue, then saturation
    bool valid() const { return hues > 0 && sats > 1 && vals > 0 && data.size() == size_t(hues) * sats * vals * 3; }
    // a*(1-t) + b*t, entry by entry (tables of equal dimensions)
    static HueSatTable blend(const HueSatTable& a, const HueSatTable& b, double t);
    // Applies the table to linear ProPhoto RGB in place.
    void apply(float* rgb) const;
};

struct CameraProfile {
    enum class Kind { Dcp, Look };
    Kind kind = Kind::Dcp;
    QString name; // ProfileName / the look's name: what settings store
    QString file; // file name in profileDir()

    // DCP
    QString cameraModel; // UniqueCameraModel, e.g. "Nikon D5600"
    int illuminant1 = 0, illuminant2 = 0;
    Mat3 colorMatrix1{}, colorMatrix2{}, forwardMatrix1{}, forwardMatrix2{};
    HueSatTable hueSatMap1, hueSatMap2, lookTable;
    double baselineExposureOffset = 0;

    // Look (.xmp)
    QString baseProfile;     // crs:CameraProfile, e.g. "Adobe Standard"
    QString modelRestriction; // crs:CameraModelRestriction, empty for any camera
    CurvePoints toneCurve;    // crs:ToneCurvePV2012, in [0,1]
    // lookTable above holds the look's table

    // Whether a DCP is for the camera `make model` (LibRaw names).
    bool matchesCamera(const QString& make, const QString& model) const;
};

// Parse a profile file; throws std::runtime_error on errors.
CameraProfile loadDcp(const QString& path);
CameraProfile loadLookXmp(const QString& path);
CameraProfile loadProfile(const QString& path); // by extension

// Installed profiles: $SILMA_PROFILE_DIR, or ~/.local/share/silma/profiles.
QString profileDir();
// All installed profiles (re-read when the directory changes); unreadable files are skipped.
QList<std::shared_ptr<const CameraProfile>> installedProfiles();
// Copies profile files (or every .dcp/.xmp in the given directories, recursively) into
// profileDir(), after checking that they parse. Returns the names of the installed profiles;
// `errors` gets one line per skipped file.
QStringList installProfiles(const QStringList& paths, QStringList* errors = nullptr);

// The profile new photos start with: the one last chosen in the GUI (empty: Silmä's own colour).
QString defaultProfile();
void setDefaultProfile(const QString& name);
// Settings of a photo as imported (and as Reset makes them): the defaults, with
// defaultProfile() for raws.
DevelopSettings newPhotoSettings(bool rendered);

// What a profile name resolves to for one camera: the DCP and, for a look, the look on top.
struct ResolvedProfile {
    std::shared_ptr<const CameraProfile> dcp, look;
    QString name;
    bool valid() const { return bool(dcp); }
};
ResolvedProfile resolveProfile(const QString& name, const QString& make, const QString& model);
// Profile names usable for a camera, sorted (DCPs and looks whose base DCP is installed).
QStringList profilesForCamera(const QString& make, const QString& model);

// The colour transform of a resolved profile for one white balance.
class ProfileTransform {
public:
    // `cam` gives the white point of `mul`; the DCP's colour matrices take over from the
    // raw's for it, as the DNG specification describes.
    ProfileTransform(const ResolvedProfile& p, const RawColorInfo& raw, const Vec3& mul);
    // White-balanced camera RGB (neutral 1,1,1) -> linear ProPhoto RGB (D50).
    const Mat3& camToProPhoto() const { return m_camToPP; }
    // Hue/saturation map (before exposure) and look table (after), in place on ProPhoto RGB.
    void applyHueSatMap(float* rgb) const
    {
        if (m_hsm.valid())
            m_hsm.apply(rgb);
    }
    void applyLook(float* rgb) const
    {
        if (m_look)
            m_look->apply(rgb);
    }
    double baselineExposureOffset() const { return m_baselineOffset; }

private:
    Mat3 m_camToPP{};
    HueSatTable m_hsm;
    const HueSatTable* m_look = nullptr;
    std::shared_ptr<const CameraProfile> m_dcp, m_lookProfile;
    double m_baselineOffset = 0;
};

namespace color {
extern const Mat3 kProPhotoToXyzD50;
extern const Mat3 kXyzD50ToProPhoto;
// With Bradford D50 -> D65. A function: it is built from constants of another file.
const Mat3& proPhotoToRec2020();
constexpr Vec3 kD50 = {0.96422, 1.0, 0.82521};
} // namespace color

} // namespace silma
