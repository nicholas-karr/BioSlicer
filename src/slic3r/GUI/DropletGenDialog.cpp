#include "DropletGenDialog.hpp"

#include <algorithm>
#include <memory>

#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/choice.h>
#include <wx/sizer.h>
#include <wx/spinctrl.h>
#include <wx/stattext.h>

#include "GLCanvas3D.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "Plater.hpp"
#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/DropletPattern.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/TriangleMesh.hpp"

namespace Slic3r {
namespace GUI {

namespace {

constexpr unsigned all_modes = 7;
constexpr unsigned grid_only = 1;
constexpr unsigned line_only = 2;
constexpr unsigned wells_only = 4;

// The values the dialog was left with, so the next experiment starts from them.
struct Remembered
{
    int    mode { 0 };
    double z { 0. }, volume { 50. }, volume_row { 0. }, volume_col { 0. };
    int    count { 1 };
    double pitch_x { 2. }, pitch_y { 2. };
    int    cols { 4 }, rows { 3 };
    int    line_count { 5 };
    double end_x { 20. }, end_y { 0. };
    double inner_x { 6. }, inner_y { 6. }, wall { 0.8 }, wall_height { 3. }, floor { 0.4 };
    int    per_well { 1 };
    int    shape { 0 };
    bool   serpentine { true }, center { true }, dip { false };
};
Remembered s_last;

std::string channel_name(const ConfigOptionStrings *labels, size_t i)
{
    if (labels != nullptr && i < labels->values.size() && ! labels->values[i].empty())
        return labels->values[i];
    return "Extruder " + std::to_string(i + 1);
}

} // namespace

wxSpinCtrlDouble *DropletGenDialog::add_double(const wxString &label, unsigned modes, double min, double max, double value, double inc, int digits)
{
    auto *ctrl = new wxSpinCtrlDouble(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, min, max, value, inc);
    ctrl->SetDigits(digits);
    auto *text = new wxStaticText(this, wxID_ANY, label);
    m_grid->Add(text, 0, wxALIGN_CENTER_VERTICAL | wxALL, 4);
    m_grid->Add(ctrl, 1, wxEXPAND | wxALL, 4);
    m_rows.push_back({ text, ctrl, modes });
    return ctrl;
}

wxSpinCtrl *DropletGenDialog::add_int(const wxString &label, unsigned modes, int min, int max, int value)
{
    auto *ctrl = new wxSpinCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, min, max, value);
    auto *text = new wxStaticText(this, wxID_ANY, label);
    m_grid->Add(text, 0, wxALIGN_CENTER_VERTICAL | wxALL, 4);
    m_grid->Add(ctrl, 1, wxEXPAND | wxALL, 4);
    m_rows.push_back({ text, ctrl, modes });
    return ctrl;
}

DropletGenDialog::DropletGenDialog(wxWindow *parent)
    : wxDialog(parent, wxID_ANY, _L("Generate Droplet Series"), wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
{
    SetFont(wxGetApp().normal_font());
    const DynamicPrintConfig &cfg = wxGetApp().preset_bundle->printers.get_edited_preset().config;
    const auto *types  = cfg.option<ConfigOptionStrings>("channel_type");
    const auto *labels = cfg.option<ConfigOptionStrings>("channel_label");
    const auto *sla    = cfg.option<ConfigOptionBools>("sla_material_extruder");
    const size_t n_ext = cfg.option<ConfigOptionFloats>("nozzle_diameter")->values.size();

    wxArrayString droplet_names, wall_names;
    for (size_t i = 0; i < n_ext; ++i) {
        const bool is_droplet = types != nullptr && i < types->values.size() && types->values[i] == "droplet";
        if (is_droplet) {
            m_droplet_extruders.push_back(int(i) + 1);
            droplet_names.Add(wxString::FromUTF8(channel_name(labels, i)));
        }
    }
    // The wall is cured from SLA material: those come first, the other extruders follow for a printer without one.
    for (int pass = 0; pass < 2; ++pass)
        for (size_t i = 0; i < n_ext; ++i) {
            const bool is_droplet = types != nullptr && i < types->values.size() && types->values[i] == "droplet";
            const bool is_sla     = sla != nullptr && i < sla->values.size() && sla->values[i] != 0;
            if (is_droplet || is_sla != (pass == 0))
                continue;
            m_wall_extruders.push_back(int(i) + 1);
            wall_names.Add(wxString::FromUTF8(channel_name(labels, i)));
        }

    auto *outer = new wxBoxSizer(wxVERTICAL);
    m_grid = new wxFlexGridSizer(2, 8, 0);
    m_grid->AddGrowableCol(1);

    auto add_choice = [&](const wxString &label, unsigned modes, const wxArrayString &items, int selection) {
        auto *ctrl = new wxChoice(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, items);
        ctrl->SetSelection(items.empty() ? wxNOT_FOUND : std::clamp(selection, 0, int(items.size()) - 1));
        auto *text = new wxStaticText(this, wxID_ANY, label);
        m_grid->Add(text, 0, wxALIGN_CENTER_VERTICAL | wxALL, 4);
        m_grid->Add(ctrl, 1, wxEXPAND | wxALL, 4);
        m_rows.push_back({ text, ctrl, modes });
        return ctrl;
    };
    auto add_check = [&](const wxString &label, unsigned modes, bool value) {
        auto *ctrl = new wxCheckBox(this, wxID_ANY, label);
        ctrl->SetValue(value);
        auto *text = new wxStaticText(this, wxID_ANY, wxEmptyString);
        m_grid->Add(text, 0, wxALL, 4);
        m_grid->Add(ctrl, 1, wxEXPAND | wxALL, 4);
        m_rows.push_back({ text, ctrl, modes });
        return ctrl;
    };

    wxArrayString modes;
    modes.Add(_L("Grid of droplets"));
    modes.Add(_L("Line of droplets"));
    modes.Add(_L("Array of wells with droplets"));
    m_mode = add_choice(_L("Series:"), all_modes, modes, s_last.mode);
    m_droplet_extruder = add_choice(_L("Droplet channel:"), all_modes, droplet_names, 0);

    m_volume     = add_double(_L("Volume (nL):"), all_modes, 0.1, 1e6, s_last.volume, 5., 2);
    m_count      = add_int(_L("Drops per point:"), all_modes, 1, 1000, s_last.count);
    m_z          = add_double(_L("Landing height (mm):"), grid_only | line_only, 0., 250., s_last.z, 0.1, 2);
    m_dip        = add_check(_L("Dip the nozzle to the landing height"), grid_only | line_only, s_last.dip);

    m_cols       = add_int(_L("Columns:"), grid_only | wells_only, 1, 48, s_last.cols);
    m_rows_spin  = add_int(_L("Rows:"), grid_only | wells_only, 1, 48, s_last.rows);
    m_pitch_x    = add_double(_L("Pitch X (mm):"), grid_only | wells_only, 0.1, 200., s_last.pitch_x, 0.5, 2);
    m_pitch_y    = add_double(_L("Pitch Y (mm):"), grid_only | wells_only, 0.1, 200., s_last.pitch_y, 0.5, 2);
    m_volume_row = add_double(_L("Volume step per row (nL):"), grid_only, -1e6, 1e6, s_last.volume_row, 5., 2);
    m_volume_col = add_double(_L("Volume step per column (nL):"), grid_only, -1e6, 1e6, s_last.volume_col, 5., 2);
    m_serpentine = add_check(_L("Run alternate rows backwards"), grid_only, s_last.serpentine);

    m_line_count = add_int(_L("Points:"), line_only, 1, 1000, s_last.line_count);
    m_end_x      = add_double(_L("Length X (mm):"), line_only, -250., 250., s_last.end_x, 1., 2);
    m_end_y      = add_double(_L("Length Y (mm):"), line_only, -250., 250., s_last.end_y, 1., 2);

    wxArrayString shapes;
    shapes.Add(_L("Round"));
    shapes.Add(_L("Rectangular"));
    m_shape      = add_choice(_L("Well shape:"), wells_only, shapes, s_last.shape);
    m_wall_extruder = add_choice(_L("Wall material:"), wells_only, wall_names, 0);
    m_inner_x    = add_double(_L("Well diameter or width (mm):"), wells_only, 0.5, 100., s_last.inner_x, 0.5, 2);
    m_inner_y    = add_double(_L("Well length, rectangular (mm):"), wells_only, 0.5, 100., s_last.inner_y, 0.5, 2);
    m_wall       = add_double(_L("Wall thickness (mm):"), wells_only, 0.1, 10., s_last.wall, 0.1, 2);
    m_wall_height = add_double(_L("Wall height (mm):"), wells_only, 0.1, 100., s_last.wall_height, 0.5, 2);
    m_floor      = add_double(_L("Floor thickness (mm, 0 = none):"), wells_only, 0., 20., s_last.floor, 0.1, 2);
    m_per_well   = add_int(_L("Droplets per well (n x n):"), wells_only, 1, 10, s_last.per_well);

    m_center = add_check(_L("Center on the bed"), grid_only | line_only, s_last.center);
    m_origin_x = add_double(_L("Start X (mm):"), grid_only | line_only, -500., 500., 0., 1., 2);
    m_origin_y = add_double(_L("Start Y (mm):"), grid_only | line_only, -500., 500., 0., 1., 2);

    outer->Add(m_grid, 0, wxEXPAND | wxALL, 8);

    m_status = new wxStaticText(this, wxID_ANY, wxEmptyString);
    outer->Add(m_status, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 10);

    auto *place  = new wxButton(this, wxID_OK, _L("Place"));
    auto *cancel = new wxButton(this, wxID_CANCEL, _L("Cancel"));
    auto *buttons = new wxBoxSizer(wxHORIZONTAL);
    buttons->AddStretchSpacer();
    buttons->Add(place, 0, wxRIGHT, 6);
    buttons->Add(cancel, 0);
    outer->Add(buttons, 0, wxEXPAND | wxALL, 10);
    SetSizerAndFit(outer);
    SetMinSize(wxSize(440, -1));

    // Dock against the left edge of the application window so the live preview stays visible.
    if (wxWindow *top = wxGetTopLevelParent(GetParent())) {
        const wxRect r = top->GetScreenRect();
        Move(r.x + 8, r.y + std::max(0, (r.height - GetSize().y) / 2));
    }

    if (m_droplet_extruders.empty()) {
        m_status->SetLabel(_L("The printer has no droplet channel. Set the channel type of an extruder to 'droplet' in the printer settings."));
        place->Disable();
    }
    place->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { on_place(); });
    m_mode->Bind(wxEVT_CHOICE, [this](wxCommandEvent &) { refresh_controls(); });
    m_center->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent &) {
        // Start the free position where the centered pattern was, so it is on the bed (the bed need not include 0,0).
        if (! m_center->GetValue()) {
            const DynamicPrintConfig &cfg = wxGetApp().preset_bundle->printers.get_edited_preset().config;
            const BoundingBoxf bed(cfg.option<ConfigOptionPoints>("bed_shape")->values);
            if (bed.defined) {
                const bool grid = m_mode->GetSelection() == Grid;
                const Vec2d extent = grid ? Vec2d((m_cols->GetValue() - 1) * m_pitch_x->GetValue(), (m_rows_spin->GetValue() - 1) * m_pitch_y->GetValue())
                                          : Vec2d(m_end_x->GetValue(), m_end_y->GetValue());
                const Vec2d origin = Vec2d(bed.center()) - extent * 0.5;
                m_origin_x->SetValue(origin.x());
                m_origin_y->SetValue(origin.y());
            }
        }
        refresh_controls();
    });
    refresh_controls();

    // Spin events are unreliable inside a modal dialog on GTK: watch the values instead.
    Bind(wxEVT_IDLE, [this](wxIdleEvent &) {
        if (signature() != m_signature)
            rebuild_preview();
    });
    Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent &evt) {
        clear_preview();
        evt.Skip();
    });
    rebuild_preview();
}

DropletGenDialog::~DropletGenDialog()
{
    if (! m_placed)
        clear_preview();
}

std::vector<double> DropletGenDialog::signature() const
{
    std::vector<double> v;
    for (const Row &row : m_rows) {
        if (auto *d = dynamic_cast<wxSpinCtrlDouble*>(row.control))
            v.push_back(d->GetValue());
        else if (auto *i = dynamic_cast<wxSpinCtrl*>(row.control))
            v.push_back(i->GetValue());
        else if (auto *c = dynamic_cast<wxChoice*>(row.control))
            v.push_back(c->GetSelection());
        else if (auto *b = dynamic_cast<wxCheckBox*>(row.control))
            v.push_back(b->GetValue());
    }
    return v;
}

void DropletGenDialog::clear_preview()
{
    Plater *plater = wxGetApp().plater();
    if (plater == nullptr)
        return;
    Plater::SuppressSnapshots suppress(plater);
    if (! m_preview_objects.empty()) {
        // Highest index first so the earlier ones stay valid.
        std::vector<size_t> sorted = m_preview_objects;
        std::sort(sorted.begin(), sorted.end(), std::greater<size_t>());
        for (size_t idx : sorted)
            plater->remove(idx);
        m_preview_objects.clear();
    }
    if (m_preview_from != size_t(-1)) {
        DropletSets &sets = plater->model().droplet_sets;
        if (m_preview_from < sets.size())
            sets.resize(m_preview_from);
        m_preview_from = size_t(-1);
        plater->canvas3D()->set_as_dirty();
        plater->canvas3D()->force_repaint();
    }
}

void DropletGenDialog::rebuild_preview()
{
    m_signature = signature();
    clear_preview();
    apply(true);
}

void DropletGenDialog::refresh_controls()
{
    const unsigned mode_bit = 1u << unsigned(std::max(0, m_mode->GetSelection()));
    const bool     round    = m_shape->GetSelection() == 0;
    for (const Row &row : m_rows) {
        const bool show = (row.modes & mode_bit) != 0;
        row.label->Show(show);
        row.control->Show(show);
    }
    // The start fields only matter when the pattern is not centered; the rectangular well length when it is not round.
    const bool free_start = ! m_center->GetValue();
    for (const Row &row : m_rows)
        if (row.control == m_origin_x || row.control == m_origin_y) {
            row.label->Show(free_start && (row.modes & mode_bit));
            row.control->Show(free_start && (row.modes & mode_bit));
        } else if (row.control == m_inner_y) {
            row.label->Show(! round && (mode_bit & wells_only));
            row.control->Show(! round && (mode_bit & wells_only));
        }
    Layout();
    Fit();
}

void DropletGenDialog::on_place()
{
    clear_preview();
    if (apply(false)) {
        m_placed = true;
        EndModal(wxID_OK);
    } else
        m_signature = signature();
}

bool DropletGenDialog::apply(bool preview)
{
    const int  mode   = m_mode->GetSelection();
    const int  sel    = m_droplet_extruder->GetSelection();
    if (sel == wxNOT_FOUND || sel >= int(m_droplet_extruders.size()))
        return false;
    const int  droplet_extruder = m_droplet_extruders[size_t(sel)];

    s_last = { mode, m_z->GetValue(), m_volume->GetValue(), m_volume_row->GetValue(), m_volume_col->GetValue(), m_count->GetValue(),
        m_pitch_x->GetValue(), m_pitch_y->GetValue(), m_cols->GetValue(), m_rows_spin->GetValue(), m_line_count->GetValue(),
        m_end_x->GetValue(), m_end_y->GetValue(), m_inner_x->GetValue(), m_inner_y->GetValue(), m_wall->GetValue(),
        m_wall_height->GetValue(), m_floor->GetValue(), m_per_well->GetValue(), m_shape->GetSelection(),
        m_serpentine->GetValue(), m_center->GetValue(), m_dip->GetValue() };

    const DynamicPrintConfig &cfg = wxGetApp().preset_bundle->printers.get_edited_preset().config;
    const BoundingBoxf bed(cfg.option<ConfigOptionPoints>("bed_shape")->values);
    const Vec2d bed_center = bed.defined ? Vec2d(bed.center()) : Vec2d::Zero();

    Plater *plater = wxGetApp().plater();
    if (mode == Wells) {
        WellArrayParams p;
        p.shape          = m_shape->GetSelection() == 0 ? WellShape::Round : WellShape::Rectangular;
        p.cols           = s_last.cols;
        p.rows           = s_last.rows;
        p.pitch_x        = s_last.pitch_x;
        p.pitch_y        = s_last.pitch_y;
        p.inner_x        = s_last.inner_x;
        p.inner_y        = p.shape == WellShape::Round ? s_last.inner_x : s_last.inner_y;
        p.wall_thickness = s_last.wall;
        p.wall_height    = s_last.wall_height;
        p.floor_thickness = s_last.floor;
        p.droplets_n     = s_last.per_well;
        p.volume_nl      = s_last.volume;
        p.count          = s_last.count;
        // Wells that touch would merge into one: the pitch has to leave room for two half walls.
        if (p.pitch_x < p.inner_x + p.wall_thickness || p.pitch_y < p.inner_y + p.wall_thickness) {
            m_status->SetLabel(_L("The pitch is smaller than a well with its wall. Increase the pitch or reduce the wells."));
            Layout();
            return false;
        }
        // The whole array, with its walls, has to fit the bed.
        const double width  = (p.cols - 1) * p.pitch_x + p.inner_x + 2. * p.wall_thickness;
        const double length = (p.rows - 1) * p.pitch_y + p.inner_y + 2. * p.wall_thickness;
        if (bed.defined && (width > bed.size().x() || length > bed.size().y())) {
            m_status->SetLabel(wxString::Format(_L("The array is %.1f x %.1f mm and does not fit the %.1f x %.1f mm bed. Use fewer or smaller wells, or a smaller pitch."),
                width, length, bed.size().x(), bed.size().y()));
            Layout();
            return false;
        }
        WellArray array = make_well_array(p);
        if (array.mesh.empty()) {
            m_status->SetLabel(_L("The wells have no wall."));
            return false;
        }
        Model model;
        ModelObject *object = model.add_object();
        object->name = "Wells " + std::to_string(p.cols) + "x" + std::to_string(p.rows);
        ModelVolume *volume = object->add_volume(TriangleMesh(std::move(array.mesh)));
        volume->name = "Wall";
        const int wall_sel = m_wall_extruder->GetSelection();
        if (wall_sel != wxNOT_FOUND && wall_sel < int(m_wall_extruders.size()))
            volume->config.set_key_value("extruder", new ConfigOptionInt(m_wall_extruders[size_t(wall_sel)]));
        for (DropletSet &set : array.droplets) {
            set.extruder = droplet_extruder;
            set.dip      = true; // into the well
            object->droplet_sets.emplace_back(std::move(set));
        }
        object->add_instance();
        std::unique_ptr<Plater::SuppressSnapshots> suppress;
        std::unique_ptr<Plater::TakeSnapshot>      snapshot;
        if (preview)
            suppress = std::make_unique<Plater::SuppressSnapshots>(plater);
        else
            snapshot = std::make_unique<Plater::TakeSnapshot>(plater, _L("Place wells and droplets"));
        const std::vector<size_t> added = plater->add_model_objects(model.objects);
        if (preview)
            m_preview_objects = added;
        // Placing alone leaves the array at the origin, which is off the bed: put it in the middle of the bed.
        for (const size_t idx : added)
            if (idx < plater->model().objects.size()) {
                ModelObject *placed = plater->model().objects[idx];
                for (ModelInstance *instance : placed->instances)
                    instance->set_offset(Vec3d(bed_center.x(), bed_center.y(), instance->get_offset().z()));
                plater->changed_object(int(idx));
            }
        m_status->SetLabel(preview ? _L("Preview. Place keeps it.") : wxString());
        Layout();
        return true;
    }

    DropletSet set;
    if (mode == Grid) {
        DropletGridParams p;
        p.cols = s_last.cols;
        p.rows = s_last.rows;
        p.pitch_x = s_last.pitch_x;
        p.pitch_y = s_last.pitch_y;
        p.volume_nl = s_last.volume;
        p.volume_step_row = s_last.volume_row;
        p.volume_step_col = s_last.volume_col;
        p.count = s_last.count;
        p.serpentine = s_last.serpentine;
        const Vec2d extent((p.cols - 1) * p.pitch_x, (p.rows - 1) * p.pitch_y);
        const Vec2d origin = s_last.center ? Vec2d(bed_center - extent * 0.5) : Vec2d(m_origin_x->GetValue(), m_origin_y->GetValue());
        if (bed.defined && (origin.x() < bed.min.x() || origin.y() < bed.min.y() || origin.x() + extent.x() > bed.max.x() || origin.y() + extent.y() > bed.max.y())) {
            m_status->SetLabel(wxString::Format(_L("The grid is %.1f x %.1f mm and does not fit the %.1f x %.1f mm bed."), extent.x(), extent.y(), bed.size().x(), bed.size().y()));
            Layout();
            return false;
        }
        p.origin = Vec3d(origin.x(), origin.y(), s_last.z);
        set = make_droplet_grid(p);
    } else {
        const Vec2d extent(s_last.end_x, s_last.end_y);
        const Vec2d origin = s_last.center ? Vec2d(bed_center - extent * 0.5) : Vec2d(m_origin_x->GetValue(), m_origin_y->GetValue());
        const Vec2d end = origin + extent;
        if (bed.defined && (std::min(origin.x(), end.x()) < bed.min.x() || std::min(origin.y(), end.y()) < bed.min.y() ||
                            std::max(origin.x(), end.x()) > bed.max.x() || std::max(origin.y(), end.y()) > bed.max.y())) {
            m_status->SetLabel(_L("The line does not fit the bed."));
            Layout();
            return false;
        }
        set = make_droplet_line(Vec3d(origin.x(), origin.y(), s_last.z), Vec3d(origin.x() + extent.x(), origin.y() + extent.y(), s_last.z),
                                s_last.line_count, s_last.volume, s_last.count);
    }
    set.extruder = droplet_extruder;
    set.dip      = s_last.dip;
    DropletSets sets;
    sets.emplace_back(std::move(set));
    if (preview) {
        m_preview_from = plater->model().droplet_sets.size();
        for (DropletSet &s : sets)
            plater->model().droplet_sets.emplace_back(std::move(s));
        plater->canvas3D()->set_as_dirty();
        plater->canvas3D()->force_repaint();
        m_status->SetLabel(_L("Preview. Place keeps it."));
        Layout();
    } else
        plater->add_droplet_sets(std::move(sets), _L("Place droplets"));
    return true;
}

} // namespace GUI
} // namespace Slic3r
