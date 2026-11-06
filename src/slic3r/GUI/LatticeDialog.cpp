#include "LatticeDialog.hpp"

#include <wx/checkbox.h>
#include <wx/choice.h>
#include <wx/spinctrl.h>

#include "GUI_App.hpp"
#include "I18N.hpp"
#include "Plater.hpp"
#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/TPMS.hpp"

namespace Slic3r {
namespace GUI {

namespace {

struct Remembered
{
    int    type { 0 }, axis { 2 };
    double x { 20. }, y { 20. }, z { 20. }, cell { 5. }, density { 30. }, density_end { 50. };
    bool   graded { false };
};
Remembered s_last;

} // namespace

LatticeDialog::LatticeDialog(wxWindow *parent)
    : ParamDialog(parent, _L("Generate Lattice"), _L("Place on bed"))
{
    m_type        = add_choice(_L("Lattice:"), { _L("Schwarz P (straight channels in three directions)"), _L("Diamond (branching channels)") }, s_last.type);
    m_x           = add_value(_L("Width (X)"), _L("mm"), 5., 200., s_last.x, 1., 1);
    m_y           = add_value(_L("Depth (Y)"), _L("mm"), 5., 200., s_last.y, 1., 1);
    m_z           = add_value(_L("Height (Z)"), _L("mm"), 5., 200., s_last.z, 1., 1);
    m_cell        = add_value(_L("Length of one repeat"), _L("mm"), 1., 50., s_last.cell, 0.5, 1);
    m_density     = add_value(_L("Solid share of the volume"), _L("%"), 2., 98., s_last.density, 1., 1);
    m_graded      = add_check(_L("Change the density along an axis"), s_last.graded);
    m_density_end = add_value(_L("Solid share at the far end"), _L("%"), 2., 98., s_last.density_end, 1., 1);
    m_axis        = add_choice(_L("Along:"), { _L("X"), _L("Y"), _L("Z") }, s_last.axis);
    add_note(_L("The share is exact when each side is a whole number of repeats. Large blocks with small repeats take a while to compute."));
    finish_layout();
}

bool LatticeDialog::place()
{
    s_last = { m_type->GetSelection(), m_axis->GetSelection(), m_x->GetValue(), m_y->GetValue(), m_z->GetValue(), m_cell->GetValue(),
               m_density->GetValue(), m_density_end->GetValue(), m_graded->GetValue() };

    tpms::Params params;
    params.type        = s_last.type == 0 ? tpms::Type::SchwarzP : tpms::Type::Diamond;
    params.size        = Vec3d(s_last.x, s_last.y, s_last.z);
    params.cell        = s_last.cell;
    params.density     = s_last.density / 100.;
    params.density_end = s_last.graded ? s_last.density_end / 100. : -1.;
    params.axis        = s_last.axis;

    const DynamicPrintConfig &cfg = wxGetApp().preset_bundle->printers.get_edited_preset().config;
    const BoundingBoxf bed(cfg.option<ConfigOptionPoints>("bed_shape")->values);
    if (bed.defined && (params.size.x() > bed.size().x() || params.size.y() > bed.size().y())) {
        set_status(wxString::Format(_L("The block is %.1f x %.1f mm and does not fit the %.1f x %.1f mm bed."),
            params.size.x(), params.size.y(), bed.size().x(), bed.size().y()));
        return false;
    }

    std::string error;
    indexed_triangle_set mesh = tpms::make_lattice(params, &error);
    if (mesh.indices.empty()) {
        set_status(wxString::FromUTF8(error));
        return false;
    }

    Model model;
    ModelObject *object = model.add_object();
    object->name = params.type == tpms::Type::SchwarzP ? "Schwarz P lattice" : "Diamond lattice";
    object->add_volume(TriangleMesh(std::move(mesh)))->name = "Lattice";
    object->center_around_origin();
    object->add_instance();

    Plater *plater = wxGetApp().plater();
    Plater::TakeSnapshot snapshot(plater, _L("Add lattice"));
    const std::vector<size_t> added = plater->add_model_objects(model.objects);
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
