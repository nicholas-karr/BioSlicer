#ifndef slic3r_GLGizmoDroplets_hpp_
#define slic3r_GLGizmoDroplets_hpp_

#include <optional>
#include <string>
#include <vector>

#include "GLGizmoBase.hpp"
#include "libslic3r/Droplet.hpp"
#include "libslic3r/Line.hpp"

namespace Slic3r {

class Model;

namespace GUI {

// Edit the droplets of the project by clicking in the 3D scene:
//   left click           add a droplet on the surface of the selected object, or on the bed at the working height
//   left click on one    select it, to edit its numbers
//   right click on one   delete it
// The droplets are drawn by the canvas whether or not this gizmo is open.
class GLGizmoDroplets : public GLGizmoBase
{
public:
    GLGizmoDroplets(GLCanvas3D &parent, const std::string &icon_filename, unsigned int sprite_id);

    bool on_mouse(const wxMouseEvent &mouse_event) override;
    std::string get_tooltip() const override;

protected:
    bool on_init() override { return true; }
    std::string on_get_name() const override;
    bool on_is_activable() const override;
    bool on_is_selectable() const override { return false; }
    CommonGizmosDataID on_get_requirements() const override;
    void on_set_state() override;
    void on_render() override;
    void on_render_input_window(float x, float y, float bottom_limit) override;

private:
    // Where a droplet is stored.
    struct Source
    {
        int    object { -1 };
        size_t set { 0 };
        size_t index { 0 };
        bool operator==(const Source &rhs) const { return object == rhs.object && set == rhs.set && index == rhs.index; }
    };

    // Where a click at the mouse would put a droplet: in world coordinates, and as stored (object coordinates when it
    // lands on the selected object, bed coordinates otherwise).
    struct Target
    {
        Vec3d world;
        Vec3d stored;
        int   object { -1 };
    };
    std::optional<Target> locate(const Vec2d &mouse_pos, const Linef3 &ray) const;

    DropletPoint *point(const Source &source) const;
    // The sets of a container: those of the model object, or of the model itself for object -1.
    DropletSets  *sets_of(int object) const;
    void          add_at(const Vec3d &position, int object);
    void          remove(const Source &source);
    void          select(const std::optional<Source> &source);
    // Tell the plater the droplets changed.
    void          changed(int object);
    std::vector<int> droplet_extruders() const;

    float  m_working_z { 0.f };
    float  m_volume_nl { 50.f };
    int    m_count { 1 };
    int    m_extruder_choice { 0 };
    bool   m_dip { false };
    std::optional<Source> m_selected;
    // Where the droplet would land if the mouse were clicked now, shown as a ghost.
    std::optional<Target> m_hover_target;
};

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GLGizmoDroplets_hpp_
