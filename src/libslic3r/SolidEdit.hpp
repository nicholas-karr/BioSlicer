#ifndef slic3r_SolidEdit_hpp_
#define slic3r_SolidEdit_hpp_

// Solid editing for dimensionally exact designs: boolean operations on meshes, parts and objects,
// applying negative volumes, and cutting channels. All lengths are in millimetres.

#include "TriangleMesh.hpp"
#include "BoundingBox.hpp"
#include "Point.hpp"

#include <string>
#include <vector>

namespace Slic3r {

class Model;
class ModelObject;
class ModelVolume;

namespace SolidEdit {

enum class BooleanOp { Union, Subtract, Intersect };

// Size of a result in bed coordinates, in mm, mm3 and ml.
struct Measurement
{
    bool          valid { false };
    double        volume_mm3 { 0. };
    double        volume_ml { 0. };
    BoundingBoxf3 bbox;

    // For example "Volume 1500.00 mm3 (1.500 ml). Size 15.00 x 10.00 x 10.00 mm, from (0.00, 0.00, 0.00) to (15.00, 10.00, 10.00) mm."
    std::string text() const;
};

// Measures a mesh after transforming it with trafo.
Measurement measure(const TriangleMesh &mesh, const Transform3d &trafo = Transform3d::Identity());
// Measures the union of all solid parts of one instance of an object (volumes of overlapping parts are added).
Measurement measure(const ModelObject &object, size_t instance_idx = 0);

// Outcome of an operation. When ok is false, error holds a plain-language message and nothing was modified.
struct Result
{
    bool        ok { false };
    std::string error;
    TriangleMesh mesh;
    // Size of the result in bed coordinates, filled in by every successful operation on a model.
    Measurement measurement;
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

// Chain of booleans that produced a part: a base shape and the steps applied to it. The shapes are stored in
// the local coordinates of the part, so moving, rotating or scaling the part does not invalidate the chain.
struct HistoryStep
{
    BooleanOp    op { BooleanOp::Union };
    std::string  name;
    bool         enabled { true };
    TriangleMesh mesh;
};

struct History
{
    std::string  base_name;
    TriangleMesh base;
    std::vector<HistoryStep> steps;

    bool empty() const { return base.its.indices.empty(); }
    // One line per entry: "Base: A", "Subtract: B", "Union: C (off)".
    std::vector<std::string> describe() const;
};

// Text form kept in the 3MF file and in ModelVolume::csg_history. Only characters that are safe inside an
// XML attribute are used.
std::string history_to_text(const History &history);
bool        history_from_text(const std::string &text, History &history);

// Recomputes the part from the base and the enabled steps. Fails with a plain message if a step fails.
Result regenerate(const History &history);

// Recomputes the part of volume volume_idx from its stored history and replaces its geometry. The part keeps its
// transformation, name and settings. history is stored on the volume afterwards.
Result regenerate_volume(ModelObject &object, size_t volume_idx, const History &history);

// Reads the history stored on a volume. Returns false if it has none or it cannot be read.
bool get_history(const ModelVolume &volume, History &history);

// Parses a point list, one point per line as "X Y Z" in mm. Blank lines are skipped; any other line that is not
// three numbers is an error naming the line. Commas and semicolons count as spaces.
bool parse_points(const std::string &text, std::vector<Vec3d> &points, std::string &error);

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
