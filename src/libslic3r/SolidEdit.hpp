#ifndef slic3r_SolidEdit_hpp_
#define slic3r_SolidEdit_hpp_

// Solid editing for dimensionally exact designs: boolean operations on meshes, parts and objects,
// applying negative volumes, and cutting channels. All lengths are in millimetres.

#include "TriangleMesh.hpp"
#include "Point.hpp"

#include <string>
#include <vector>

namespace Slic3r {

class Model;
class ModelObject;

namespace SolidEdit {

enum class BooleanOp { Union, Subtract, Intersect };

// Outcome of an operation. When ok is false, error holds a plain-language message and nothing was modified.
struct Result
{
    bool        ok { false };
    std::string error;
    TriangleMesh mesh;
    // Set by boolean_objects() only: the new object, owned by the model.
    ModelObject *object { nullptr };

    static Result failure(std::string msg) { Result r; r.error = std::move(msg); return r; }
};

// Returns an empty string if the mesh is a usable solid (not empty, watertight, not self-intersecting,
// positive volume). Otherwise returns a plain-language reason, prefixed by label.
std::string check_solid(const TriangleMesh &mesh, const std::string &label);

// Base op tools[0] op tools[1] ... The result must be a non-empty solid.
Result boolean(BooleanOp op, const TriangleMesh &base, const std::vector<TriangleMesh> &tools);

// Boolean on the model parts of one object. volume_idxs[0] is the base. The result replaces the base part
// (keeping its name, extruder and settings) and the other parts are removed. Only solid parts are allowed.
Result boolean_volumes(ModelObject &object, BooleanOp op, const std::vector<size_t> &volume_idxs);

// Boolean on whole objects (first instance of each, in bed coordinates). obj_idxs[0] is the base. A new object
// with the base object's settings and first part's extruder is added to the model; the inputs are left in
// place for the caller to remove. Result::object points to the new object.
Result boolean_objects(Model &model, BooleanOp op, const std::vector<size_t> &obj_idxs);

// Subtracts every negative volume from every solid part of the object, then removes the negative volumes.
// Each part keeps its own material. Parts that vanish are removed.
Result apply_negative_volumes(ModelObject &object);

// A channel is a tube of circular cross-section swept along a polyline.
struct ChannelParams
{
    // Tube diameter in mm.
    double diameter { 1.0 };
    // How far the tube is prolonged beyond the first and last point, along the first and last segment, in mm.
    // A positive value that reaches past the surface turns the end into an open port.
    double inlet_extension { 0. };
    double outlet_extension { 0. };
    // Number of sides of the tube polygon. The polygon has the same area as the circle.
    int    sides { 64 };
};

// Builds the solid tube. Straight segments are exact prisms, bends are rounded with spheres.
Result make_channel_tool(const std::vector<Vec3d> &path, const ChannelParams &params);

// Cuts the channel from the mesh. Path points are in the mesh's coordinates.
Result subtract_channel(const TriangleMesh &solid, const std::vector<Vec3d> &path, const ChannelParams &params);

// Cuts the channel from all solid parts of the object. Path points are in bed coordinates and are mapped
// through the inverse of the instance transformation. Parts the channel does not touch are not changed.
Result subtract_channel(ModelObject &object, size_t instance_idx, const std::vector<Vec3d> &path, const ChannelParams &params);

} // namespace SolidEdit
} // namespace Slic3r

#endif // slic3r_SolidEdit_hpp_
