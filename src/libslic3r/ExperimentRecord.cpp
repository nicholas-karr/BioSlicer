#include "ExperimentRecord.hpp"

#include <fstream>

#include <boost/filesystem/path.hpp>
#include <boost/nowide/fstream.hpp>
#include "nlohmann/json.hpp"

#include "Model.hpp"
#include "Print.hpp"
#include "PrintConfig.hpp"
#include "libslic3r.h"
#include "libslic3r_version.h"

namespace Slic3r {

namespace {

using json = nlohmann::json;

json config_to_json(const ConfigBase &config)
{
    json out = json::object();
    for (const std::string &key : config.keys())
        out[key] = config.opt_serialize(key);
    return out;
}

json vec3_to_json(const Vec3d &v) { return json::array({ v.x(), v.y(), v.z() }); }

// The droplets as they are dispensed: in bed coordinates, with the extruder of the channel.
json droplets_to_json(const std::vector<ResolvedDroplet> &droplets)
{
    json out = json::array();
    for (const ResolvedDroplet &d : droplets)
        out.push_back({ { "x_mm", d.pos.x() }, { "y_mm", d.pos.y() }, { "z_mm", d.pos.z() }, { "volume_nl", d.volume_nl },
                        { "count", d.count }, { "extruder", d.extruder }, { "dip", d.dip }, { "object", d.object } });
    return out;
}

const char *volume_type_name(ModelVolumeType type)
{
    switch (type) {
    case ModelVolumeType::MODEL_PART:         return "part";
    case ModelVolumeType::NEGATIVE_VOLUME:    return "negative volume";
    case ModelVolumeType::PARAMETER_MODIFIER: return "modifier";
    case ModelVolumeType::SUPPORT_BLOCKER:    return "support blocker";
    case ModelVolumeType::SUPPORT_ENFORCER:   return "support enforcer";
    default:                                  return "other";
    }
}

} // namespace

std::string experiment_record_json(const Print &print)
{
    const DynamicPrintConfig &full = print.full_print_config();
    json record;
    record["record_version"]  = 1;
    record["bioslicer_version"] = SLIC3R_VERSION;

    json presets = json::object();
    for (const char *key : { "print_settings_id", "filament_settings_id", "printer_settings_id" })
        if (full.has(key))
            presets[key] = full.opt_serialize(key);
    record["presets"] = std::move(presets);

    // Every setting whose value is not the built-in default, which includes everything the presets and the user changed.
    const DynamicPrintConfig defaults = DynamicPrintConfig::full_print_config();
    json changed = json::object();
    for (const std::string &key : full.keys()) {
        const ConfigOption *opt = full.option(key);
        const ConfigOption *def = defaults.option(key);
        if (opt != nullptr && (def == nullptr || *opt != *def))
            changed[key] = full.opt_serialize(key);
    }
    record["settings_changed_from_default"] = std::move(changed);

    json objects = json::array();
    for (const ModelObject *object : print.model().objects) {
        json o;
        o["name"]       = object->name;
        o["input_file"] = object->input_file;
        o["settings"]   = config_to_json(object->config.get());
        json instances = json::array();
        for (const ModelInstance *instance : object->instances)
            instances.push_back({ { "printable", instance->printable }, { "offset_mm", vec3_to_json(instance->get_offset()) },
                                  { "rotation_rad", vec3_to_json(instance->get_rotation()) }, { "scale", vec3_to_json(instance->get_scaling_factor()) } });
        o["instances"] = std::move(instances);
        json volumes = json::array();
        for (const ModelVolume *volume : object->volumes) {
            const BoundingBoxf3 box = volume->mesh().bounding_box();
            volumes.push_back({ { "name", volume->name }, { "type", volume_type_name(volume->type()) },
                                { "size_mm", vec3_to_json(box.size()) }, { "settings", config_to_json(volume->config.get()) } });
        }
        o["volumes"]  = std::move(volumes);
        objects.push_back(std::move(o));
    }
    record["objects"]  = std::move(objects);
    record["droplets"] = droplets_to_json(print.droplets());
    return record.dump(2) + "\n";
}

std::string experiment_record_path(const std::string &gcode_path)
{
    boost::filesystem::path path(gcode_path);
    path.replace_extension("experiment.json");
    return path.string();
}

bool write_experiment_record(const Print &print, const std::string &gcode_path)
{
    boost::nowide::ofstream file(experiment_record_path(gcode_path), std::ios::binary);
    file << experiment_record_json(print);
    return bool(file);
}

} // namespace Slic3r
