#include "NumericTransform.hpp"

#include "libslic3r/Geometry.hpp"
#include "libslic3r/Model.hpp"

namespace Slic3r {

NumericTransform get_numeric_transform(const ModelObject &object, size_t instance_idx)
{
    NumericTransform ret;
    const ModelInstance *inst = object.instances[instance_idx];
    ret.position_mm  = object.instance_bounding_box(instance_idx).center();
    ret.rotation_deg = inst->get_rotation() * (180. / PI);
    ret.size_mm      = object.raw_mesh_bounding_box().size().cwiseProduct(inst->get_scaling_factor().cwiseAbs());
    return ret;
}

std::string apply_numeric_transform(ModelObject &object, size_t instance_idx, const NumericTransform &t)
{
    if (instance_idx >= object.instances.size())
        return "The object has no such instance.";
    if (!t.position_mm.allFinite() || !t.rotation_deg.allFinite() || !t.size_mm.allFinite())
        return "All values must be numbers.";
    const Vec3d raw = object.raw_mesh_bounding_box().size();
    Vec3d       scale;
    for (int i = 0; i < 3; ++i) {
        if (t.size_mm[i] <= 0.)
            return "The size must be larger than 0 mm in every direction.";
        if (raw[i] <= 0.)
            return "The object is flat along one axis, so it cannot be sized along it.";
        scale[i] = t.size_mm[i] / raw[i];
    }
    ModelInstance *inst = object.instances[instance_idx];
    inst->set_scaling_factor(scale);
    inst->set_rotation(t.rotation_deg * (PI / 180.));
    inst->set_offset(Vec3d::Zero());
    object.invalidate_bounding_box();
    inst->set_offset(t.position_mm - object.instance_bounding_box(instance_idx).center());
    object.invalidate_bounding_box();
    return {};
}

} // namespace Slic3r
