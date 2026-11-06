#include "SolidEdit.hpp"

#include "BoundingBox.hpp"
#include "MeshBoolean.hpp"
#include "Model.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <set>
#include <sstream>

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

TriangleMesh transformed(const TriangleMesh &mesh, const Transform3d &trafo)
{
    TriangleMesh out = mesh;
    out.transform(trafo, true);
    return out;
}

const char *op_name(BooleanOp op)
{
    return op == BooleanOp::Union ? "Union" : op == BooleanOp::Subtract ? "Subtract" : "Intersect";
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
            // Nothing is left to operate on; Union can never get here.
            if (acc.its.indices.empty())
                break;
            switch (op) {
            case BooleanOp::Union:     MeshBoolean::cgal::plus(acc, tool);      break;
            case BooleanOp::Subtract:  MeshBoolean::cgal::minus(acc, tool);     break;
            case BooleanOp::Intersect: MeshBoolean::cgal::intersect(acc, tool); break;
            }
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

// Replaces the geometry of a volume with a mesh given in object coordinates. Any stored boolean history is
// dropped, callers that keep a history store it again afterwards.
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
    volume.csg_history.clear();
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

// The state of a volume before it is modified, enough to start or continue its history afterwards.
struct Before
{
    Transform3d  matrix;
    TriangleMesh local_mesh;
    std::string  name;
    std::string  history;

    explicit Before(const ModelVolume &v) : matrix(v.get_matrix()), local_mesh(v.mesh()), name(v.name), history(v.csg_history) {}

    // History in object coordinates: the stored one, or a new one starting at the volume as it was.
    History in_object_coords() const
    {
        History h;
        if (history_from_text(history, h)) {
            h.base.transform(matrix, true);
            for (HistoryStep &s : h.steps)
                s.mesh.transform(matrix, true);
        } else {
            h = History();
            h.base_name = name;
            h.base = transformed(local_mesh, matrix);
        }
        return h;
    }
};

// Stores a history given in object coordinates on a volume, converted to the local coordinates of the volume.
void store_history(ModelVolume &volume, History history)
{
    const Transform3d to_local = volume.get_matrix().inverse();
    history.base.transform(to_local, true);
    for (HistoryStep &s : history.steps)
        s.mesh.transform(to_local, true);
    volume.csg_history = history_to_text(history);
}

// Union of all solid parts of an object, transformed by trafo.
Result object_solid(const ModelObject &object, const Transform3d &trafo)
{
    std::vector<TriangleMesh> meshes;
    for (const ModelVolume *v : object.volumes)
        if (v->is_model_part())
            meshes.emplace_back(transformed(v->mesh(), trafo * v->get_matrix()));
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

std::string encode_name(const std::string &name)
{
    std::string out;
    char buf[4];
    for (unsigned char c : name)
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.')
            out += char(c);
        else {
            std::snprintf(buf, sizeof(buf), "%%%02X", unsigned(c));
            out += buf;
        }
    return out;
}

std::string decode_name(const std::string &text)
{
    std::string out;
    for (size_t i = 0; i < text.size(); ++ i)
        if (text[i] == '%' && i + 2 < text.size() + 0 && std::isxdigit((unsigned char)text[i + 1]) && std::isxdigit((unsigned char)text[i + 2])) {
            out += char(std::strtol(text.substr(i + 1, 2).c_str(), nullptr, 16));
            i += 2;
        } else
            out += text[i];
    return out;
}

void mesh_to_text(std::string &out, const TriangleMesh &mesh)
{
    char buf[40];
    out += std::to_string(mesh.its.vertices.size()) + " " + std::to_string(mesh.its.indices.size());
    for (const stl_vertex &v : mesh.its.vertices)
        for (int i = 0; i < 3; ++ i) {
            std::snprintf(buf, sizeof(buf), " %.9g", double(v(i)));
            out += buf;
        }
    for (const stl_triangle_vertex_indices &f : mesh.its.indices)
        for (int i = 0; i < 3; ++ i)
            out += " " + std::to_string(f(i));
}

bool mesh_from_text(const std::string &text, TriangleMesh &mesh)
{
    std::istringstream in(text);
    in.imbue(std::locale::classic());
    size_t nv = 0, ni = 0;
    if (! (in >> nv >> ni) || nv > 100000000 || ni > 100000000)
        return false;
    indexed_triangle_set its;
    its.vertices.resize(nv);
    its.indices.resize(ni);
    for (stl_vertex &v : its.vertices)
        for (int i = 0; i < 3; ++ i) {
            double d;
            if (! (in >> d))
                return false;
            v(i) = float(d);
        }
    for (stl_triangle_vertex_indices &f : its.indices)
        for (int i = 0; i < 3; ++ i) {
            long idx;
            if (! (in >> idx) || idx < 0 || size_t(idx) >= nv)
                return false;
            f(i) = int(idx);
        }
    mesh = TriangleMesh(std::move(its));
    return true;
}

} // namespace

std::string Measurement::text() const
{
    if (! valid)
        return {};
    const Vec3d size = bbox.size();
    char buf[400];
    std::snprintf(buf, sizeof(buf),
        "Volume %.2f mm3 (%.3f ml). Size %.2f x %.2f x %.2f mm, from (%.2f, %.2f, %.2f) to (%.2f, %.2f, %.2f) mm.",
        volume_mm3, volume_ml, size.x(), size.y(), size.z(),
        bbox.min.x(), bbox.min.y(), bbox.min.z(), bbox.max.x(), bbox.max.y(), bbox.max.z());
    return buf;
}

Measurement measure(const TriangleMesh &mesh, const Transform3d &trafo)
{
    Measurement m;
    if (mesh.its.indices.empty())
        return m;
    const TriangleMesh t = transformed(mesh, trafo);
    m.valid      = true;
    m.volume_mm3 = double(its_volume(t.its));
    m.volume_ml  = m.volume_mm3 / 1000.;
    m.bbox       = t.bounding_box();
    return m;
}

Measurement measure(const ModelObject &object, size_t instance_idx)
{
    Measurement total;
    if (instance_idx >= object.instances.size())
        return total;
    const Transform3d inst = object.instances[instance_idx]->get_matrix();
    for (const ModelVolume *v : object.volumes) {
        if (! v->is_model_part())
            continue;
        const Measurement m = measure(v->mesh(), inst * v->get_matrix());
        if (! m.valid)
            continue;
        total.volume_mm3 += m.volume_mm3;
        if (total.valid)
            total.bbox.merge(m.bbox);
        else
            total.bbox = m.bbox;
        total.valid = true;
    }
    total.volume_ml = total.volume_mm3 / 1000.;
    return total;
}

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
    Result r = boolean_impl(op, base, tools, false);
    if (r.ok)
        r.measurement = measure(r.mesh);
    return r;
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
    History history = Before(*base).in_object_coords();
    for (size_t i = 1; i < volume_idxs.size(); ++ i) {
        HistoryStep step;
        step.op   = op;
        step.name = object.volumes[volume_idxs[i]]->name;
        step.mesh = tools[i - 1];
        history.steps.emplace_back(std::move(step));
    }

    replace_volume_mesh(*base, TriangleMesh(result.mesh));
    store_history(*base, std::move(history));
    delete_volumes(object, std::vector<size_t>(volume_idxs.begin() + 1, volume_idxs.end()));
    result.measurement = measure(object);
    return result;
}

Result boolean_objects(Model &model, BooleanOp op, const std::vector<size_t> &obj_idxs)
{
    if (obj_idxs.size() < 2)
        return Result::failure("Select at least two objects.");
    if (std::set<size_t>(obj_idxs.begin(), obj_idxs.end()).size() != obj_idxs.size())
        return Result::failure("The same object was selected twice.");
    for (size_t idx : obj_idxs) {
        if (idx >= model.objects.size() || model.objects[idx]->instances.empty())
            return Result::failure("A selected object does not exist.");
        if (model.objects[idx]->instances.size() > 1)
            return Result::failure("The object " + quote_name(model.objects[idx]->name) + " has several instances. "
                                   "Make them separate objects first.");
    }

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
    for (ModelVolume *v : obj->volumes)
        delete v;
    obj->volumes.clear();
    obj->layer_config_ranges.clear();

    const Transform3d base_inst = base_obj.instances.front()->get_matrix();
    ModelVolume *nv = obj->add_volume(transformed(result.mesh, base_inst.inverse()), ModelVolumeType::MODEL_PART);
    if (tpl != nullptr) {
        nv->name = tpl->name;
        nv->config.assign_config(tpl->config);
    }
    obj->input_file.clear();

    // Center the object around its origin without moving it on the bed. The shift is in object coordinates,
    // so the instance offset changes by the linear part of the instance transformation applied to it.
    obj->origin_translation = Vec3d::Zero();
    const Vec3d shift = -obj->raw_mesh_bounding_box().center();
    obj->translate(shift);
    ModelInstance *inst = obj->instances.front();
    inst->set_offset(inst->get_offset() - inst->get_matrix().linear() * shift);
    obj->invalidate_bounding_box();

    // The history is kept in the coordinates of the new object, which are the bed coordinates mapped
    // through the inverse of its instance transformation.
    const Transform3d to_object = inst->get_matrix().inverse();
    History history;
    history.base_name = base_obj.name;
    history.base      = transformed(base, to_object);
    for (size_t i = 1; i < obj_idxs.size(); ++ i) {
        HistoryStep step;
        step.op   = op;
        step.name = model.objects[obj_idxs[i]]->name;
        step.mesh = transformed(solids[i - 1], to_object);
        history.steps.emplace_back(std::move(step));
    }
    store_history(*nv, std::move(history));

    result.object      = obj;
    result.measurement = measure(*obj);
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

    struct Change { size_t idx; bool vanished; TriangleMesh mesh; History history; };
    std::vector<Change> changes;
    for (size_t idx : parts) {
        TriangleMesh part = volume_mesh_in_object(*object.volumes[idx]);
        const BoundingBoxf3 box = part.bounding_box();
        std::vector<TriangleMesh> tools;
        std::vector<std::string> tool_names;
        for (size_t i = 0; i < neg_meshes.size(); ++ i)
            if (box.intersects(neg_boxes[i])) {
                tools.push_back(neg_meshes[i]);
                tool_names.push_back(object.volumes[negatives[i]]->name);
            }
        if (tools.empty())
            continue;
        Result r = boolean_impl(BooleanOp::Subtract, part, tools, true);
        if (! r.ok)
            return Result::failure("Part " + quote_name(object.volumes[idx]->name) + ": " + r.error);
        History history = Before(*object.volumes[idx]).in_object_coords();
        for (size_t i = 0; i < tools.size(); ++ i) {
            HistoryStep step;
            step.op   = BooleanOp::Subtract;
            step.name = tool_names[i];
            step.mesh = tools[i];
            history.steps.emplace_back(std::move(step));
        }
        changes.push_back({ idx, r.mesh.its.indices.empty(), std::move(r.mesh), std::move(history) });
    }

    const size_t vanished = std::count_if(changes.begin(), changes.end(), [](const Change &c) { return c.vanished; });
    if (vanished == parts.size())
        return Result::failure("The negative volumes remove the whole object.");

    std::vector<size_t> to_delete = negatives;
    for (Change &c : changes) {
        if (c.vanished)
            to_delete.push_back(c.idx);
        else {
            replace_volume_mesh(*object.volumes[c.idx], std::move(c.mesh));
            store_history(*object.volumes[c.idx], std::move(c.history));
        }
    }
    delete_volumes(object, to_delete);

    Result out;
    out.ok = true;
    out.measurement = measure(object);
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

    const size_t m = pts.size() - 1; // number of segments
    std::vector<Vec3d> dirs(m);
    for (size_t i = 0; i < m; ++ i)
        dirs[i] = (pts[i + 1] - pts[i]).normalized();
    // Bends sharper than 150 degrees would need mitres several diameters long.
    for (size_t i = 0; i + 1 < m; ++ i)
        if (dirs[i].dot(dirs[i + 1]) < std::cos(M_PI * 150. / 180.))
            return Result::failure("The channel turns back on itself too sharply at point " + std::to_string(i + 2) +
                                   ". Use a gentler bend (less than 150 degrees).");

    // The tube is one swept surface: a ring of n points at every path point, lying in the plane that bisects
    // the two neighbouring segments (perpendicular to the segment at the ends). Each ring is the previous ring
    // moved along the segment onto the next plane, so neighbouring segments share their ring exactly, the
    // surface is watertight without any boolean, and the volume is exactly the circle area times the length of
    // the path, bends included. The polygon has the area of the circle.
    const int    n      = params.sides;
    const double r      = 0.5 * params.diameter;
    const double r_poly = r * std::sqrt(2. * M_PI / (double(n) * std::sin(2. * M_PI / double(n))));

    const Vec3d helper = std::abs(dirs[0].x()) < 0.9 ? Vec3d::UnitX() : Vec3d::UnitY();
    const Vec3d u = dirs[0].cross(helper).normalized();
    const Vec3d v = dirs[0].cross(u);

    std::vector<std::vector<Vec3d>> rings(m + 1, std::vector<Vec3d>(size_t(n)));
    for (int j = 0; j < n; ++ j) {
        const double t = 2. * M_PI * double(j) / double(n);
        rings[0][size_t(j)] = pts[0] + r_poly * (std::cos(t) * u + std::sin(t) * v);
    }
    for (size_t k = 1; k <= m; ++ k) {
        const Vec3d plane_normal = k == m ? Vec3d(dirs[m - 1]) : Vec3d((dirs[k - 1] + dirs[k]).normalized());
        for (int j = 0; j < n; ++ j) {
            const Vec3d &from = rings[k - 1][size_t(j)];
            const double dist = plane_normal.dot(pts[k] - from) / plane_normal.dot(dirs[k - 1]);
            rings[k][size_t(j)] = from + dist * dirs[k - 1];
        }
    }

    indexed_triangle_set its;
    its.vertices.reserve((m + 1) * size_t(n) + 2);
    for (const std::vector<Vec3d> &ring : rings)
        for (const Vec3d &p : ring)
            its.vertices.emplace_back(p.cast<float>());
    const int first_center = int(its.vertices.size());
    its.vertices.emplace_back(pts.front().cast<float>());
    its.vertices.emplace_back(pts.back().cast<float>());
    for (size_t k = 0; k < m; ++ k)
        for (int j = 0; j < n; ++ j) {
            const int jn = (j + 1) % n;
            const int a_j = int(k) * n + j, a_n = int(k) * n + jn, b_j = int(k + 1) * n + j, b_n = int(k + 1) * n + jn;
            its.indices.emplace_back(a_j, a_n, b_n);
            its.indices.emplace_back(a_j, b_n, b_j);
        }
    for (int j = 0; j < n; ++ j) {
        const int jn = (j + 1) % n;
        its.indices.emplace_back(first_center, jn, j);
        its.indices.emplace_back(first_center + 1, int(m) * n + j, int(m) * n + jn);
    }

    Result out;
    out.mesh = TriangleMesh(std::move(its));
    if (! check_solid(out.mesh, "The channel").empty())
        return Result::failure("The channel path bends too sharply or its segments are too short for this diameter. "
                               "Use longer segments or a smaller diameter.");
    out.ok = true;
    out.measurement = measure(out.mesh);
    return out;
}

Result subtract_channel(const TriangleMesh &solid, const std::vector<Vec3d> &path, const ChannelParams &params)
{
    Result tool = make_channel_tool(path, params);
    if (! tool.ok)
        return tool;
    Result r = boolean_impl(BooleanOp::Subtract, solid, { tool.mesh }, false);
    if (r.ok)
        r.measurement = measure(r.mesh);
    return r;
}

Result subtract_channel(ModelObject &object, size_t instance_idx, const std::vector<Vec3d> &path, const ChannelParams &params)
{
    if (instance_idx >= object.instances.size())
        return Result::failure("The object has no such instance.");
    // The parts are shared by all instances, so a channel placed through one instance would show up in the
    // wrong place on the others.
    if (object.instances.size() > 1)
        return Result::failure("This object has several instances, which share one shape. Make them separate "
                               "objects before cutting a channel.");

    // The tube is built and cut in bed coordinates, so its diameter stays round and exact even when the
    // instance is scaled or rotated. The cut parts are mapped back to object coordinates afterwards.
    const Transform3d instance_trafo = object.instances[instance_idx]->get_matrix();
    const Transform3d to_object      = instance_trafo.inverse();

    Result tool = make_channel_tool(path, params);
    if (! tool.ok)
        return tool;
    const BoundingBoxf3 tool_box = tool.mesh.bounding_box();

    char tool_label[64];
    std::snprintf(tool_label, sizeof(tool_label), "Channel %.3g mm", params.diameter);

    struct Change { size_t idx; bool vanished; TriangleMesh mesh; History history; };
    std::vector<Change> changes;
    bool any_part = false;
    for (size_t i = 0; i < object.volumes.size(); ++ i) {
        if (! object.volumes[i]->is_model_part())
            continue;
        any_part = true;
        TriangleMesh part = transformed(object.volumes[i]->mesh(), instance_trafo * object.volumes[i]->get_matrix());
        if (! part.bounding_box().intersects(tool_box))
            continue;
        Result r = boolean_impl(BooleanOp::Subtract, part, { tool.mesh }, true);
        if (! r.ok)
            return Result::failure("Part " + quote_name(object.volumes[i]->name) + ": " + r.error);
        r.mesh.transform(to_object, true);
        History history = Before(*object.volumes[i]).in_object_coords();
        HistoryStep step;
        step.op   = BooleanOp::Subtract;
        step.name = tool_label;
        step.mesh = transformed(tool.mesh, to_object);
        history.steps.emplace_back(std::move(step));
        changes.push_back({ i, r.mesh.its.indices.empty(), std::move(r.mesh), std::move(history) });
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
        else {
            replace_volume_mesh(*object.volumes[c.idx], std::move(c.mesh));
            store_history(*object.volumes[c.idx], std::move(c.history));
        }
    }
    delete_volumes(object, to_delete);

    Result out;
    out.ok = true;
    out.measurement = measure(object, instance_idx);
    return out;
}

std::vector<std::string> History::describe() const
{
    std::vector<std::string> lines;
    lines.emplace_back("Base: " + base_name);
    for (const HistoryStep &s : steps)
        lines.emplace_back(std::string(op_name(s.op)) + ": " + s.name + (s.enabled ? "" : " (off)"));
    return lines;
}

std::string history_to_text(const History &history)
{
    std::string out = "csg1;" + encode_name(history.base_name) + ";";
    mesh_to_text(out, history.base);
    out += ";" + std::to_string(history.steps.size());
    for (const HistoryStep &s : history.steps) {
        out += ";" + std::to_string(int(s.op)) + ";" + (s.enabled ? "1" : "0") + ";" + encode_name(s.name) + ";";
        mesh_to_text(out, s.mesh);
    }
    return out;
}

bool history_from_text(const std::string &text, History &history)
{
    std::vector<std::string> fields;
    size_t start = 0;
    while (true) {
        const size_t end = text.find(';', start);
        fields.emplace_back(text.substr(start, end == std::string::npos ? std::string::npos : end - start));
        if (end == std::string::npos)
            break;
        start = end + 1;
    }
    if (fields.size() < 4 || fields[0] != "csg1")
        return false;
    History h;
    h.base_name = decode_name(fields[1]);
    if (! mesh_from_text(fields[2], h.base))
        return false;
    const long count = std::strtol(fields[3].c_str(), nullptr, 10);
    if (count < 0 || fields.size() != size_t(4 + 4 * count))
        return false;
    for (long i = 0; i < count; ++ i) {
        const size_t f = size_t(4 + 4 * i);
        HistoryStep s;
        const long op = std::strtol(fields[f].c_str(), nullptr, 10);
        if (op < 0 || op > 2)
            return false;
        s.op      = BooleanOp(op);
        s.enabled = fields[f + 1] == "1";
        s.name    = decode_name(fields[f + 2]);
        if (! mesh_from_text(fields[f + 3], s.mesh))
            return false;
        h.steps.emplace_back(std::move(s));
    }
    history = std::move(h);
    return true;
}

Result regenerate(const History &history)
{
    if (history.empty())
        return Result::failure("There is no boolean history.");
    TriangleMesh acc = history.base;
    if (std::string err = check_solid(acc, "The base shape " + quote_name(history.base_name)); ! err.empty())
        return Result::failure(err);
    size_t number = 0;
    for (const HistoryStep &s : history.steps) {
        ++ number;
        if (! s.enabled)
            continue;
        Result r = boolean_impl(s.op, acc, { s.mesh }, false);
        if (! r.ok)
            return Result::failure("Step " + std::to_string(number) + " (" + op_name(s.op) + " " + quote_name(s.name) + "): " + r.error);
        acc = std::move(r.mesh);
    }
    Result out;
    out.ok = true;
    out.mesh = std::move(acc);
    out.measurement = measure(out.mesh);
    return out;
}

Result regenerate_volume(ModelObject &object, size_t volume_idx, const History &history)
{
    if (volume_idx >= object.volumes.size() || ! object.volumes[volume_idx]->is_model_part())
        return Result::failure("The selected part does not exist or is not a solid part.");
    Result r = regenerate(history);
    if (! r.ok)
        return r;

    ModelVolume &v = *object.volumes[volume_idx];
    v.set_mesh(TriangleMesh(r.mesh));
    v.calculate_convex_hull();
    v.supported_facets.reset();
    v.seam_facets.reset();
    v.mm_segmentation_facets.reset();
    v.fuzzy_skin_facets.reset();
    v.csg_history = history_to_text(history);
    object.input_file.clear();
    object.invalidate_bounding_box();
    r.measurement = measure(object);
    return r;
}

bool get_history(const ModelVolume &volume, History &history)
{
    return ! volume.csg_history.empty() && history_from_text(volume.csg_history, history);
}

bool parse_points(const std::string &text, std::vector<Vec3d> &points, std::string &error)
{
    points.clear();
    std::istringstream lines(text);
    std::string line;
    int line_no = 0;
    while (std::getline(lines, line)) {
        ++ line_no;
        for (char &c : line)
            if (c == ',' || c == ';' || c == '\t' || c == '\r')
                c = ' ';
        if (line.find_first_not_of(' ') == std::string::npos)
            continue;
        std::istringstream fields(line);
        fields.imbue(std::locale::classic());
        double x, y, z;
        std::string rest;
        if (! (fields >> x >> y >> z) || (fields >> rest)) {
            error = "Line " + std::to_string(line_no) + " is not three numbers (X Y Z in mm).";
            points.clear();
            return false;
        }
        points.emplace_back(x, y, z);
    }
    if (points.size() < 2) {
        error = "A channel needs at least two points.";
        return false;
    }
    return true;
}

} // namespace SolidEdit
} // namespace Slic3r
