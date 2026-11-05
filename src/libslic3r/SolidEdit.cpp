#include "SolidEdit.hpp"

#include "BoundingBox.hpp"
#include "MeshBoolean.hpp"
#include "Model.hpp"

#include <algorithm>
#include <cmath>
#include <exception>
#include <set>

namespace Slic3r {
namespace SolidEdit {

namespace {

// Mesh of a model volume in object coordinates.
TriangleMesh volume_mesh_in_object(const ModelVolume &volume)
{
    TriangleMesh mesh = volume.mesh();
    mesh.transform(volume.get_matrix(), true);
    return mesh;
}

std::string quote_name(const std::string &name)
{
    return "'" + name + "'";
}

Result boolean_impl(BooleanOp op, const TriangleMesh &base, const std::vector<TriangleMesh> &tools, bool allow_empty)
{
    if (tools.empty())
        return Result::failure("Select at least two shapes.");

    if (std::string err = check_solid(base, "The first shape"); ! err.empty())
        return Result::failure(err);
    for (size_t i = 0; i < tools.size(); ++ i)
        if (std::string err = check_solid(tools[i], "Shape " + std::to_string(i + 2)); ! err.empty())
            return Result::failure(err);

    TriangleMesh acc = base;
    try {
        for (const TriangleMesh &tool : tools) {
            switch (op) {
            case BooleanOp::Union:     MeshBoolean::cgal::plus(acc, tool);      break;
            case BooleanOp::Subtract:  MeshBoolean::cgal::minus(acc, tool);     break;
            case BooleanOp::Intersect: MeshBoolean::cgal::intersect(acc, tool); break;
            }
            if (acc.its.indices.empty() && op == BooleanOp::Intersect)
                break;
        }
    } catch (const std::exception &) {
        return Result::failure("The boolean operation failed. The shapes may touch each other in a way that cannot be resolved. "
                               "Try moving one of them by a small amount.");
    } catch (...) {
        return Result::failure("The boolean operation failed.");
    }

    Result out;
    out.ok = true;
    if (acc.its.indices.empty() || its_volume(acc.its) <= 0.f) {
        if (! allow_empty)
            return Result::failure(op == BooleanOp::Intersect ?
                "The shapes do not overlap, so the intersection would be empty." :
                "The result would be empty.");
        out.mesh = TriangleMesh();
        return out;
    }
    out.mesh = std::move(acc);
    return out;
}

// Replaces the geometry of a volume with a mesh given in object coordinates.
void replace_volume_mesh(ModelVolume &volume, TriangleMesh &&mesh_in_object)
{
    volume.set_mesh(std::move(mesh_in_object));
    volume.set_transformation(Transform3d::Identity());
    volume.center_geometry_after_creation(false);
    volume.calculate_convex_hull();
    // Painted data refers to facets of the old mesh.
    volume.supported_facets.reset();
    volume.seam_facets.reset();
    volume.mm_segmentation_facets.reset();
    volume.fuzzy_skin_facets.reset();
    volume.text_configuration.reset();
    volume.emboss_shape.reset();
    if (ModelObject *object = volume.get_object(); object != nullptr) {
        object->input_file.clear();
        object->invalidate_bounding_box();
    }
}

void delete_volumes(ModelObject &object, std::vector<size_t> idxs)
{
    std::sort(idxs.begin(), idxs.end(), std::greater<size_t>());
    for (size_t idx : idxs)
        object.delete_volume(idx);
}

// Union of all solid parts of an object, transformed by trafo.
Result object_solid(const ModelObject &object, const Transform3d &trafo)
{
    std::vector<TriangleMesh> meshes;
    for (const ModelVolume *v : object.volumes)
        if (v->is_model_part()) {
            TriangleMesh m = v->mesh();
            m.transform(trafo * v->get_matrix(), true);
            meshes.emplace_back(std::move(m));
        }
    if (meshes.empty())
        return Result::failure("The object " + quote_name(object.name) + " has no solid parts.");
    Result out;
    out.ok = true;
    if (meshes.size() == 1) {
        if (std::string err = check_solid(meshes.front(), "The object " + quote_name(object.name)); ! err.empty())
            return Result::failure(err);
        out.mesh = std::move(meshes.front());
        return out;
    }
    TriangleMesh first = std::move(meshes.front());
    meshes.erase(meshes.begin());
    return boolean_impl(BooleanOp::Union, first, meshes, false);
}

// Closed prism between two points, ring radius r with n sides. Outward facing triangles.
void add_prism(indexed_triangle_set &its, const Vec3d &a, const Vec3d &b, double r, int n)
{
    const Vec3d d = (b - a).normalized();
    const Vec3d helper = std::abs(d.x()) < 0.9 ? Vec3d::UnitX() : Vec3d::UnitY();
    const Vec3d u = d.cross(helper).normalized();
    const Vec3d v = d.cross(u);

    const int base = int(its.vertices.size());
    for (int end = 0; end < 2; ++ end)
        for (int i = 0; i < n; ++ i) {
            const double t = 2. * M_PI * double(i) / double(n);
            its.vertices.emplace_back(((end == 0 ? a : b) + r * (std::cos(t) * u + std::sin(t) * v)).cast<float>());
        }
    const int ca = int(its.vertices.size());
    its.vertices.emplace_back(a.cast<float>());
    const int cb = int(its.vertices.size());
    its.vertices.emplace_back(b.cast<float>());

    for (int i = 0; i < n; ++ i) {
        const int j = (i + 1) % n;
        const int a_i = base + i, a_j = base + j, b_i = base + n + i, b_j = base + n + j;
        its.indices.emplace_back(a_i, a_j, b_j);
        its.indices.emplace_back(a_i, b_j, b_i);
        its.indices.emplace_back(ca, a_j, a_i);
        its.indices.emplace_back(cb, b_i, b_j);
    }
}

} // namespace

std::string check_solid(const TriangleMesh &mesh, const std::string &label)
{
    if (mesh.its.indices.empty())
        return label + " is empty.";
    if (its_num_open_edges(mesh.its) > 0)
        return label + " is not watertight (it has holes or open edges). Repair it first.";
    try {
        if (MeshBoolean::cgal::does_self_intersect(mesh))
            return label + " intersects itself. Repair it first.";
    } catch (...) {
        return label + " is not a valid closed surface. Repair it first.";
    }
    if (its_volume(mesh.its) <= 0.f)
        return label + " has no volume or is turned inside out. Repair it first.";
    return {};
}

Result boolean(BooleanOp op, const TriangleMesh &base, const std::vector<TriangleMesh> &tools)
{
    return boolean_impl(op, base, tools, false);
}

Result boolean_volumes(ModelObject &object, BooleanOp op, const std::vector<size_t> &volume_idxs)
{
    if (volume_idxs.size() < 2)
        return Result::failure("Select at least two parts.");
    if (std::set<size_t>(volume_idxs.begin(), volume_idxs.end()).size() != volume_idxs.size())
        return Result::failure("The same part was selected twice.");
    for (size_t idx : volume_idxs) {
        if (idx >= object.volumes.size())
            return Result::failure("A selected part does not exist.");
        if (! object.volumes[idx]->is_model_part())
            return Result::failure("Boolean operations work on solid parts only. Modifiers, negative volumes and "
                                   "support blockers cannot be combined.");
    }

    std::vector<TriangleMesh> tools;
    for (size_t i = 1; i < volume_idxs.size(); ++ i)
        tools.emplace_back(volume_mesh_in_object(*object.volumes[volume_idxs[i]]));

    Result result = boolean_impl(op, volume_mesh_in_object(*object.volumes[volume_idxs.front()]), tools, false);
    if (! result.ok)
        return result;

    ModelVolume *base = object.volumes[volume_idxs.front()];
    replace_volume_mesh(*base, TriangleMesh(result.mesh));
    delete_volumes(object, std::vector<size_t>(volume_idxs.begin() + 1, volume_idxs.end()));
    return result;
}

Result boolean_objects(Model &model, BooleanOp op, const std::vector<size_t> &obj_idxs)
{
    if (obj_idxs.size() < 2)
        return Result::failure("Select at least two objects.");
    if (std::set<size_t>(obj_idxs.begin(), obj_idxs.end()).size() != obj_idxs.size())
        return Result::failure("The same object was selected twice.");
    for (size_t idx : obj_idxs)
        if (idx >= model.objects.size() || model.objects[idx]->instances.empty())
            return Result::failure("A selected object does not exist.");

    std::vector<TriangleMesh> solids;
    for (size_t idx : obj_idxs) {
        const ModelObject &obj = *model.objects[idx];
        Result r = object_solid(obj, obj.instances.front()->get_matrix());
        if (! r.ok)
            return r;
        solids.emplace_back(std::move(r.mesh));
    }
    TriangleMesh base = std::move(solids.front());
    solids.erase(solids.begin());
    Result result = boolean_impl(op, base, solids, false);
    if (! result.ok)
        return result;

    const ModelObject &base_obj = *model.objects[obj_idxs.front()];
    const ModelVolume *tpl = nullptr;
    for (const ModelVolume *v : base_obj.volumes)
        if (v->is_model_part()) {
            tpl = v;
            break;
        }

    ModelObject *obj = model.add_object(base_obj);
    while (obj->instances.size() > 1)
        obj->delete_instance(obj->instances.size() - 1);
    for (ModelVolume *v : obj->volumes)
        delete v;
    obj->volumes.clear();
    obj->layer_config_ranges.clear();

    TriangleMesh local = result.mesh;
    local.transform(base_obj.instances.front()->get_matrix().inverse(), true);
    ModelVolume *nv = obj->add_volume(std::move(local), ModelVolumeType::MODEL_PART);
    if (tpl != nullptr) {
        nv->name = tpl->name;
        nv->config.assign_config(tpl->config);
    }
    obj->input_file.clear();
    obj->center_around_origin();
    obj->translate_instances(-obj->origin_translation);
    obj->origin_translation = Vec3d::Zero();
    obj->invalidate_bounding_box();

    result.object = obj;
    return result;
}

Result apply_negative_volumes(ModelObject &object)
{
    std::vector<size_t> parts, negatives;
    for (size_t i = 0; i < object.volumes.size(); ++ i) {
        if (object.volumes[i]->is_model_part())
            parts.push_back(i);
        else if (object.volumes[i]->is_negative_volume())
            negatives.push_back(i);
    }
    if (negatives.empty())
        return Result::failure("There are no negative volumes to apply.");
    if (parts.empty())
        return Result::failure("The object has no solid parts to cut.");

    std::vector<TriangleMesh> neg_meshes;
    std::vector<BoundingBoxf3> neg_boxes;
    for (size_t idx : negatives) {
        neg_meshes.emplace_back(volume_mesh_in_object(*object.volumes[idx]));
        if (std::string err = check_solid(neg_meshes.back(), "The negative volume " + quote_name(object.volumes[idx]->name)); ! err.empty())
            return Result::failure(err);
        neg_boxes.emplace_back(neg_meshes.back().bounding_box());
    }

    struct Change { size_t idx; bool vanished; TriangleMesh mesh; };
    std::vector<Change> changes;
    for (size_t idx : parts) {
        TriangleMesh part = volume_mesh_in_object(*object.volumes[idx]);
        const BoundingBoxf3 box = part.bounding_box();
        std::vector<TriangleMesh> tools;
        for (size_t i = 0; i < neg_meshes.size(); ++ i)
            if (box.intersects(neg_boxes[i]))
                tools.push_back(neg_meshes[i]);
        if (tools.empty())
            continue;
        Result r = boolean_impl(BooleanOp::Subtract, part, tools, true);
        if (! r.ok)
            return Result::failure("Part " + quote_name(object.volumes[idx]->name) + ": " + r.error);
        changes.push_back({ idx, r.mesh.its.indices.empty(), std::move(r.mesh) });
    }

    const size_t vanished = std::count_if(changes.begin(), changes.end(), [](const Change &c) { return c.vanished; });
    if (vanished == parts.size())
        return Result::failure("The negative volumes remove the whole object.");

    std::vector<size_t> to_delete = negatives;
    for (Change &c : changes) {
        if (c.vanished)
            to_delete.push_back(c.idx);
        else
            replace_volume_mesh(*object.volumes[c.idx], std::move(c.mesh));
    }
    delete_volumes(object, to_delete);

    Result out;
    out.ok = true;
    return out;
}

Result make_channel_tool(const std::vector<Vec3d> &path, const ChannelParams &params)
{
    if (path.size() < 2)
        return Result::failure("A channel needs at least two points.");
    if (! (params.diameter > 0.))
        return Result::failure("The channel diameter must be larger than zero.");
    if (params.inlet_extension < 0. || params.outlet_extension < 0.)
        return Result::failure("Port lengths cannot be negative.");
    if (params.sides < 3)
        return Result::failure("A channel needs at least 3 sides.");
    for (size_t i = 1; i < path.size(); ++ i)
        if ((path[i] - path[i - 1]).norm() < 1e-6)
            return Result::failure("Two consecutive channel points are at the same position.");

    std::vector<Vec3d> pts = path;
    pts.front() -= (path[1] - path[0]).normalized() * params.inlet_extension;
    pts.back()  += (path.back() - path[path.size() - 2]).normalized() * params.outlet_extension;

    // The polygon has the area of the circle, so the straight channel volume is exactly pi r^2 L.
    const int    n      = params.sides;
    const double r      = 0.5 * params.diameter;
    const double r_poly = r * std::sqrt(2. * M_PI / (double(n) * std::sin(2. * M_PI / double(n))));

    TriangleMesh tool;
    try {
        for (size_t i = 0; i + 1 < pts.size(); ++ i) {
            indexed_triangle_set prism;
            add_prism(prism, pts[i], pts[i + 1], r_poly, n);
            if (i == 0)
                tool = TriangleMesh(std::move(prism));
            else
                MeshBoolean::cgal::plus(tool, TriangleMesh(std::move(prism)));
        }
        // Spheres round the bends.
        for (size_t i = 1; i + 1 < pts.size(); ++ i) {
            TriangleMesh ball(its_make_sphere(r, 2. * M_PI / double(n)));
            ball.translate(pts[i].cast<float>());
            MeshBoolean::cgal::plus(tool, ball);
        }
    } catch (...) {
        return Result::failure("The channel could not be built. Check that its points do not fold back on themselves.");
    }

    Result out;
    out.ok = true;
    out.mesh = std::move(tool);
    return out;
}

Result subtract_channel(const TriangleMesh &solid, const std::vector<Vec3d> &path, const ChannelParams &params)
{
    Result tool = make_channel_tool(path, params);
    if (! tool.ok)
        return tool;
    return boolean_impl(BooleanOp::Subtract, solid, { tool.mesh }, false);
}

Result subtract_channel(ModelObject &object, size_t instance_idx, const std::vector<Vec3d> &path, const ChannelParams &params)
{
    if (instance_idx >= object.instances.size())
        return Result::failure("The object has no such instance.");
    const Transform3d to_object = object.instances[instance_idx]->get_matrix().inverse();
    std::vector<Vec3d> local;
    for (const Vec3d &p : path)
        local.emplace_back(to_object * p);

    Result tool = make_channel_tool(local, params);
    if (! tool.ok)
        return tool;
    const BoundingBoxf3 tool_box = tool.mesh.bounding_box();

    struct Change { size_t idx; bool vanished; TriangleMesh mesh; };
    std::vector<Change> changes;
    bool any_part = false;
    for (size_t i = 0; i < object.volumes.size(); ++ i) {
        if (! object.volumes[i]->is_model_part())
            continue;
        any_part = true;
        TriangleMesh part = volume_mesh_in_object(*object.volumes[i]);
        if (! part.bounding_box().intersects(tool_box))
            continue;
        Result r = boolean_impl(BooleanOp::Subtract, part, { tool.mesh }, true);
        if (! r.ok)
            return Result::failure("Part " + quote_name(object.volumes[i]->name) + ": " + r.error);
        changes.push_back({ i, r.mesh.its.indices.empty(), std::move(r.mesh) });
    }
    if (! any_part)
        return Result::failure("The object has no solid parts.");
    if (changes.empty())
        return Result::failure("The channel does not touch the object.");

    size_t solid_parts = 0;
    for (const ModelVolume *v : object.volumes)
        solid_parts += v->is_model_part();
    if (size_t(std::count_if(changes.begin(), changes.end(), [](const Change &c) { return c.vanished; })) == solid_parts)
        return Result::failure("The channel removes the whole object.");

    std::vector<size_t> to_delete;
    for (Change &c : changes) {
        if (c.vanished)
            to_delete.push_back(c.idx);
        else
            replace_volume_mesh(*object.volumes[c.idx], std::move(c.mesh));
    }
    delete_volumes(object, to_delete);

    Result out;
    out.ok = true;
    return out;
}

} // namespace SolidEdit
} // namespace Slic3r
