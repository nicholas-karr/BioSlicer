#include "DropletMarkers.hpp"

#include <algorithm>
#include <cmath>

#include "GUI_App.hpp"
#include "GLShader.hpp"
#include "Camera.hpp"
#include "Plater.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/Geometry.hpp"

namespace Slic3r {
namespace GUI {

float DropletMarkers::marker_radius(double volume_nl)
{
    // 1 nL is 0.001 mm^3.
    const double radius = std::cbrt(3. * std::max(volume_nl, 0.) * 1e-3 / (4. * PI));
    return float(std::max(radius, 0.3));
}

void DropletMarkers::render(const Model &model, const ResolvedDroplet *selected)
{
    m_drawn = collect_droplets_for_display(model);
    if (m_drawn.empty())
        return;
    GLShaderProgram *shader = wxGetApp().get_shader("gouraud_light");
    if (shader == nullptr)
        return;

    if (! m_sphere.is_initialized())
        m_sphere.init_from(its_make_sphere(1.0, double(PI) / 12.0));

    const std::vector<ColorRGBA> colors = wxGetApp().plater()->get_extruder_colors_from_plater_config();
    const Camera &camera = wxGetApp().plater()->get_camera();
    const Transform3d &view_matrix = camera.get_view_matrix();

    shader->start_using();
    shader->set_uniform("projection_matrix", camera.get_projection_matrix());
    shader->set_uniform("emission_factor", 0.3f);
    for (const ResolvedDroplet &d : m_drawn) {
        const bool is_selected = selected != nullptr && d.object == selected->object && d.source_set == selected->source_set
            && d.index == selected->index;
        ColorRGBA color = ColorRGBA::BLUEISH();
        if (d.extruder > 0 && d.extruder <= colors.size())
            color = colors[d.extruder - 1];
        m_sphere.set_color(is_selected ? ColorRGBA::REDISH() : color);

        const Transform3d model_matrix = Geometry::translation_transform(d.pos) * Geometry::scale_transform(double(marker_radius(d.volume_nl)));
        shader->set_uniform("view_model_matrix", view_matrix * model_matrix);
        const Matrix3d view_normal_matrix = view_matrix.matrix().block(0, 0, 3, 3) * model_matrix.matrix().block(0, 0, 3, 3).inverse().transpose();
        shader->set_uniform("view_normal_matrix", view_normal_matrix);
        m_sphere.render();
    }
    shader->stop_using();
}

void DropletMarkers::render_ghost(const Vec3d &position, double volume_nl)
{
    GLShaderProgram *shader = wxGetApp().get_shader("gouraud_light");
    if (shader == nullptr)
        return;
    if (! m_sphere.is_initialized())
        m_sphere.init_from(its_make_sphere(1.0, double(PI) / 12.0));
    const Camera &camera = wxGetApp().plater()->get_camera();
    const Transform3d &view_matrix = camera.get_view_matrix();
    shader->start_using();
    shader->set_uniform("projection_matrix", camera.get_projection_matrix());
    shader->set_uniform("emission_factor", 0.6f);
    m_sphere.set_color(ColorRGBA(0.35f, 1.f, 0.55f, 1.f));
    const Transform3d model_matrix = Geometry::translation_transform(position) * Geometry::scale_transform(double(std::max(marker_radius(volume_nl), 0.6f)));
    shader->set_uniform("view_model_matrix", view_matrix * model_matrix);
    shader->set_uniform("view_normal_matrix", Matrix3d(view_matrix.matrix().block(0, 0, 3, 3) * model_matrix.matrix().block(0, 0, 3, 3).inverse().transpose()));
    m_sphere.render();
    shader->stop_using();
}

int DropletMarkers::pick(const Vec3d &ray_origin, const Vec3d &ray_direction, double max_distance) const
{
    const Vec3d dir = ray_direction.normalized();
    int    best = -1;
    double best_distance = max_distance;
    for (size_t i = 0; i < m_drawn.size(); ++i) {
        const Vec3d to_point = m_drawn[i].pos - ray_origin;
        const double along = to_point.dot(dir);
        if (along < 0.)
            continue;
        // Distance of the droplet from the ray, less the size of its marker: a droplet is hit by clicking on it.
        const double distance = (to_point - along * dir).norm() - double(marker_radius(m_drawn[i].volume_nl));
        if (distance < best_distance) {
            best_distance = distance;
            best = int(i);
        }
    }
    return best;
}

} // namespace GUI
} // namespace Slic3r
