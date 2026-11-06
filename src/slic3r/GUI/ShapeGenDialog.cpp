#include "ShapeGenDialog.hpp"

#include <algorithm>
#include <cmath>
#include <vector>
#include <string>

#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/button.h>
#include <wx/choice.h>
#include <wx/spinctrl.h>

#include "GUI_App.hpp"
#include "Plater.hpp"
#include "GLCanvas3D.hpp"
#include "I18N.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/ShapeGen.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/MeshPrimitives.hpp"
#include "libslic3r/PresetBundle.hpp"

namespace Slic3r {
namespace GUI {

static ShapeGenParams s_params;
// Exact primitives: values in mm (counts for segment fields) per PrimitiveType
static std::vector<double> s_prim[int(PrimitiveType::Count)];

// Shape indices after Helix map to PrimitiveType in order.
static constexpr int FirstPrimitiveShape = 6;

static bool is_primitive_shape(int shape) { return shape >= FirstPrimitiveShape; }
static PrimitiveType primitive_of_shape(int shape) { return PrimitiveType(shape - FirstPrimitiveShape); }

// ---------------------------------------------------------------------------
// Build the full ModelObject into a Model
// ---------------------------------------------------------------------------

// Build interleaved [FFF1, SLA1, FFF2, SLA2, ...] extruder list from the
// current printer preset.  Falls back to {1, 5} if the config is unavailable.
static std::vector<int> make_extruder_list(int n_materials)
{
    std::vector<int> fff_exts, sla_exts;
    const DynamicPrintConfig& cfg =
        wxGetApp().preset_bundle->printers.get_edited_preset().config;
    const auto* sla_opt = cfg.option<ConfigOptionBools>("sla_material_extruder");
    int n_ext = (int)cfg.option<ConfigOptionFloats>("nozzle_diameter")->values.size();
    for (int i = 0; i < n_ext; ++i) {
        bool is_sla = sla_opt && i < (int)sla_opt->values.size() && sla_opt->values[i];
        (is_sla ? sla_exts : fff_exts).push_back(i + 1);
    }
    if (fff_exts.empty()) fff_exts.push_back(1);

    if (sla_exts.empty()) {
        // No SLA extruders — cycle through FFF extruders so each material has a distinct color.
        std::vector<int> result;
        result.reserve(n_materials);
        for (int i = 0; i < n_materials; ++i)
            result.push_back(fff_exts[i % (int)fff_exts.size()]);
        return result;
    }

    std::vector<int> result;
    result.reserve(n_materials);
    int fi = 0, si = 0;
    while ((int)result.size() < n_materials) {
        if (fi < (int)fff_exts.size()) result.push_back(fff_exts[fi++]);
        if ((int)result.size() < n_materials && si < (int)sla_exts.size())
            result.push_back(sla_exts[si++]);
        if (fi >= (int)fff_exts.size() && si >= (int)sla_exts.size()) { fi = 0; si = 0; }
    }
    return result;
}

static void build_into_model(Model& out_model, const ShapeGenParams& p)
{
    if (is_primitive_shape(p.shape)) {
        const PrimitiveType type = primitive_of_shape(p.shape);
        ModelObject* obj = out_model.add_object();
        obj->name = primitive_name(type);
        ModelVolume* vol = obj->add_volume(TriangleMesh(make_primitive(type, s_prim[int(type)])));
        vol->name = "Body";
        const std::vector<int> extruders = make_extruder_list(1);
        vol->config.set_key_value("extruder", new ConfigOptionInt(extruders.empty() ? 5 : extruders[0]));
        return;
    }

    auto meshes = generate_shape(p);
    const std::vector<int> extruders = make_extruder_list((int)meshes.size());
    const bool helix = p.shape == 5;

    ModelObject* obj = out_model.add_object();
    obj->name = helix ? "Helix Shape" : "Generated Shape";

    for (size_t m = 0; m < meshes.size(); ++m) {
        if (meshes[m].vertices.empty())
            continue;
        ModelVolume* vol = obj->add_volume(TriangleMesh(std::move(meshes[m])));
        if (helix)
            vol->name = m == 0 ? "Cylinder" : "Strand " + std::to_string(m);
        else
            vol->name = p.pattern == 0 ? "Body" : "Material " + std::to_string(m + 1);
        vol->config.set_key_value("extruder", new ConfigOptionInt(extruders[m]));
    }
}

// ---------------------------------------------------------------------------
// ShapeGenDialog
// ---------------------------------------------------------------------------

ShapeGenDialog::ShapeGenDialog(wxWindow* parent)
    : wxDialog(parent, wxID_ANY, _L("Generate SLA Shape"),
               wxDefaultPosition, wxDefaultSize,
               wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
{
    SetFont(wxGetApp().normal_font());

    auto* outer = new wxBoxSizer(wxVERTICAL);

    // Main grid — 0 vgap so hidden rows collapse fully; per-item wxALL,4 provides spacing.
    auto* grid = new wxFlexGridSizer(2, 8, 0);
    grid->AddGrowableCol(1);

    auto add_row = [&](const wxString& label, wxWindow* ctrl) {
        grid->Add(new wxStaticText(this, wxID_ANY, label),
                  0, wxALIGN_CENTER_VERTICAL | wxALL, 4);
        grid->Add(ctrl, 1, wxEXPAND | wxALL, 4);
    };

    // Always-visible rows
    wxArrayString shapes;
    for (const char* s : {"Cube", "Cylinder", "Cone", "Sphere", "Pyramid", "Helix"})
        shapes.Add(s);
    for (int i = 0; i < int(PrimitiveType::Count); ++i)
        shapes.Add(_(primitive_name(PrimitiveType(i))));
    for (int i = 0; i < int(PrimitiveType::Count); ++i)
        if (s_prim[i].empty())
            s_prim[i] = primitive_default_params(PrimitiveType(i));
    m_choice_shape = new wxChoice(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, shapes);
    m_choice_shape->SetSelection(s_params.shape);
    add_row(_L("Shape:"), m_choice_shape);

    m_spin_size = new wxSpinCtrlDouble(this, wxID_ANY, wxEmptyString,
        wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, 5.0, 200.0, s_params.size, 1.0);
    m_spin_size->SetDigits(1);
    m_size_label = new wxStaticText(this, wxID_ANY, _L("Size (mm):"));
    grid->Add(m_size_label, 0, wxALIGN_CENTER_VERTICAL | wxALL, 4);
    grid->Add(m_spin_size,  1, wxEXPAND | wxALL, 4);

    m_spin_sla_h = new wxSpinCtrlDouble(this, wxID_ANY, wxEmptyString,
        wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, 1.0, 200.0, s_params.sla_h, 1.0);
    m_spin_sla_h->SetDigits(1);
    m_height_label = new wxStaticText(this, wxID_ANY, _L("Height (mm):"));
    grid->Add(m_height_label, 0, wxALIGN_CENTER_VERTICAL | wxALL, 4);
    grid->Add(m_spin_sla_h,   1, wxEXPAND | wxALL, 4);

    // Exact primitive rows (shown for the shapes after Helix). Values are typed in mm.
    for (int i = 0; i < MaxPrimitiveParams; ++i) {
        m_prim_labels[i] = new wxStaticText(this, wxID_ANY, wxEmptyString);
        m_prim_spins[i]  = new wxSpinCtrlDouble(this, wxID_ANY, wxEmptyString,
            wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, 0., 1., 0., 1.);
        grid->Add(m_prim_labels[i], 0, wxALIGN_CENTER_VERTICAL | wxALL, 4);
        grid->Add(m_prim_spins[i],  1, wxEXPAND | wxALL, 4);
    }

    wxArrayString patterns;
    for (const char* p : {"Single", "Stripes", "Checkerboard", "Helix", "Honeycomb"})
        patterns.Add(p);
    m_choice_pattern = new wxChoice(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, patterns);
    m_choice_pattern->SetSelection(s_params.pattern);
    m_pattern_label = new wxStaticText(this, wxID_ANY, _L("Pattern:"));
    grid->Add(m_pattern_label, 0, wxALIGN_CENTER_VERTICAL | wxALL, 4);
    grid->Add(m_choice_pattern, 1, wxEXPAND | wxALL, 4);

    // Materials row (hidden for Single pattern)
    m_n_mat_label      = new wxStaticText(this, wxID_ANY, _L("Materials:"));
    m_spin_n_materials = new wxSpinCtrl(this, wxID_ANY, wxEmptyString,
        wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, 2, 8, s_params.n_materials);
    grid->Add(m_n_mat_label,      0, wxALIGN_CENTER_VERTICAL | wxALL, 4);
    grid->Add(m_spin_n_materials, 1, wxEXPAND | wxALL, 4);

    // Stripes row
    m_stripe_rows_label = new wxStaticText(this, wxID_ANY, _L("Stripes:"));
    m_spin_stripe_rows  = new wxSpinCtrl(this, wxID_ANY, wxEmptyString,
        wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, 2, 32, s_params.stripe_rows);
    grid->Add(m_stripe_rows_label, 0, wxALIGN_CENTER_VERTICAL | wxALL, 4);
    grid->Add(m_spin_stripe_rows,  1, wxEXPAND | wxALL, 4);

    // Checkerboard rows
    m_cb_col_label    = new wxStaticText(this, wxID_ANY, _L("Columns:"));
    m_spin_cb_columns = new wxSpinCtrl(this, wxID_ANY, wxEmptyString,
        wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, 4, 32, s_params.cb_columns);
    grid->Add(m_cb_col_label,    0, wxALIGN_CENTER_VERTICAL | wxALL, 4);
    grid->Add(m_spin_cb_columns, 1, wxEXPAND | wxALL, 4);

    m_cb_row_label = new wxStaticText(this, wxID_ANY, _L("Rows:"));
    m_spin_cb_rows = new wxSpinCtrl(this, wxID_ANY, wxEmptyString,
        wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, 2, 16, s_params.cb_rows);
    grid->Add(m_cb_row_label, 0, wxALIGN_CENTER_VERTICAL | wxALL, 4);
    grid->Add(m_spin_cb_rows, 1, wxEXPAND | wxALL, 4);

    // Helix rows
    m_hx_rev_label = new wxStaticText(this, wxID_ANY, _L("Revolutions:"));
    m_spin_hx_rev  = new wxSpinCtrlDouble(this, wxID_ANY, wxEmptyString,
        wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, 1.0, 8.0, s_params.hx_revolutions, 0.5);
    m_spin_hx_rev->SetDigits(1);
    grid->Add(m_hx_rev_label, 0, wxALIGN_CENTER_VERTICAL | wxALL, 4);
    grid->Add(m_spin_hx_rev,  1, wxEXPAND | wxALL, 4);

    m_hx_wid_label  = new wxStaticText(this, wxID_ANY, _L("Strand (%):"));
    m_spin_hx_width = new wxSpinCtrl(this, wxID_ANY, wxEmptyString,
        wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, 10, 90, s_params.hx_width);
    grid->Add(m_hx_wid_label,  0, wxALIGN_CENTER_VERTICAL | wxALL, 4);
    grid->Add(m_spin_hx_width, 1, wxEXPAND | wxALL, 4);

    // Honeycomb rows
    m_hc_sec_label    = new wxStaticText(this, wxID_ANY, _L("Cells across:"));
    m_spin_hc_sectors = new wxSpinCtrl(this, wxID_ANY, wxEmptyString,
        wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, 2, 20, s_params.hc_sectors);
    grid->Add(m_hc_sec_label,    0, wxALIGN_CENTER_VERTICAL | wxALL, 4);
    grid->Add(m_spin_hc_sectors, 1, wxEXPAND | wxALL, 4);

    m_hc_ban_label = new wxStaticText(this, wxID_ANY, _L("Wall (%):"));
    m_spin_hc_bands = new wxSpinCtrl(this, wxID_ANY, wxEmptyString,
        wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, 5, 50, s_params.hc_bands);
    grid->Add(m_hc_ban_label, 0, wxALIGN_CENTER_VERTICAL | wxALL, 4);
    grid->Add(m_spin_hc_bands, 1, wxEXPAND | wxALL, 4);

    // Helix shape rows (shown when shape == Helix, hidden otherwise)
    m_hs_turns_label = new wxStaticText(this, wxID_ANY, _L("Turns:"));
    m_spin_hs_turns  = new wxSpinCtrl(this, wxID_ANY, wxEmptyString,
        wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, 1, 10, s_params.hs_turns);
    grid->Add(m_hs_turns_label, 0, wxALIGN_CENTER_VERTICAL | wxALL, 4);
    grid->Add(m_spin_hs_turns,  1, wxEXPAND | wxALL, 4);

    m_hs_tube_label = new wxStaticText(this, wxID_ANY, _L("Tube (%):"));
    m_spin_hs_tube  = new wxSpinCtrl(this, wxID_ANY, wxEmptyString,
        wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, 5, 40, s_params.hs_tube_pct);
    grid->Add(m_hs_tube_label, 0, wxALIGN_CENTER_VERTICAL | wxALL, 4);
    grid->Add(m_spin_hs_tube,  1, wxEXPAND | wxALL, 4);

    m_hs_strands_label = new wxStaticText(this, wxID_ANY, _L("Strands:"));
    m_spin_hs_strands  = new wxSpinCtrl(this, wxID_ANY, wxEmptyString,
        wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, 1, 6, s_params.hs_strands);
    grid->Add(m_hs_strands_label, 0, wxALIGN_CENTER_VERTICAL | wxALL, 4);
    grid->Add(m_spin_hs_strands,  1, wxEXPAND | wxALL, 4);

    outer->Add(grid, 0, wxEXPAND | wxALL, 8);

    m_status_label = new wxStaticText(this, wxID_ANY, wxEmptyString,
        wxDefaultPosition, wxDefaultSize, wxST_NO_AUTORESIZE);
    outer->Add(m_status_label, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 10);

    m_btn_place      = new wxButton(this, wxID_OK,     _L("Place on Bed"));
    auto* btn_cancel = new wxButton(this, wxID_CANCEL, _L("Cancel"));
    auto* btn_row    = new wxBoxSizer(wxHORIZONTAL);
    btn_row->AddStretchSpacer();
    btn_row->Add(m_btn_place, 0, wxRIGHT, 6);
    btn_row->Add(btn_cancel,  0);
    outer->Add(btn_row, 0, wxEXPAND | wxALL, 10);

    SetSizerAndFit(outer);
    SetMinSize(wxSize(420, -1));

    // Dock against the left edge of the application window so the live preview stays visible.
    if (wxWindow *top = wxGetTopLevelParent(GetParent())) {
        const wxRect r = top->GetScreenRect();
        Move(r.x + 8, r.y + std::max(0, (r.height - GetSize().y) / 2));
    }

    // Event bindings
    m_choice_shape->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) {
        s_params.shape = m_choice_shape->GetSelection();
        refresh_controls();
        rebuild_preview();
    });
    m_choice_pattern->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) {
        s_params.pattern = m_choice_pattern->GetSelection();
        refresh_controls();
        rebuild_preview();
    });

    // Direct rebuild on spin events (no debounce — timer events are unreliable
    // inside a modal dialog's event loop on GTK).
    auto trigger = [this](auto&) { rebuild_preview(); };
    m_spin_size->Bind(wxEVT_SPINCTRLDOUBLE,    trigger);
    m_spin_sla_h->Bind(wxEVT_SPINCTRLDOUBLE,   trigger);
    m_spin_stripe_rows->Bind(wxEVT_SPINCTRL, trigger);
    m_spin_cb_columns->Bind(wxEVT_SPINCTRL,    trigger);
    m_spin_cb_rows->Bind(wxEVT_SPINCTRL,       trigger);
    m_spin_hx_rev->Bind(wxEVT_SPINCTRLDOUBLE,  trigger);
    m_spin_hx_width->Bind(wxEVT_SPINCTRL,      trigger);
    m_spin_hc_sectors->Bind(wxEVT_SPINCTRL,    trigger);
    m_spin_hc_bands->Bind(wxEVT_SPINCTRL,      trigger);
    m_spin_n_materials->Bind(wxEVT_SPINCTRL,   trigger);
    m_spin_hs_turns->Bind(wxEVT_SPINCTRL,      trigger);
    m_spin_hs_tube->Bind(wxEVT_SPINCTRL,       trigger);
    m_spin_hs_strands->Bind(wxEVT_SPINCTRL,    trigger);
    for (auto* spin : m_prim_spins)
        spin->Bind(wxEVT_SPINCTRLDOUBLE, trigger);

    m_btn_place->Bind(wxEVT_BUTTON,   &ShapeGenDialog::on_place,  this);
    btn_cancel->Bind(wxEVT_BUTTON,    &ShapeGenDialog::on_cancel, this);

    Bind(wxEVT_IDLE, [this](wxIdleEvent&) {
        if (m_spin_size->GetValue()           != s_params.size           ||
            m_spin_sla_h->GetValue()          != s_params.sla_h          ||
            m_spin_n_materials->GetValue()    != s_params.n_materials    ||
            m_spin_stripe_rows->GetValue()    != s_params.stripe_rows    ||
            m_spin_cb_columns->GetValue()     != s_params.cb_columns     ||
            m_spin_cb_rows->GetValue()        != s_params.cb_rows        ||
            m_spin_hx_rev->GetValue()         != s_params.hx_revolutions ||
            m_spin_hx_width->GetValue()       != s_params.hx_width       ||
            m_spin_hc_sectors->GetValue()     != s_params.hc_sectors     ||
            m_spin_hc_bands->GetValue()       != s_params.hc_bands       ||
            m_spin_hs_turns->GetValue()       != s_params.hs_turns       ||
            m_spin_hs_tube->GetValue()        != s_params.hs_tube_pct    ||
            m_spin_hs_strands->GetValue()     != s_params.hs_strands     ||
            m_choice_shape->GetSelection()    != s_params.shape          ||
            primitive_controls_changed()                                 ||
            m_choice_pattern->GetSelection()  != s_params.pattern)
            rebuild_preview();
    });

    refresh_controls();
    rebuild_preview();
}

ShapeGenDialog::~ShapeGenDialog()
{
    if (!m_placed)
        remove_temp_object();
}

void ShapeGenDialog::refresh_controls()
{
    const bool primitive = is_primitive_shape(s_params.shape);
    const int pat     = primitive ? 0 : s_params.pattern;
    const bool helix  = (s_params.shape == 5) || primitive;

    // When Helix shape is selected, the shape itself defines the material layout;
    // pattern and material controls are not applicable.
    m_pattern_label->Show(!helix);
    m_choice_pattern->Show(!helix);

    m_n_mat_label->Show(!helix && pat != 0);
    m_spin_n_materials->Show(!helix && pat != 0);

    m_stripe_rows_label->Show(!helix && pat == 1);
    m_spin_stripe_rows->Show(!helix && pat == 1);

    m_cb_col_label->Show(!helix && pat == 2);
    m_spin_cb_columns->Show(!helix && pat == 2);
    m_cb_row_label->Show(!helix && pat == 2);
    m_spin_cb_rows->Show(!helix && pat == 2);

    m_hx_rev_label->Show(!helix && pat == 3);
    m_spin_hx_rev->Show(!helix && pat == 3);
    m_hx_wid_label->Show(!helix && pat == 3);
    m_spin_hx_width->Show(!helix && pat == 3);

    m_hc_sec_label->Show(!helix && pat == 4);
    m_spin_hc_sectors->Show(!helix && pat == 4);
    m_hc_ban_label->Show(!helix && pat == 4);
    m_spin_hc_bands->Show(!helix && pat == 4);

    // Helix shape parameters
    const bool helix_only = (s_params.shape == 5);
    m_hs_turns_label->Show(helix_only);
    m_spin_hs_turns->Show(helix_only);
    m_hs_tube_label->Show(helix_only);
    m_spin_hs_tube->Show(helix_only);
    m_hs_strands_label->Show(helix_only);
    m_spin_hs_strands->Show(helix_only);

    // Exact primitives replace the generic size and height rows by one row per parameter.
    m_size_label->Show(!primitive);
    m_spin_size->Show(!primitive);
    m_height_label->Show(!primitive);
    m_spin_sla_h->Show(!primitive);
    const std::vector<PrimitiveParamInfo> no_params;
    const auto& info = primitive ? primitive_param_info(primitive_of_shape(s_params.shape)) : no_params;
    for (int i = 0; i < MaxPrimitiveParams; ++i) {
        const bool used = i < int(info.size());
        m_prim_labels[i]->Show(used);
        m_prim_spins[i]->Show(used);
        if (!used)
            continue;
        const std::vector<double>& values = s_prim[int(primitive_of_shape(s_params.shape))];
        m_prim_labels[i]->SetLabel(_(info[i].label) + (*info[i].unit ? wxString(" (") + info[i].unit + ")" : wxString()));
        m_prim_spins[i]->SetRange(info[i].min, info[i].max);
        m_prim_spins[i]->SetDigits(info[i].integer ? 0 : 4);
        m_prim_spins[i]->SetValue(values[i]);
    }

    Layout();
    Fit();
}

bool ShapeGenDialog::primitive_controls_changed() const
{
    if (!is_primitive_shape(s_params.shape))
        return false;
    const std::vector<double>& values = s_prim[int(primitive_of_shape(s_params.shape))];
    for (size_t i = 0; i < values.size() && i < size_t(MaxPrimitiveParams); ++i)
        if (m_prim_spins[i]->GetValue() != values[i])
            return true;
    return false;
}

void ShapeGenDialog::remove_temp_object()
{
    if (m_temp_obj_idxs.empty())
        return;
    // Remove highest index first so earlier indices stay valid.
    std::vector<size_t> sorted = m_temp_obj_idxs;
    std::sort(sorted.begin(), sorted.end(), std::greater<size_t>());
    for (size_t idx : sorted)
        wxGetApp().plater()->remove(idx);
    m_temp_obj_idxs.clear();
}

void ShapeGenDialog::rebuild_preview()
{
    // Always read from controls — spin events on GTK can fire before the
    // internal value is committed, so GetValue() here is the ground truth.
    s_params.size           = m_spin_size->GetValue();
    s_params.sla_h          = m_spin_sla_h->GetValue();
    s_params.n_materials    = m_spin_n_materials->GetValue();
    s_params.stripe_rows    = m_spin_stripe_rows->GetValue();
    s_params.cb_columns     = m_spin_cb_columns->GetValue();
    s_params.cb_rows        = m_spin_cb_rows->GetValue();
    s_params.hx_revolutions = m_spin_hx_rev->GetValue();
    s_params.hx_width       = m_spin_hx_width->GetValue();
    s_params.hc_sectors     = m_spin_hc_sectors->GetValue();
    s_params.hc_bands       = m_spin_hc_bands->GetValue();
    s_params.hs_turns       = m_spin_hs_turns->GetValue();
    s_params.hs_tube_pct    = m_spin_hs_tube->GetValue();
    s_params.hs_strands     = m_spin_hs_strands->GetValue();

    if (is_primitive_shape(s_params.shape)) {
        std::vector<double>& values = s_prim[int(primitive_of_shape(s_params.shape))];
        for (size_t i = 0; i < values.size() && i < size_t(MaxPrimitiveParams); ++i)
            values[i] = m_prim_spins[i]->GetValue();
    }

    Plater::SuppressSnapshots suppress(wxGetApp().plater());

    const float sla_h   = (float)s_params.sla_h;

    remove_temp_object();

    Model temp;
    if (is_primitive_shape(s_params.shape)) {
        const PrimitiveType type = primitive_of_shape(s_params.shape);
        if (const std::string err = primitive_validate(type, s_prim[int(type)]); !err.empty()) {
            m_status_label->SetLabel(_(err.c_str()));
            m_btn_place->Enable(false);
            Layout();
            return;
        }
        m_btn_place->Enable(true);
    }
    build_into_model(temp, s_params);

    m_temp_obj_idxs = wxGetApp().plater()->add_model_objects(temp.objects);

    if (is_primitive_shape(s_params.shape)) {
        const PrimitiveType type = primitive_of_shape(s_params.shape);
        m_status_label->SetLabel(wxString::Format(_L("Preview: %s  |  volume %.3f mm3"),
            _(primitive_name(type)), primitive_volume(type, s_prim[int(type)])));
    } else if (s_params.shape == 5) {
        m_status_label->SetLabel(wxString::Format(
            _L("Preview: Helix  |  %d strand(s)  |  %.1f mm"),
            s_params.hs_strands, sla_h));
    } else {
        m_status_label->SetLabel(wxString::Format(
            _L("Preview: %s  |  %s  |  %.1f mm"),
            m_choice_shape->GetStringSelection(),
            m_choice_pattern->GetStringSelection(),
            sla_h));
    }
    Layout();

    // Force the GL canvas to repaint immediately — idle events are suppressed
    // while a modal dialog runs its own event loop on GTK.
    wxGetApp().plater()->canvas3D()->force_repaint();
}

void ShapeGenDialog::on_place(wxCommandEvent&)
{
    rebuild_preview();
    m_placed = true;
    Plater::TakeSnapshot snapshot(wxGetApp().plater(), _L("Place SLA Shape"));
    EndModal(wxID_OK);
}

void ShapeGenDialog::on_cancel(wxCommandEvent&)
{
    remove_temp_object();
    EndModal(wxID_CANCEL);
}

} // namespace GUI
} // namespace Slic3r
