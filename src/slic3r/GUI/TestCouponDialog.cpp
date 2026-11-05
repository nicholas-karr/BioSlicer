#include "TestCouponDialog.hpp"

#include <wx/choice.h>
#include <wx/spinctrl.h>

#include "GUI_App.hpp"
#include "I18N.hpp"
#include "Plater.hpp"
#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/TestCoupon.hpp"

namespace Slic3r {
namespace GUI {

namespace {

enum Kind { WallThickness = 0, ChannelWidth, Overhang, Resolution, DropletVolume };

// What a coupon is made of: its first feature, the change per feature and the count, with the limits that make sense
// for its unit. The dialog keeps one set per coupon so that millimetres, degrees and nanolitres never mix.
struct KindSettings
{
    double first, step, min_first, max_first, min_step, max_step;
    int    count;
};
constexpr int kinds = 5;
KindSettings s_kind[kinds] = {
    { 0.4,  0.2,  0.05,  5.,   -1.,   1.,    6 },   // wall thickness, mm
    { 0.6,  0.2,  0.05,  10.,  -2.,   2.,    6 },   // channel width, mm
    { 20.,  10.,  1.,    85.,  -20.,  20.,   6 },   // overhang, degrees
    { 1.0,  0.1,  0.1,   5.,   0.,    1.,    8 },   // resolution, mm (each feature smaller by the change)
    { 10.,  10.,  0.1,   5000., -1000., 1000., 6 }, // droplet volume, nL
};
struct Remembered
{
    int    kind { 0 }, profile { 0 };
    double wall { 1.5 }, height { 10. };
};
Remembered s_last;

} // namespace

TestCouponDialog::TestCouponDialog(wxWindow *parent)
    : ParamDialog(parent, _L("Generate Test Coupon"), _L("Place on bed"))
{
    m_kind = add_choice(_L("Coupon:"), { _L("Wall thickness: thinnest wall that prints"), _L("Channel width: narrowest channel that stays open"),
                                         _L("Overhang: steepest overhang without support"), _L("Resolution: smallest pin and hole"),
                                         _L("Droplet volume: row of wells with growing droplets (needs a droplet channel)") }, s_last.kind);
    m_profile = add_choice(_L("Channel shape (channel coupon only):"), { _L("Round"), _L("Square") }, s_last.profile);
    m_first   = add_value(_L("First feature"), _L("mm, degrees or nL"), 0.05, 5000., s_kind[s_last.kind].first, 0.1);
    m_step    = add_value(_L("Change per feature"), _L("mm, degrees or nL"), -1000., 1000., s_kind[s_last.kind].step, 0.05);
    m_count   = add_count(_L("Number of features"), 1, 20, s_kind[s_last.kind].count);
    m_wall    = add_value(_L("Wall around channels (channel coupon only)"), _L("mm"), 0.2, 10., s_last.wall, 0.1);
    m_height  = add_value(_L("Wall height or channel length (wall and channel coupons)"), _L("mm"), 1., 60., s_last.height, 1., 1);
    add_note(_L("Wall thickness and channel width are in mm and grow by the change per feature. Overhang angles are in degrees, measured from the "
                "vertical (0 is a wall, 90 is a bridge). For the resolution coupon the diameters get smaller by the change per feature. "
                "Droplet volumes are in nanolitres (nL) per drop. Choosing another coupon loads its own values."));

    // Remember the values of the coupon being left and show those of the one chosen, with the limits of its unit.
    auto load = [this](int kind) {
        const KindSettings &k = s_kind[kind];
        m_first->SetRange(k.min_first, k.max_first);
        m_step->SetRange(k.min_step, k.max_step);
        m_first->SetValue(k.first);
        m_step->SetValue(k.step);
        m_count->SetValue(k.count);
    };
    load(s_last.kind);
    m_kind->Bind(wxEVT_CHOICE, [this, load](wxCommandEvent &) {
        s_kind[s_last.kind].first = m_first->GetValue();
        s_kind[s_last.kind].step  = m_step->GetValue();
        s_kind[s_last.kind].count = m_count->GetValue();
        s_last.kind = m_kind->GetSelection();
        load(s_last.kind);
    });
    finish_layout();
}

bool TestCouponDialog::place()
{
    s_last.kind    = m_kind->GetSelection();
    s_last.profile = m_profile->GetSelection();
    s_last.wall    = m_wall->GetValue();
    s_last.height  = m_height->GetValue();
    KindSettings &current = s_kind[s_last.kind];
    current.first = m_first->GetValue();
    current.step  = m_step->GetValue();
    current.count = m_count->GetValue();
    struct { int kind, profile, count; double first, step, wall, height; } v
        { s_last.kind, s_last.profile, current.count, current.first, current.step, s_last.wall, s_last.height };

    Coupon coupon;
    std::string name;
    switch (v.kind) {
    case WallThickness: {
        WallThicknessCouponParams p;
        p.first_thickness = v.first;
        p.step            = v.step;
        p.count           = v.count;
        p.wall_height     = v.height;
        coupon = make_wall_thickness_coupon(p);
        name   = "Wall thickness coupon";
        break;
    }
    case ChannelWidth: {
        ChannelCouponParams p;
        p.profile     = v.profile == 0 ? ChannelProfile::Round : ChannelProfile::Square;
        p.first_width = v.first;
        p.step        = v.step;
        p.count       = v.count;
        p.wall        = v.wall;
        p.length      = v.height;
        coupon = make_channel_coupon(p);
        name   = "Channel width coupon";
        break;
    }
    case Overhang: {
        OverhangCouponParams p;
        p.first_angle = v.first;
        p.step        = v.step;
        p.count       = v.count;
        coupon = make_overhang_coupon(p);
        name   = "Overhang coupon";
        break;
    }
    case DropletVolume: {
        DropletVolumeCouponParams p;
        p.first_volume_nl = v.first;
        p.step_nl         = v.step;
        p.count           = v.count;
        coupon = make_droplet_volume_coupon(p);
        name   = "Droplet volume coupon";
        break;
    }
    default: {
        ResolutionCouponParams p;
        p.first_diameter = v.first;
        p.step           = v.step;
        p.count          = v.count;
        coupon = make_resolution_coupon(p);
        name   = "Resolution coupon";
        break;
    }
    }
    int droplet_extruder = 0;
    if (v.kind == DropletVolume) {
        droplet_extruder = first_droplet_channel();
        if (droplet_extruder == 0) {
            set_status(_L("This printer has no droplet channel. Add one in the printer settings first."));
            return false;
        }
    }
    if (! coupon.valid()) {
        set_status(wxString::FromUTF8(coupon.error));
        return false;
    }

    const DynamicPrintConfig &cfg = wxGetApp().preset_bundle->printers.get_edited_preset().config;
    const BoundingBoxf bed(cfg.option<ConfigOptionPoints>("bed_shape")->values);
    const BoundingBoxf3 box = TriangleMesh(coupon.mesh).bounding_box();
    if (bed.defined && (box.size().x() > bed.size().x() || box.size().y() > bed.size().y())) {
        set_status(wxString::Format(_L("The coupon is %.1f x %.1f mm and does not fit the %.1f x %.1f mm bed. Use fewer features."),
            box.size().x(), box.size().y(), bed.size().x(), bed.size().y()));
        return false;
    }

    Model model;
    ModelObject *object = model.add_object();
    object->name = name;
    object->add_volume(TriangleMesh(std::move(coupon.mesh)))->name = coupon.parameter;
    for (DropletSet &set : coupon.droplets) {
        set.extruder = droplet_extruder;
        object->droplet_sets.emplace_back(std::move(set));
    }
    object->center_around_origin();
    object->add_instance();

    Plater *plater = wxGetApp().plater();
    Plater::TakeSnapshot snapshot(plater, _L("Add test coupon"));
    const std::vector<size_t> added = plater->add_model_objects(model.objects);
    // Placing alone leaves the coupon at the origin, which can be off the bed: put it in the middle of the bed.
    const Vec2d center = bed.defined ? Vec2d(bed.center()) : Vec2d::Zero();
    for (const size_t idx : added)
        if (idx < plater->model().objects.size()) {
            for (ModelInstance *instance : plater->model().objects[idx]->instances)
                instance->set_offset(Vec3d(center.x(), center.y(), instance->get_offset().z()));
            plater->changed_object(int(idx));
        }
    return true;
}

} // namespace GUI
} // namespace Slic3r
