#include "VesselIO.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <queue>
#include <sstream>

#include "LocalesUtils.hpp"

namespace Slic3r { namespace vessel {

bool parse_number(std::string_view text, double &out)
{
    if (text.empty() || text.front() == '+' || text.front() == ' ')
        return false;
    size_t pos = 0;
    const double v = string_to_double_decimal_point(text, &pos);
    if (pos != text.size() || !std::isfinite(v))
        return false;
    out = v;
    return true;
}

namespace {

std::string trim(const std::string &s)
{
    const size_t a = s.find_first_not_of(" \t\r");
    const size_t b = s.find_last_not_of(" \t\r");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

std::vector<std::string> split(const std::string &line)
{
    std::vector<std::string> cells;
    std::istringstream       in(line);
    std::string              cell;
    while (std::getline(in, cell, ','))
        cells.push_back(trim(cell));
    if (!line.empty() && line.back() == ',')
        cells.emplace_back();
    return cells;
}

struct RawBranch
{
    long   parent   = -1;
    Vec3d  point[2] = {Vec3d::Zero(), Vec3d::Zero()};
    double radius[2] = {0., 0.};
    bool   has[2]   = {false, false};
    size_t line     = 0;
};

} // namespace

SkeletonImport skeleton_from_csv(const std::string &text)
{
    SkeletonImport result;
    auto fail = [&](std::string message) {
        result.error = std::move(message);
        result.skeleton = {};
        return result;
    };

    std::istringstream in(text);
    std::string        line;
    size_t             line_no = 0;

    // Header
    std::map<std::string, size_t> column;
    while (std::getline(in, line)) {
        ++line_no;
        if (trim(line).empty())
            continue;
        const std::vector<std::string> names = split(line);
        for (size_t i = 0; i < names.size(); ++i) {
            std::string n = names[i];
            std::transform(n.begin(), n.end(), n.begin(), [](unsigned char c) { return char(std::tolower(c)); });
            column[n] = i;
        }
        break;
    }
    for (const char *needed : {"branch", "parent", "point", "x_mm", "y_mm", "z_mm", "radius_mm"})
        if (column.find(needed) == column.end())
            return fail(std::string("The first line must name the columns branch, parent, point, x_mm, y_mm, z_mm and radius_mm. The column '") +
                        needed + "' is missing.");

    // Rows
    std::map<long, RawBranch> raw;
    while (std::getline(in, line)) {
        ++line_no;
        if (trim(line).empty())
            continue;
        const std::vector<std::string> cells = split(line);
        const std::string where = "Line " + std::to_string(line_no) + ": ";
        for (const auto &[name, index] : column)
            if (index >= cells.size())
                return fail(where + "the row has fewer values than the header.");

        auto number = [&](const char *name, double &v) {
            return parse_number(cells[column[name]], v);
        };
        double id = 0., parent = 0., point = 0., x = 0., y = 0., z = 0., r = 0.;
        if (!number("branch", id) || !number("parent", parent) || !number("point", point))
            return fail(where + "branch, parent and point must be whole numbers.");
        if (!number("x_mm", x) || !number("y_mm", y) || !number("z_mm", z) || !number("radius_mm", r))
            return fail(where + "x_mm, y_mm, z_mm and radius_mm must be numbers written with a decimal point.");
        if (id != std::floor(id) || parent != std::floor(parent) || id < 0 || parent < -1 || id > 1e9 || parent > 1e9)
            return fail(where + "branch must be a whole number from 0 and parent a whole number from -1.");
        if (point != 0. && point != 1.)
            return fail(where + "point must be 0 (start of the branch) or 1 (end of the branch).");
        if (!(r > 0.))
            return fail(where + "the radius must be larger than 0 mm.");

        RawBranch &b = raw[long(id)];
        if (b.line == 0) {
            b.parent = long(parent);
            b.line   = line_no;
            if (raw.size() > MAX_IMPORTED_BRANCHES)
                return fail("There are more than " + std::to_string(MAX_IMPORTED_BRANCHES) + " branches, which is more than can be handled.");
        } else if (b.parent != long(parent)) {
            return fail(where + "branch " + std::to_string(long(id)) + " has two different parents.");
        }
        const int p = int(point);
        if (b.has[p])
            return fail(where + "branch " + std::to_string(long(id)) + " has point " + std::to_string(p) + " twice.");
        b.has[p]    = true;
        b.point[p]  = Vec3d(x, y, z);
        b.radius[p] = r;
    }
    if (raw.empty())
        return fail("The file has no vessels.");

    // Structure
    long root = -2;
    for (const auto &[id, b] : raw) {
        if (!b.has[0] || !b.has[1])
            return fail("Branch " + std::to_string(id) + " needs both a start (point 0) and an end (point 1).");
        if (b.parent == -1) {
            if (root != -2)
                return fail("There is more than one root (branches " + std::to_string(root) + " and " + std::to_string(id) +
                            " have parent -1). A tree has exactly one.");
            root = id;
        } else if (raw.find(b.parent) == raw.end()) {
            return fail("Branch " + std::to_string(id) + " grows out of branch " + std::to_string(b.parent) + ", which is not in the file.");
        } else if (b.parent == id) {
            return fail("Branch " + std::to_string(id) + " is its own parent.");
        }
    }
    if (root == -2)
        return fail("There is no root. One branch must have parent -1.");

    std::map<long, std::vector<long>> children;
    for (const auto &[id, b] : raw)
        if (b.parent != -1)
            children[b.parent].push_back(id);
    for (const auto &[id, c] : children)
        if (c.size() > 2)
            return fail("Branch " + std::to_string(id) + " has " + std::to_string(c.size()) + " child branches. At most 2 are supported.");

    // Breadth first from the root: parents get lower numbers than their children.
    std::map<long, int> new_index;
    std::vector<long>   order;
    std::queue<long>    todo;
    todo.push(root);
    while (!todo.empty()) {
        const long id = todo.front();
        todo.pop();
        new_index[id] = int(order.size());
        order.push_back(id);
        for (long c : children[id])
            todo.push(c);
    }
    if (order.size() != raw.size()) {
        for (const auto &[id, b] : raw)
            if (new_index.find(id) == new_index.end())
                return fail("Branch " + std::to_string(id) + " is not connected to the root (its parents form a loop).");
    }

    Skeleton &sk = result.skeleton;
    sk.branches.resize(order.size());
    for (size_t i = 0; i < order.size(); ++i) {
        const RawBranch &rb = raw[order[i]];
        Branch &b = sk.branches[i];
        b.start   = rb.point[0];
        b.end     = rb.point[1];
        b.radius  = 0.5 * (rb.radius[0] + rb.radius[1]);
        if (std::abs(rb.radius[0] - rb.radius[1]) > 1e-6 * b.radius)
            result.warnings.push_back("Branch " + std::to_string(order[i]) + " has different radii at its two points; the mean " +
                                      float_to_string_decimal_point(b.radius, 3) + " mm is used along its whole length.");
        if (b.length() <= 0.)
            return fail("Branch " + std::to_string(order[i]) + " has the same start and end point.");
        b.parent = rb.parent == -1 ? Branch::NONE : new_index[rb.parent];
    }
    for (size_t i = 0; i < sk.branches.size(); ++i) {
        Branch &b = sk.branches[i];
        if (b.parent == Branch::NONE)
            continue;
        Branch &p = sk.branches[b.parent];
        (p.child_a == Branch::NONE ? p.child_a : p.child_b) = int(i);
        b.depth = p.depth + 1; // parents come first
        const double gap = (b.start - p.end).norm();
        if (gap > 0.01)
            return fail("Branch " + std::to_string(order[i]) + " starts " + float_to_string_decimal_point(gap, 3) + " mm away from the end of its parent " +
                        std::to_string(raw[order[i]].parent) + ". A branch must start where its parent ends (within 0.01 mm).");
        if (gap > 0.)
            b.start = p.end;
    }
    return result;
}

}} // namespace Slic3r::vessel
