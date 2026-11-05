#ifndef slic3r_SLAMaterial_hpp_
#define slic3r_SLAMaterial_hpp_

#include <string>
#include <vector>

namespace Slic3r {

class PrintConfig;
class Print;
class PrintRegion;

enum class RegionTechnology {
    FFF,
    SLA,
    Mixed
};

struct RegionTechnologyAssignment {
    RegionTechnology technology { RegionTechnology::FFF };
    // One-based extruder identifier. Set only for a valid SLA assignment.
    unsigned int sla_extruder { 0 };
};

bool is_sla_extruder(const PrintConfig &config, unsigned int extruder_one_based);

// A PrintRegion has one geometric mask. Until role-specific SLA masks exist,
// perimeter, sparse infill, and solid infill must all use the same technology;
// SLA roles must additionally use the same SLA channel.
RegionTechnologyAssignment classify_region_technology(
    const PrintConfig &config,
    const PrintRegion &region);

bool print_has_sla_regions(const Print &print);
bool print_has_fff_regions(const Print &print);

// The name a material goes by in G-code: <TYPE>_<NAME>, each part in upper case
// with every run of characters other than ASCII letters and digits collapsed to
// '_'. Empty when both parts are empty. ASCII only, so the result does not
// depend on the process locale.
std::string material_token(const std::string &channel_type, const std::string &filament_name);

// material_token() of every extruder, indexed by extruder id. TYPE comes from
// channel_type, NAME from filament_settings_id.
std::vector<std::string> extruder_material_tokens(const Print &print);

} // namespace Slic3r

#endif
