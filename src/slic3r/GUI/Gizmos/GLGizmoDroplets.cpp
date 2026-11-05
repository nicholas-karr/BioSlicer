#include "GLGizmoDroplets.hpp"

#include <algorithm>

#include <imgui/imgui.h>

#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/ImGuiPureWrap.hpp"
#include "slic3r/GUI/ImGuiWrapper.hpp"
#include "slic3r/GUI/Gizmos/GLGizmosCommon.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/MeshUtils.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"

namespace Slic3r {
namespace GUI {

namespace {
constexpr const char *clicked_set_name = "Clicked points";
}

GLGizmoDroplets::GLGizmoDroplets(GLCanvas3D &parent, const std::string &icon_filename, unsigned int sprite_id)
    : GLGizmoBase(parent, icon_filename, sprite_id)
{}

std::string GLGizmoDroplets::on_get_name() const { return _u8L("Droplets"); }

std::string GLGizmoDroplets::get_tooltip() const
{
    return _u8L("Click: add a droplet.  Click on a droplet: select it.  Right click on a droplet: delete it.");
}

bool GLGizmoDroplets::on_is_activable() const
{
    return wxGetApp().preset_bundle->printers.get_edited_preset().printer_technology() == ptFFF;
}

CommonGizmosDataID GLGizmoDroplets::on_get_requirements() const
{
    return CommonGizmosDataID(int(CommonGizmosDataID::SelectionInfo) | int(CommonGizmosDataID::Raycaster));
}

void GLGizmoDroplets::on_set_state()
{
    if (m_state == Off) {
        m_selected.reset();
        m_hover_target.reset();
        m_parent.set_selected_droplet(std::nullopt);
    }
}

std::vector<int> GLGizmoDroplets::droplet_extruders() const
{
    std::vector<int> out;
    const DynamicPrintConfig &cfg = wxGetApp().preset_bundle->printers.get_edited_preset().config;
    if (const auto *types = cfg.option<ConfigOptionStrings>("channel_type"))
        for (size_t i = 0; i < types->values.size(); ++i)
            if (types->values[i] == "droplet")
                out.push_back(int(i) + 1);
    return out;
}

DropletSets *GLGizmoDroplets::sets_of(int object) const
{
    Model &model = wxGetApp().plater()->model();
    if (object < 0)
        return &model.droplet_sets;
    return size_t(object) < model.objects.size() ? &model.objects[size_t(object)]->droplet_sets : nullptr;
}

DropletPoint *GLGizmoDroplets::point(const Source &source) const
{
    DropletSets *sets = sets_of(source.object);
    if (sets == nullptr || source.set >= sets->size() || source.index >= (*sets)[source.set].points.size())
        return nullptr;
    return &(*sets)[source.set].points[source.index];
}

void GLGizmoDroplets::changed(int object)
{
    Plater *plater = wxGetApp().plater();
    if (object >= 0 && size_t(object) < plater->model().objects.size())
        plater->changed_object(object);
    else
        plater->set_droplet_sets(DropletSets(plater->model().droplet_sets));
}

void GLGizmoDroplets::select(const std::optional<Source> &source)
{
    m_selected = source;
    if (! source) {
        m_parent.set_selected_droplet(std::nullopt);
        return;
    }
    ResolvedDroplet d;
    d.object     = source->object;
    d.source_set = source->set;
    d.index      = source->index;
    m_parent.set_selected_droplet(d);
}

void GLGizmoDroplets::add_at(const Vec3d &position, int object)
{
    const std::vector<int> extruders = droplet_extruders();
    if (extruders.empty())
        return;
    const int extruder = extruders[size_t(std::clamp(m_extruder_choice, 0, int(extruders.size()) - 1))];
    DropletSets *sets = sets_of(object);
    if (sets == nullptr)
        return;

    Plater::TakeSnapshot snapshot(wxGetApp().plater(), _L("Add droplet"));
    size_t set_idx = sets->size();
    for (size_t i = 0; i < sets->size(); ++i)
        if ((*sets)[i].name == clicked_set_name && (*sets)[i].extruder == extruder && (*sets)[i].dip == m_dip) {
            set_idx = i;
            break;
        }
    if (set_idx == sets->size()) {
        DropletSet set;
        set.name       = clicked_set_name;
        set.extruder   = extruder;
        set.dip        = m_dip;
        set.keep_order = false;
        sets->emplace_back(std::move(set));
    }
    DropletPoint p;
    p.pos       = position;
    p.volume_nl = double(m_volume_nl);
    p.count     = m_count;
    (*sets)[set_idx].points.emplace_back(p);
    this->changed(object);
    this->select(Source{ object, set_idx, (*sets)[set_idx].points.size() - 1 });
}

void GLGizmoDroplets::remove(const Source &source)
{
    DropletSets *sets = sets_of(source.object);
    if (sets == nullptr || point(source) == nullptr)
        return;
    Plater::TakeSnapshot snapshot(wxGetApp().plater(), _L("Delete droplet"));
    auto &points = (*sets)[source.set].points;
    points.erase(points.begin() + source.index);
    if (points.empty() && (*sets)[source.set].name == clicked_set_name)
        sets->erase(sets->begin() + source.set);
    this->select(std::nullopt);
    this->changed(source.object);
}

std::optional<GLGizmoDroplets::Target> GLGizmoDroplets::locate(const Vec2d &mouse_pos, const Linef3 &ray) const
{
    // The surface of the selected object under the mouse: the droplet then belongs to the object.
    ModelObject *mo = m_c->selection_info() ? m_c->selection_info()->model_object() : nullptr;
    // model_instance() needs a selected object.
    ModelInstance *inst = mo != nullptr ? m_c->selection_info()->model_instance() : nullptr;
    if (mo != nullptr && inst != nullptr && m_c->raycaster()->raycasters().size() == 1 && m_c->raycaster()->raycaster() != nullptr) {
        Vec3f hit, normal;
        const Transform3d trafo = inst->get_transformation().get_matrix();
        if (m_c->raycaster()->raycaster()->unproject_on_mesh(mouse_pos, trafo, wxGetApp().plater()->get_camera(), hit, normal, nullptr)) {
            const Model &model = wxGetApp().plater()->model();
            const auto it = std::find(model.objects.begin(), model.objects.end(), mo);
            if (it != model.objects.end())
                // Stored in object coordinates: where the mesh was hit.
                return Target{ trafo * hit.cast<double>(), hit.cast<double>(), int(it - model.objects.begin()) };
        }
    }
    // The bed: the ray meets the plane at the working height.
    const Vec3d direction = ray.b - ray.a;
    if (std::abs(direction.z()) > 1e-9) {
        const double t = (double(m_working_z) - ray.a.z()) / direction.z();
        if (t > 0.) {
            const Vec3d p = ray.a + t * direction;
            return Target{ p, p, -1 };
        }
    }
    return std::nullopt;
}

bool GLGizmoDroplets::on_mouse(const wxMouseEvent &mouse_event)
{
    const Vec2d  mouse_pos(mouse_event.GetX(), mouse_event.GetY());
    const Linef3 ray = m_parent.mouse_ray(Point(mouse_event.GetX(), mouse_event.GetY()));
    const Vec3d  direction = ray.b - ray.a;
    const DropletMarkers &markers = m_parent.droplet_markers();
    const int    hit = markers.pick(ray.a, direction, 0.3);

    if (! (mouse_event.LeftDown() || mouse_event.RightDown())) {
        // Moving: show where a click would put a droplet, unless the mouse is on one.
        if (mouse_event.Moving() || mouse_event.Dragging()) {
            m_hover_target = hit >= 0 ? std::nullopt : locate(mouse_pos, ray);
            m_parent.set_as_dirty();
        }
        return false;
    }

    // A droplet under the mouse: select it, or delete it with the right button.
    if (hit >= 0) {
        const ResolvedDroplet &d = markers.last()[size_t(hit)];
        const Source source { d.object, d.source_set, d.index };
        if (mouse_event.RightDown())
            this->remove(source);
        else
            this->select(source);
        return true;
    }
    if (mouse_event.RightDown())
        return false;
    if (const std::optional<Target> target = locate(mouse_pos, ray)) {
        this->add_at(target->stored, target->object);
        return true;
    }
    return false;
}

void GLGizmoDroplets::on_render()
{
    if (m_hover_target)
        m_parent.render_droplet_ghost(m_hover_target->world, double(m_volume_nl));
}

void GLGizmoDroplets::on_render_input_window(float x, float y, float bottom_limit)
{
    const float approx_height = m_imgui->scaled(24.0f);
    y = std::min(y, bottom_limit - approx_height);
    ImGuiPureWrap::set_next_window_pos(x, y, ImGuiCond_Always);
    ImGuiPureWrap::begin(get_name(), ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse);

    const std::vector<int> extruders = droplet_extruders();
    if (extruders.empty()) {
        ImGuiPureWrap::text(_u8L("The printer has no droplet channel."));
        const bool close = ImGuiPureWrap::button(_u8L("Close"));
        ImGuiPureWrap::end();
        if (close)
            m_parent.get_gizmos_manager().open_gizmo(GLGizmosManager::Droplets);
        return;
    }

    ImGuiPureWrap::text(_u8L("New droplets"));
    ImGui::PushItemWidth(m_imgui->scaled(8.f));
    ImGui::InputFloat(_u8L("Volume (nL)").c_str(), &m_volume_nl, 1.f, 10.f, "%.2f");
    m_volume_nl = std::max(m_volume_nl, 0.1f);
    ImGui::InputInt(_u8L("Drops").c_str(), &m_count);
    m_count = std::clamp(m_count, 1, 1000);
    ImGui::InputFloat(_u8L("Bed height (mm)").c_str(), &m_working_z, 0.1f, 1.f, "%.2f");
    m_working_z = std::max(m_working_z, 0.f);
    ImGui::PopItemWidth();
    if (extruders.size() > 1) {
        m_extruder_choice = std::clamp(m_extruder_choice, 0, int(extruders.size()) - 1);
        std::string items;
        for (int e : extruders)
            items += "Extruder " + std::to_string(e) + '\0';
        ImGui::Combo(_u8L("Channel").c_str(), &m_extruder_choice, items.c_str());
    }
    ImGui::Checkbox(_u8L("Dip to the landing height").c_str(), &m_dip);

    ImGui::Separator();
    DropletPoint *selected = m_selected ? point(*m_selected) : nullptr;
    if (selected == nullptr) {
        ImGuiPureWrap::text(_u8L("No droplet selected."));
    } else {
        ImGuiPureWrap::text(m_selected->object >= 0 ? _u8L("Selected droplet (object coordinates)") : _u8L("Selected droplet (bed coordinates)"));
        bool edited = false;
        DropletPoint edit = *selected;
        ImGui::PushItemWidth(m_imgui->scaled(8.f));
        const ImGuiInputTextFlags commit = ImGuiInputTextFlags_EnterReturnsTrue;
        edited |= ImGui::InputDouble("X (mm)", &edit.pos.x(), 0.1, 1., "%.3f", commit);
        edited |= ImGui::InputDouble("Y (mm)", &edit.pos.y(), 0.1, 1., "%.3f", commit);
        edited |= ImGui::InputDouble("Z (mm)", &edit.pos.z(), 0.1, 1., "%.3f", commit);
        edited |= ImGui::InputDouble(_u8L("Volume (nL)").c_str(), &edit.volume_nl, 1., 10., "%.2f", commit);
        edited |= ImGui::InputInt(_u8L("Drops").c_str(), &edit.count, 1, 10, commit);
        ImGui::PopItemWidth();
        ImGuiPureWrap::text(_u8L("Press Enter to apply a number."));
        if (edited) {
            edit.volume_nl = std::max(edit.volume_nl, 0.1);
            edit.count     = std::clamp(edit.count, 1, 1000);
            Plater::TakeSnapshot snapshot(wxGetApp().plater(), _L("Edit droplet"));
            *selected = edit;
            this->changed(m_selected->object);
        }
        if (ImGuiPureWrap::button(_u8L("Delete droplet"))) {
            const Source source = *m_selected;
            this->remove(source);
        }
    }
    ImGui::Separator();
    ImGuiPureWrap::text(_u8L("Esc closes this panel."));
    const bool close = ImGuiPureWrap::button(_u8L("Close"));
    ImGuiPureWrap::end();
    // Closing destroys the state this window draws from: do it after the window is ended.
    if (close)
        m_parent.get_gizmos_manager().open_gizmo(GLGizmosManager::Droplets);
}

} // namespace GUI
} // namespace Slic3r
