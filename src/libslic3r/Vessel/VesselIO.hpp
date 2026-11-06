#ifndef slic3r_VesselIO_hpp_
#define slic3r_VesselIO_hpp_

#include <string>
#include <string_view>
#include <vector>

#include "libslic3r/Vessel/VesselTree.hpp"

namespace Slic3r { namespace vessel {

// Reads a whole string as a number written with a decimal point. False if there is anything else in it
// or the number is not finite; `out` is not touched then.
bool parse_number(std::string_view text, double &out);

struct SkeletonImport
{
    Skeleton                 skeleton;
    // Things that were adjusted or ignored, in plain language.
    std::vector<std::string> warnings;
    // Empty if the import worked, else the reason it did not, naming the line or the branch.
    std::string              error;

    bool ok() const { return error.empty(); }
};

// Reads the table written by skeleton_to_csv(), or one made by other software or from imaging with the same columns:
// branch,parent,depth,point,x_mm,y_mm,z_mm,radius_mm (the order may differ; depth is optional and recomputed).
// Each branch is a straight vessel given by its start (point 0) and end (point 1). `parent` is the number of the branch
// it grows out of, -1 for the single root. A branch starts where its parent ends (up to 0.01 mm, then it is moved there)
// and a branch has at most two children. A branch with different radii at its two points gets their mean.
// Branches are numbered again so that parents come first, starting with the root.
SkeletonImport skeleton_from_csv(const std::string &text);

// Largest number of branches that is accepted.
constexpr size_t MAX_IMPORTED_BRANCHES = 1000;

}} // namespace Slic3r::vessel

#endif // slic3r_VesselIO_hpp_
