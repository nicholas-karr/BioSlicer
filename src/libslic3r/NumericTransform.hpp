#ifndef slic3r_NumericTransform_hpp_
#define slic3r_NumericTransform_hpp_

#include <string>

#include "libslic3r/Point.hpp"

namespace Slic3r {

class ModelObject;

// Exact numeric placement of an object instance, in millimetres and degrees.
struct NumericTransform
{
    // Centre of the instance bounding box (model parts only) in the bed coordinate system. The instance
    // origin is not used because loading a project may move it without moving the object.
    Vec3d position_mm = Vec3d::Zero();
    // Rotation around X, Y and Z, applied in the same order as the object info panel.
    Vec3d rotation_deg = Vec3d::Zero();
    // Size along the object's own axes (before rotation), in mm. Model parts only, modifiers excluded.
    Vec3d size_mm = Vec3d::Zero();
};

NumericTransform get_numeric_transform(const ModelObject &object, size_t instance_idx);

// Sets position, rotation and size of one instance exactly as given. Returns an empty string on
// success, otherwise a plain-language reason and the instance is left unchanged.
std::string apply_numeric_transform(ModelObject &object, size_t instance_idx, const NumericTransform &transform);

} // namespace Slic3r

#endif // slic3r_NumericTransform_hpp_
