#ifndef slic3r_ExperimentRecord_hpp_
#define slic3r_ExperimentRecord_hpp_

#include <string>

namespace Slic3r {

class Print;

// The experiment record is a JSON file written next to an exported G-code file when the print setting
// "experiment_record" is on. It holds what is needed to make the same print again: the BioSlicer version, the names
// of the print, filament and printer presets, every setting that differs from the built-in default, and for each
// object its name, source file, placement, its own overridden settings and its parts with their overrides, and
// every droplet in bed coordinates. Lengths are in millimetres, volumes in nanolitres.
std::string experiment_record_json(const Print &print);

// "dir/name.gcode" gives "dir/name.experiment.json".
std::string experiment_record_path(const std::string &gcode_path);

// Writes the record next to the G-code file. Returns false when the file cannot be written.
bool write_experiment_record(const Print &print, const std::string &gcode_path);

} // namespace Slic3r

#endif
