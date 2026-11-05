#include "SLAMaterial.hpp"

#include <array>

#include "Flow.hpp"
#include "Print.hpp"
#include "PrintConfig.hpp"

namespace Slic3r {

bool is_sla_extruder(const PrintConfig &config, unsigned int extruder_one_based)
{
    return extruder_one_based > 0
        && extruder_one_based <= config.sla_material_extruder.values.size()
        && config.sla_material_extruder.values[extruder_one_based - 1];
}

RegionTechnologyAssignment classify_region_technology(
    const PrintConfig &config,
    const PrintRegion &region)
{
    const std::array<unsigned int, 3> extruders {
        region.extruder(frPerimeter),
        region.extruder(frInfill),
        region.extruder(frSolidInfill)
    };

    const bool first_is_sla = is_sla_extruder(config, extruders.front());
    for (const unsigned int extruder : extruders)
        if (is_sla_extruder(config, extruder) != first_is_sla)
            return { RegionTechnology::Mixed, 0 };

    if (!first_is_sla)
        return { RegionTechnology::FFF, 0 };

    for (const unsigned int extruder : extruders)
        if (extruder != extruders.front())
            return { RegionTechnology::Mixed, 0 };

    return { RegionTechnology::SLA, extruders.front() };
}

std::string material_token(const std::string &channel_type, const std::string &filament_name)
{
    auto normalize = [](const std::string &in) {
        std::string out;
        out.reserve(in.size());
        for (const char c : in) {
            if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))
                out.push_back(c);
            else if (c >= 'a' && c <= 'z')
                out.push_back(char(c - 'a' + 'A'));
            else if (!out.empty() && out.back() != '_')
                out.push_back('_');
        }
        while (!out.empty() && out.back() == '_')
            out.pop_back();
        return out;
    };
    const std::string type = normalize(channel_type);
    const std::string name = normalize(filament_name);
    return type.empty() ? name : type + "_" + name;
}

std::vector<std::string> extruder_material_tokens(const Print &print)
{
    const std::vector<std::string> &channel_types = print.config().channel_type.values;
    // filament_settings_id lives in full_print_config - print.config() doesn't expose it.
    const auto *filament_ids = dynamic_cast<const ConfigOptionStrings *>(
        print.full_print_config().option("filament_settings_id"));
    std::vector<std::string> tokens(print.config().nozzle_diameter.size());
    for (size_t i = 0; i < tokens.size(); ++i)
        tokens[i] = material_token(
            i < channel_types.size() ? channel_types[i] : std::string(),
            (filament_ids && i < filament_ids->values.size()) ? filament_ids->values[i] : std::string());
    return tokens;
}

bool print_has_sla_regions(const Print &print)
{
    for (const PrintObject *object : print.objects())
        for (const PrintRegion &region : object->all_regions())
            if (classify_region_technology(print.config(), region).technology
                == RegionTechnology::SLA)
                return true;
    return false;
}

bool print_has_fff_regions(const Print &print)
{
    for (const PrintObject *object : print.objects())
        for (const PrintRegion &region : object->all_regions())
            if (classify_region_technology(print.config(), region).technology
                == RegionTechnology::FFF)
                return true;
    return false;
}

} // namespace Slic3r
