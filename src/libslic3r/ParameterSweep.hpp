#ifndef slic3r_ParameterSweep_hpp_
#define slic3r_ParameterSweep_hpp_

#include <string>
#include <vector>

#include "Droplet.hpp"
#include "DropletPattern.hpp"
#include "TestCoupon.hpp"
#include "TriangleMesh.hpp"

namespace Slic3r {

class Model;
class ModelObject;

// A parameter sweep makes a number of copies of a small test piece, each with one parameter stepped, and arranges
// them on the plate in rows. Every copy has its index engraved in a label strip in front of it, and a table
// of what each copy is can be exported as CSV. All lengths are in millimetres.
enum class SweepSubject {
    // A single wall on a base plate; the thickness of the wall is stepped.
    WallThickness,
    // A block with a single round channel; the diameter of the channel is stepped.
    ChannelDiameter,
    // A block with two round channels; the distance between their centres is stepped.
    ChannelPitch,
    // A round well with its droplet; the volume of the droplet is stepped, in nanolitres.
    DropletVolume,
};

struct SweepParams
{
    SweepSubject subject { SweepSubject::ChannelDiameter };
    // The stepped value of the first copy and the step to the next, in the unit of the subject's parameter.
    double start { 0.6 };
    double step { 0.2 };
    int    count { 6 };

    // The plate the copies are arranged on and the space left between them.
    Vec2d  plate_size { 200., 200. };
    double spacing { 5. };

    // The label strip with the index of the copy.
    bool   engrave_index { true };
    double label_height { 3. };
    double label_thickness { 1.5 };
    double label_depth { 0.6 };

    // What the piece is made of when a parameter is not the stepped one.
    WallThicknessCouponParams wall;
    ChannelCouponParams       channel;
    WellArrayParams           well;
};

struct SweepItem
{
    // 1-based, and the text engraved in the label strip.
    int         index { 0 };
    std::string label;
    double      value { 0. };
    // The copy with its minimum corner at the origin.
    indexed_triangle_set mesh;
    DropletSets          droplets;
    Vec3d       size { Vec3d::Zero() };
    double      volume_mm3 { 0. };
    // The middle of the copy relative to the middle of the arrangement, and on the plate.
    Vec2d       offset { Vec2d::Zero() };
    Vec2d       plate_position { Vec2d::Zero() };
};

struct SweepResult
{
    // Empty on success. On failure no items are made.
    std::string error;
    std::string subject_name;
    std::string parameter;
    std::string unit;
    std::vector<SweepItem> items;
    // The size of the arrangement and whether it fits the plate. Copies that do not fit are still arranged.
    Vec2d       arrangement_size { Vec2d::Zero() };
    bool        fits { true };
    // What each copy is, as CSV: one header line and one line per copy, comma separated, decimal point, '\n' line ends.
    std::string csv;

    bool valid() const { return error.empty() && ! items.empty(); }
};

std::string sweep_subject_name(SweepSubject subject);
std::string sweep_parameter_name(SweepSubject subject);
std::string sweep_parameter_unit(SweepSubject subject);

// Areas of the engraved digits in square millimetres for a label of the given height, for checking an engraving.
double sweep_label_area(const std::string &label, double label_height);

SweepResult make_parameter_sweep(const SweepParams &params);

// Adds one object per copy to the model: the piece with its label as a volume, its droplets, and an instance at the
// middle of the arrangement offset by `center`. `droplet_extruder` is the extruder of the droplet sets.
void add_sweep_to_model(Model &model, const SweepResult &sweep, const Vec2d &center, int droplet_extruder = 0);

} // namespace Slic3r

#endif
