#ifndef slic3r_Droplet_hpp_
#define slic3r_Droplet_hpp_

#include <string>
#include <vector>

#include "libslic3r.h"
#include "Point.hpp"

namespace cereal { class access; }

namespace Slic3r {

class Model;
class PrintConfig;

// One droplet: a discrete point where `count` drops of `volume_nl` each are dispensed.
// `pos.z()` is the landing height, the height the dispensed liquid ends up at.
struct DropletPoint
{
    Vec3d  pos { Vec3d::Zero() };
    double volume_nl { 50. };
    int    count { 1 };

    bool operator==(const DropletPoint &rhs) const
        { return pos == rhs.pos && volume_nl == rhs.volume_nl && count == rhs.count; }
    bool operator!=(const DropletPoint &rhs) const { return ! (*this == rhs); }

    template<class Archive> void serialize(Archive &ar) { ar(pos.x(), pos.y(), pos.z(), volume_nl, count); }
};

// A set of droplet points dispensed by one droplet channel.
struct DropletSet
{
    std::string  name;
    // One-based extruder of a droplet channel. 0 = the first droplet channel of the printer.
    int          extruder { 0 };
    // Descend to the landing height plus the standoff at every point instead of staying above the print.
    bool         dip { false };
    // Fire in the authored order instead of the order of shortest travel.
    bool         keep_order { false };
    std::vector<DropletPoint> points;

    bool operator==(const DropletSet &rhs) const
        { return name == rhs.name && extruder == rhs.extruder && dip == rhs.dip && keep_order == rhs.keep_order && points == rhs.points; }
    bool operator!=(const DropletSet &rhs) const { return ! (*this == rhs); }

    template<class Archive> void serialize(Archive &ar) { ar(name, extruder, dip, keep_order, points); }
};

using DropletSets = std::vector<DropletSet>;

// A droplet in bed coordinates, as the G-code export sees it.
struct ResolvedDroplet
{
    Vec3d    pos { Vec3d::Zero() };
    double   volume_nl { 0. };
    int      count { 1 };
    // One-based extruder of the droplet channel, always valid for a list returned by collect_droplets().
    unsigned extruder { 0 };
    bool     dip { false };
    bool     keep_order { false };
    // Index of the set in the order sets were collected, so the order within a set can be kept.
    size_t   set { 0 };
    // Index of the point within its set.
    size_t   index { 0 };
    // Where the point is stored: the index of the model object, -1 for the model itself, and the index of the set
    // within that object or model.
    int      object { -1 };
    size_t   source_set { 0 };

    bool operator==(const ResolvedDroplet &rhs) const
        { return pos == rhs.pos && volume_nl == rhs.volume_nl && count == rhs.count && extruder == rhs.extruder
              && dip == rhs.dip && keep_order == rhs.keep_order && set == rhs.set && index == rhs.index
              && object == rhs.object && source_set == rhs.source_set; }
    bool operator!=(const ResolvedDroplet &rhs) const { return ! (*this == rhs); }
};

bool is_droplet_extruder(const PrintConfig &config, unsigned int extruder_one_based);

// One-based extruder of the first droplet channel of the printer, 0 if there is none.
unsigned int first_droplet_extruder(const PrintConfig &config);

// All droplets of the model in bed coordinates: the sets of every printable instance of every printable
// object (transformed by the instance) followed by the sets of the model itself. A set without extruder is
// assigned to the first droplet channel; a set whose extruder is not a droplet channel is dropped, as the
// printer cannot dispense with it. The number of points of such sets is added to *skipped, if given
// (Print::validate reports it).
std::vector<ResolvedDroplet> collect_droplets(const Model &model, const PrintConfig &config, size_t *skipped = nullptr);

// All droplets of the model in bed coordinates, for showing them: no printer is needed, so a set keeps its extruder,
// 0 included, and the droplets of every bed are listed.
std::vector<ResolvedDroplet> collect_droplets_for_display(const Model &model);

// Reorders droplets for the shortest travel, starting from `start`: nearest neighbour followed by 2-opt.
void order_droplets_by_travel(std::vector<ResolvedDroplet> &droplets, const Vec2d &start);

// Pulse time of a droplet in microseconds: its volume divided by the volume per pulse of the material.
// Returns 0 for a material without calibration.
double droplet_pulse_us(double volume_nl, double nl_per_us);

// Text form of droplet sets, stored in 3MF files. Lines:
//   set <extruder> <dip> <keep_order> <name>
//   p <x> <y> <z> <volume_nl> <count>
std::string  droplet_sets_to_text(const DropletSets &sets);
DropletSets  droplet_sets_from_text(const std::string &text);

} // namespace Slic3r

#endif
