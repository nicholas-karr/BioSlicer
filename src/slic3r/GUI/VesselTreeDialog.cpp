#include "VesselTreeDialog.hpp"

#include <algorithm>
#include <string>

#include <wx/button.h>
#include <wx/choice.h>
#include <wx/sizer.h>
#include <wx/spinctrl.h>
#include <wx/stattext.h>

#include "GLCanvas3D.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "Plater.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/TriangleMesh.hpp"

namespace Slic3r {
namespace GUI {

namespace {

// Settings of the last use in this session.
vessel::VesselSpec s_spec;

// The live preview is drawn with fewer sides and solid so that it stays responsive.
constexpr int PREVIEW_SIDES = 12;

wxSpinCtrlDouble *make_spin(wxWindow *parent, double min, double max, double value, double inc, int digits)
{
    auto *c = new wxSpinCtrlDouble(parent, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize,
                                   wxSP_ARROW_KEYS, min, max, value, inc);
    c->SetDigits(digits);
    return c;
}

} // namespace

VesselTreeDialog::VesselTreeDialog(wxWindow *parent, int edit_object_idx)
    : wxDialog(parent, wxID_ANY, edit_object_idx >= 0 ? _L("Edit Vessel Tree") : _L("Vessel Tree"),
               wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
    , m_edit_object_idx(edit_object_idx)
{
    SetFont(wxGetApp().normal_font());
    Plater *plater = wxGetApp().plater();

    if (m_edit_object_idx >= 0) {
        // Take the object out of the scene; the preview takes its place. Cancel undoes this.
        const ModelObject *mo = plater->model().objects[m_edit_object_idx];
        vessel::VesselSpec saved;
        if (vessel::spec_from_text(mo->vessel_tree_spec, saved))
            s_spec = saved;
        Plater::TakeSnapshot snapshot(plater, _L("Edit vessel tree"));
        Plater::SuppressSnapshots suppress(plater);
        plater->remove(size_t(m_edit_object_idx));
    }

    auto *outer = new wxBoxSizer(wxVERTICAL);
    auto *grid  = new wxFlexGridSizer(2, 8, 0);
    grid->AddGrowableCol(1);

    wxStaticText *last_label = nullptr;
    auto add_row = [&](const wxString &label, wxWindow *ctrl) {
        last_label = new wxStaticText(this, wxID_ANY, label);
        grid->Add(last_label, 0, wxALIGN_CENTER_VERTICAL | wxALL, 4);
        grid->Add(ctrl, 1, wxEXPAND | wxALL, 4);
        return last_label;
    };

    m_root_radius = make_spin(this, 0.05, 50., 2., 0.1, 2);
    add_row(_L("Root vessel radius (mm):"), m_root_radius);
    m_root_length = make_spin(this, 0.5, 500., 20., 1., 1);
    add_row(_L("Root vessel length (mm):"), m_root_length);
    m_levels = new wxSpinCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, 0, 8, 3);
    add_row(_L("Number of branching levels:"), m_levels);
    m_length_ratio = make_spin(this, 0.1, 2., 0.8, 0.05, 2);
    add_row(_L("Child length / parent length:"), m_length_ratio);

    m_radius_rule = new wxChoice(this, wxID_ANY);
    m_radius_rule->Append(_L("Murray's law (recommended)"));
    m_radius_rule->Append(_L("Fixed ratio"));
    add_row(_L("Radius of child vessels:"), m_radius_rule);
    m_flow_split = make_spin(this, 0.05, 0.95, 0.5, 0.05, 2);
    m_flow_split_label = add_row(_L("Flow share of the first child (0.5 = equal):"), m_flow_split);
    m_radius_ratio = make_spin(this, 0.1, 1., 0.8, 0.05, 2);
    m_radius_ratio_label = add_row(_L("Child radius / parent radius:"), m_radius_ratio);

    m_angle_rule = new wxChoice(this, wxID_ANY);
    m_angle_rule->Append(_L("Murray optimal angles (recommended)"));
    m_angle_rule->Append(_L("Fixed angle"));
    add_row(_L("Branching angles:"), m_angle_rule);
    m_total_angle = make_spin(this, 5., 170., 75., 5., 1);
    m_total_angle_label = add_row(_L("Angle between the two children (degrees):"), m_total_angle);

    m_angle_jitter = make_spin(this, 0., 45., 0., 1., 1);
    add_row(_L("Random angle variation (+- degrees):"), m_angle_jitter);
    m_length_jitter = make_spin(this, 0., 0.9, 0., 0.05, 2);
    add_row(_L("Random length variation (+- fraction):"), m_length_jitter);
    m_min_radius = make_spin(this, 0., 20., 0., 0.05, 2);
    add_row(_L("Stop below this radius (mm, 0 = no limit):"), m_min_radius);
    m_seed = new wxSpinCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, 0, 2000000000, 1);
    add_row(_L("Random seed (same seed = same tree):"), m_seed);

    m_wall = make_spin(this, 0., 20., 0., 0.05, 2);
    add_row(_L("Wall thickness (mm, 0 = solid):"), m_wall);
    m_sides = new wxSpinCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, 8, 128, 32);
    add_row(_L("Detail (corners per cross section):"), m_sides);

    m_bounds = new wxChoice(this, wxID_ANY);
    m_bounds->Append(_L("Nothing"));
    m_bounds_names.emplace_back();
    for (size_t i = 0; i < plater->model().objects.size(); ++i) {
        if (int(i) == m_edit_object_idx || plater->model().objects[i]->name.empty())
            continue;
        m_bounds->Append(from_u8(plater->model().objects[i]->name));
        m_bounds_names.push_back(plater->model().objects[i]->name);
    }
    m_bounds->SetSelection(0);
    add_row(_L("Keep the vessels inside this object:"), m_bounds);
    m_root_x = make_spin(this, -2000., 2000., 0., 1., 1);
    m_root_x_label = add_row(_L("Root start X on the bed (mm):"), m_root_x);
    m_root_y = make_spin(this, -2000., 2000., 0., 1., 1);
    m_root_y_label = add_row(_L("Root start Y on the bed (mm):"), m_root_y);
    m_root_z = make_spin(this, -2000., 2000., 0., 1., 1);
    m_root_z_label = add_row(_L("Root start height Z (mm):"), m_root_z);

    outer->Add(grid, 0, wxEXPAND | wxALL, 8);
    m_status = new wxStaticText(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxST_NO_AUTORESIZE);
    outer->Add(m_status, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 10);

    m_btn_place      = new wxButton(this, wxID_OK, m_edit_object_idx >= 0 ? _L("Apply") : _L("Add to Bed"));
    auto *btn_cancel = new wxButton(this, wxID_CANCEL, _L("Cancel"));
    auto *btn_row    = new wxBoxSizer(wxHORIZONTAL);
    btn_row->AddStretchSpacer();
    btn_row->Add(m_btn_place, 0, wxRIGHT, 6);
    btn_row->Add(btn_cancel, 0);
    outer->Add(btn_row, 0, wxEXPAND | wxALL, 10);

    SetSizerAndFit(outer);
    SetMinSize(wxSize(480, -1));

    // Dock against the left edge of the application window so the live preview stays visible.
    if (wxWindow *top = wxGetTopLevelParent(GetParent())) {
        const wxRect r = top->GetScreenRect();
        Move(r.x + 8, r.y + std::max(0, (r.height - GetSize().y) / 2));
    }

    auto on_change = [this](auto &) { refresh_controls(); rebuild_preview(); };
    for (wxSpinCtrlDouble *c : {m_root_radius, m_root_length, m_length_ratio, m_flow_split, m_radius_ratio, m_total_angle, m_angle_jitter,
                                m_length_jitter, m_min_radius, m_wall, m_root_x, m_root_y, m_root_z})
        c->Bind(wxEVT_SPINCTRLDOUBLE, on_change);
    for (wxSpinCtrl *c : {m_levels, m_seed, m_sides})
        c->Bind(wxEVT_SPINCTRL, on_change);
    m_radius_rule->Bind(wxEVT_CHOICE, on_change);
    m_angle_rule->Bind(wxEVT_CHOICE, on_change);
    m_bounds->Bind(wxEVT_CHOICE, [this](wxCommandEvent &) {
        // Start the root at the bottom centre of the chosen object.
        indexed_triangle_set its;
        if (bounds_mesh(m_bounds_names[m_bounds->GetSelection()], its) && !its.vertices.empty()) {
            BoundingBoxf3 bb;
            for (const Vec3f &v : its.vertices)
                bb.merge(v.cast<double>());
            m_root_x->SetValue(bb.center().x());
            m_root_y->SetValue(bb.center().y());
            m_root_z->SetValue(bb.min.z() + 1.);
        }
        refresh_controls();
        rebuild_preview();
    });
    m_btn_place->Bind(wxEVT_BUTTON, &VesselTreeDialog::on_place, this);
    btn_cancel->Bind(wxEVT_BUTTON, &VesselTreeDialog::on_cancel, this);

    write_controls(s_spec);
    refresh_controls();
    rebuild_preview();
}

VesselTreeDialog::~VesselTreeDialog()
{
    if (!m_finished)
        remove_temp_objects();
}

void VesselTreeDialog::refresh_controls()
{
    const bool murray   = m_radius_rule->GetSelection() == 0;
    const bool fixed_angle = m_angle_rule->GetSelection() == 1;
    const bool bounded  = m_bounds->GetSelection() > 0;
    m_flow_split_label->Show(murray);
    m_flow_split->Show(murray);
    m_radius_ratio_label->Show(!murray);
    m_radius_ratio->Show(!murray);
    m_total_angle_label->Show(fixed_angle);
    m_total_angle->Show(fixed_angle);
    for (wxWindow *w : std::initializer_list<wxWindow *>{m_root_x_label, m_root_x, m_root_y_label, m_root_y, m_root_z_label, m_root_z})
        w->Show(bounded);
    Layout();
    Fit();
}

vessel::VesselSpec VesselTreeDialog::read_controls() const
{
    vessel::VesselSpec s;
    s.tree.root_radius = m_root_radius->GetValue();
    s.tree.root_length = m_root_length->GetValue();
    s.tree.depth       = m_levels->GetValue();
    s.tree.length_ratio = m_length_ratio->GetValue();
    s.tree.rule.radius_rule  = m_radius_rule->GetSelection() == 0 ? vessel::RadiusRule::Murray : vessel::RadiusRule::FixedRatio;
    s.tree.rule.flow_split   = m_flow_split->GetValue();
    s.tree.rule.radius_ratio = m_radius_ratio->GetValue();
    s.tree.rule.angle_rule   = m_angle_rule->GetSelection() == 0 ? vessel::AngleRule::MurrayOptimal : vessel::AngleRule::Fixed;
    s.tree.rule.total_angle  = m_total_angle->GetValue();
    s.tree.angle_jitter  = m_angle_jitter->GetValue();
    s.tree.length_jitter = m_length_jitter->GetValue();
    s.tree.min_radius    = m_min_radius->GetValue();
    s.tree.seed          = uint32_t(m_seed->GetValue());
    s.mesh.wall_thickness = m_wall->GetValue();
    s.mesh.sides          = m_sides->GetValue();
    const int bounds_sel  = m_bounds->GetSelection();
    if (bounds_sel > 0) {
        s.bounds_object      = m_bounds_names[bounds_sel];
        s.tree.root_position = Vec3d(m_root_x->GetValue(), m_root_y->GetValue(), m_root_z->GetValue());
    }
    return s;
}

void VesselTreeDialog::write_controls(const vessel::VesselSpec &s)
{
    m_root_radius->SetValue(s.tree.root_radius);
    m_root_length->SetValue(s.tree.root_length);
    m_levels->SetValue(s.tree.depth);
    m_length_ratio->SetValue(s.tree.length_ratio);
    m_radius_rule->SetSelection(s.tree.rule.radius_rule == vessel::RadiusRule::Murray ? 0 : 1);
    m_flow_split->SetValue(s.tree.rule.flow_split);
    m_radius_ratio->SetValue(s.tree.rule.radius_ratio);
    m_angle_rule->SetSelection(s.tree.rule.angle_rule == vessel::AngleRule::MurrayOptimal ? 0 : 1);
    m_total_angle->SetValue(s.tree.rule.total_angle);
    m_angle_jitter->SetValue(s.tree.angle_jitter);
    m_length_jitter->SetValue(s.tree.length_jitter);
    m_min_radius->SetValue(s.tree.min_radius);
    m_seed->SetValue(int(s.tree.seed));
    m_wall->SetValue(s.mesh.wall_thickness);
    m_sides->SetValue(s.mesh.sides);
    m_bounds->SetSelection(0);
    for (size_t i = 1; i < m_bounds_names.size(); ++i)
        if (m_bounds_names[i] == s.bounds_object && !s.bounds_object.empty()) {
            m_bounds->SetSelection(int(i));
            m_root_x->SetValue(s.tree.root_position.x());
            m_root_y->SetValue(s.tree.root_position.y());
            m_root_z->SetValue(s.tree.root_position.z());
        }
}

bool VesselTreeDialog::bounds_mesh(const std::string &object_name, indexed_triangle_set &out) const
{
    if (object_name.empty())
        return false;
    for (const ModelObject *mo : wxGetApp().plater()->model().objects)
        if (mo->name == object_name && !mo->instances.empty()) {
            TriangleMesh mesh = mo->raw_mesh();
            mesh.transform(mo->instances.front()->get_transformation().get_matrix());
            out = std::move(mesh.its);
            return true;
        }
    return false;
}

void VesselTreeDialog::remove_temp_objects()
{
    if (m_temp_obj_idxs.empty())
        return;
    std::vector<size_t> sorted = m_temp_obj_idxs;
    std::sort(sorted.begin(), sorted.end(), std::greater<size_t>());
    for (size_t idx : sorted)
        wxGetApp().plater()->remove(idx);
    m_temp_obj_idxs.clear();
}

namespace {

// Builds the object for a spec. Throws Slic3r::Exception with a plain-language message if the settings do not work.
// The object keeps the spec so that it can be edited and regenerated later.
std::unique_ptr<Model> build_model(const vessel::VesselSpec &spec, const indexed_triangle_set *bounds, vessel::Skeleton &skeleton)
{
    skeleton = vessel::generate_tree(spec.tree, bounds);
    indexed_triangle_set its = vessel::make_tree_mesh(skeleton, spec.mesh);

    auto model = std::make_unique<Model>();
    ModelObject *obj = model->add_object();
    obj->name = "Vessel tree";
    ModelVolume *vol = obj->add_volume(TriangleMesh(std::move(its)));
    vol->name = "Vessel";
    obj->vessel_tree_spec = vessel::spec_to_text(spec);
    if (bounds != nullptr) {
        // The mesh is already in bed coordinates: keep it where it is.
        obj->center_around_origin();
        obj->add_instance()->set_offset(-obj->origin_translation);
    }
    return model;
}

} // namespace

void VesselTreeDialog::rebuild_preview()
{
    s_spec = read_controls();
    vessel::VesselSpec preview = s_spec;
    preview.mesh.sides = std::min(preview.mesh.sides, PREVIEW_SIDES);
    preview.mesh.wall_thickness = 0.;

    Plater::SuppressSnapshots suppress(wxGetApp().plater());
    remove_temp_objects();

    try {
        indexed_triangle_set bounds;
        const bool bounded = bounds_mesh(preview.bounds_object, bounds);
        vessel::Skeleton skeleton;
        std::unique_ptr<Model> model = build_model(preview, bounded ? &bounds : nullptr, skeleton);
        m_temp_obj_idxs = wxGetApp().plater()->add_model_objects(model->objects);

        wxString text = wxString::Format(_L("Preview: %d vessels, %d ends, volume of material about %.0f mm3"),
                                         int(skeleton.branches.size()), int(skeleton.leaf_count()),
                                         vessel::mesh_volume(model->objects.front()->volumes.front()->mesh().its));
        if (skeleton.clipped_by_bounds > 0)
            text += wxString::Format(_L(". %d vessels were shortened to stay inside the object"), int(skeleton.clipped_by_bounds));
        if (s_spec.mesh.wall_thickness > 0.)
            text += _L(". The preview is solid; the hollow wall is made when the tree is added");
        m_status->SetLabel(text);
    } catch (const std::exception &e) {
        m_status->SetLabel(from_u8(e.what()));
    }
    Layout();
    wxGetApp().plater()->canvas3D()->force_repaint();
}

void VesselTreeDialog::on_place(wxCommandEvent &)
{
    s_spec = read_controls();
    Plater *plater = wxGetApp().plater();
    try {
        indexed_triangle_set bounds;
        const bool bounded = bounds_mesh(s_spec.bounds_object, bounds);
        vessel::Skeleton skeleton;
        // Build before touching the scene so that a failure leaves the preview as it was.
        std::unique_ptr<Model> model = build_model(s_spec, bounded ? &bounds : nullptr, skeleton);
        {
            Plater::SuppressSnapshots suppress(plater);
            remove_temp_objects();
        }
        {
            // The scene is back to its state before the dialog, so undo returns exactly to it.
            Plater::TakeSnapshot snapshot(plater, m_edit_object_idx >= 0 ? _L("Edit vessel tree") : _L("Add vessel tree"));
            plater->add_model_objects(model->objects);
        }
        m_finished = true;
        EndModal(wxID_OK);
    } catch (const std::exception &e) {
        m_status->SetLabel(from_u8(e.what()));
        Layout();
    }
}

void VesselTreeDialog::on_cancel(wxCommandEvent &)
{
    {
        Plater::SuppressSnapshots suppress(wxGetApp().plater());
        remove_temp_objects();
    }
    m_finished = true;
    // When editing, the original object was taken out of the scene at the start: bring it back.
    if (m_edit_object_idx >= 0)
        wxGetApp().plater()->undo();
    EndModal(wxID_CANCEL);
}

} // namespace GUI
} // namespace Slic3r
