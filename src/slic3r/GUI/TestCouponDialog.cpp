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

enum Kind { WallThickness = 0, ChannelWidth, Overhang, Resolution };

struct Remembered
{
    int    kind { 0 }, profile { 0 }, count { 6 };
    double first { 0.4 }, step { 0.2 }, wall { 1.5 }, height { 10. };
};
Remembered s_last;

} // namespace

TestCouponDialog::TestCouponDialog(wxWindow *parent)
    : ParamDialog(parent, _L("Generate Test Coupon"), _L("Place on bed"))
{
    m_kind = add_choice(_L("Coupon:"), { _L("Wall thickness: thinnest wall that prints"), _L("Channel width: narrowest channel that stays open"),
                                         _L("Overhang: steepest overhang without support"), _L("Resolution: smallest pin and hole") }, s_last.kind);
    m_profile = add_choice(_L("Channel shape:"), { _L("Round"), _L("Square") }, s_last.profile);
    m_first   = add_value(_L("First feature"), _L("mm or degrees"), 0.05, 90., s_last.first, 0.1);
    m_step    = add_value(_L("Change per feature"), _L("mm or degrees"), -20., 20., s_last.step, 0.05);
    m_count   = add_count(_L("Number of features"), 1, 20, s_last.count);
    m_wall    = add_value(_L("Wall around channels"), _L("mm"), 0.2, 10., s_last.wall, 0.1);
    m_height  = add_value(_L("Height or length"), _L("mm"), 1., 60., s_last.height, 1., 1);
    add_note(_L("Wall thickness and channel width are in mm and grow by the change per feature. Overhang angles are measured from the vertical "
                "(0 is a wall, 90 is a bridge). For the resolution coupon the diameters get smaller by the change per feature."));
    finish_layout();
}

bool TestCouponDialog::place()
{
    s_last = { m_kind->GetSelection(), m_profile->GetSelection(), m_count->GetValue(), m_first->GetValue(), m_step->GetValue(), m_wall->GetValue(), m_height->GetValue() };

    Coupon coupon;
    std::string name;
    switch (s_last.kind) {
    case WallThickness: {
        WallThicknessCouponParams p;
        p.first_thickness = s_last.first;
        p.step            = s_last.step;
        p.count           = s_last.count;
        p.wall_height     = s_last.height;
        coupon = make_wall_thickness_coupon(p);
        name   = "Wall thickness coupon";
        break;
    }
    case ChannelWidth: {
        ChannelCouponParams p;
        p.profile     = s_last.profile == 0 ? ChannelProfile::Round : ChannelProfile::Square;
        p.first_width = s_last.first;
        p.step        = s_last.step;
        p.count       = s_last.count;
        p.wall        = s_last.wall;
        p.length      = s_last.height;
        coupon = make_channel_coupon(p);
        name   = "Channel width coupon";
        break;
    }
    case Overhang: {
        OverhangCouponParams p;
        p.first_angle = s_last.first;
        p.step        = s_last.step;
        p.count       = s_last.count;
        coupon = make_overhang_coupon(p);
        name   = "Overhang coupon";
        break;
    }
    default: {
        ResolutionCouponParams p;
        p.first_diameter = s_last.first;
        p.step           = s_last.step;
        p.count          = s_last.count;
        coupon = make_resolution_coupon(p);
        name   = "Resolution coupon";
        break;
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
