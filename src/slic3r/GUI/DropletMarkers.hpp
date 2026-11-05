#pragma once

#include <vector>

#include "GLModel.hpp"
#include "libslic3r/Color.hpp"
#include "libslic3r/Droplet.hpp"

namespace Slic3r {

class Model;

namespace GUI {

// Draws the droplets of the model as small spheres in the 3D scene, sized by their volume and coloured by their channel.
class DropletMarkers
{
public:
    // Radius of the sphere of a droplet, mm: the radius of a drop of that volume, with a floor to keep it visible.
    static float marker_radius(double volume_nl);

    // Draws the droplets of the model. `selected` is the droplet to highlight, if any.
    void render(const Model &model, const ResolvedDroplet *selected = nullptr);

    // Draws one droplet that does not exist yet, to show where a click would put it.
    void render_ghost(const Vec3d &position, double volume_nl);

    // The droplet nearest to the ray, within `max_distance` mm of it; null if none. Returns an index into last().
    int pick(const Vec3d &ray_origin, const Vec3d &ray_direction, double max_distance) const;

    // The droplets drawn by the last render().
    const std::vector<ResolvedDroplet> &last() const { return m_drawn; }

private:
    GLModel                      m_sphere;
    std::vector<ResolvedDroplet> m_drawn;
};

} // namespace GUI
} // namespace Slic3r
