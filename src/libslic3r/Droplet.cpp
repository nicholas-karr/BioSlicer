#include "Droplet.hpp"

#include <algorithm>
#include <cstdio>
#include <limits>
#include <cstdlib>
#include <sstream>

#include "Model.hpp"
#include "MultipleBeds.hpp"
#include "PrintConfig.hpp"
#include "Utils.hpp"

namespace Slic3r {

bool is_droplet_extruder(const PrintConfig &config, unsigned int extruder_one_based)
{
    return extruder_one_based > 0
        && extruder_one_based <= config.channel_type.values.size()
        && config.channel_type.values[extruder_one_based - 1] == "droplet";
}

unsigned int first_droplet_extruder(const PrintConfig &config)
{
    for (unsigned int i = 1; i <= config.channel_type.values.size(); ++i)
        if (is_droplet_extruder(config, i))
            return i;
    return 0;
}

namespace {

constexpr unsigned int not_dispensed = std::numeric_limits<unsigned int>::max();

// `extruder_of` returns the one-based extruder a set is dispensed with, `not_dispensed` for a set that is not.
template<class ExtruderOf>
std::vector<ResolvedDroplet> collect_impl(const Model &model, bool model_sets, ExtruderOf &&extruder_of, size_t *skipped)
{
    std::vector<ResolvedDroplet> out;
    size_t set_idx = 0;

    auto add_set = [&](const DropletSet &set, const Transform3d &trafo, int object, size_t source_set) {
        const unsigned int extruder = extruder_of(set);
        const size_t this_set = set_idx++;
        if (extruder == not_dispensed) {
            if (skipped != nullptr)
                *skipped += set.points.size();
            return;
        }
        for (size_t i = 0; i < set.points.size(); ++i) {
            const DropletPoint &p = set.points[i];
            if (p.count <= 0 || p.volume_nl <= 0.)
                continue;
            ResolvedDroplet d;
            d.pos        = trafo * p.pos;
            d.volume_nl  = p.volume_nl;
            d.count      = p.count;
            d.extruder   = extruder;
            d.dip        = set.dip;
            d.keep_order = set.keep_order;
            d.set        = this_set;
            d.index      = i;
            d.object     = object;
            d.source_set = source_set;
            out.emplace_back(d);
        }
    };

    for (size_t o = 0; o < model.objects.size(); ++o) {
        const ModelObject *object = model.objects[o];
        if (! object->printable || object->droplet_sets.empty())
            continue;
        for (const ModelInstance *instance : object->instances) {
            if (! instance->printable)
                continue;
            for (size_t s = 0; s < object->droplet_sets.size(); ++s)
                add_set(object->droplet_sets[s], instance->get_matrix(), int(o), s);
        }
    }
    if (model_sets)
        for (size_t s = 0; s < model.droplet_sets.size(); ++s)
            add_set(model.droplet_sets[s], Transform3d::Identity(), -1, s);
    return out;
}

} // namespace

std::vector<ResolvedDroplet> collect_droplets(const Model &model, const PrintConfig &config, size_t *skipped)
{
    const unsigned int fallback = first_droplet_extruder(config);
    return collect_impl(model,
        // The droplets of the model itself are on the first bed.
        s_multiple_beds.get_active_bed() == 0,
        [&](const DropletSet &set) -> unsigned int {
            const unsigned int extruder = set.extruder > 0 ? unsigned(set.extruder) : fallback;
            return is_droplet_extruder(config, extruder) ? extruder : not_dispensed;
        }, skipped);
}

std::vector<ResolvedDroplet> collect_droplets_for_display(const Model &model)
{
    // Extruder 0 stands for "the first droplet channel", which is only known with a printer: keep it as it is.
    return collect_impl(model, true, [](const DropletSet &set) -> unsigned int { return unsigned(std::max(0, set.extruder)); }, nullptr);
}

void order_droplets_by_travel(std::vector<ResolvedDroplet> &droplets, const Vec2d &start)
{
    const size_t n = droplets.size();
    if (n < 2)
        return;
    auto dist = [](const Vec2d &a, const Vec2d &b) { return (a - b).norm(); };
    auto xy   = [&](const ResolvedDroplet &d) { return Vec2d(d.pos.x(), d.pos.y()); };

    // Greedy nearest neighbour seed.
    std::vector<ResolvedDroplet> rest = std::move(droplets);
    std::vector<ResolvedDroplet> order;
    order.reserve(n);
    Vec2d cur = start;
    while (! rest.empty()) {
        size_t best = 0;
        double best_d = dist(cur, xy(rest[0]));
        for (size_t i = 1; i < rest.size(); ++i)
            if (const double d = dist(cur, xy(rest[i])); d < best_d - 1e-12) {
                best = i;
                best_d = d;
            }
        cur = xy(rest[best]);
        order.emplace_back(std::move(rest[best]));
        rest.erase(rest.begin() + best);
    }

    // 2-opt on the open path from `start`. Reversing order[i..j] replaces the edges (a,b) and (c,d) by (a,c) and (b,d).
    bool improved = true;
    for (int guard = 0; improved && guard < 40; ++guard) {
        improved = false;
        for (size_t i = 0; i + 1 < n; ++i) {
            for (size_t j = i + 1; j < n; ++j) {
                const Vec2d a = i == 0 ? start : xy(order[i - 1]);
                const Vec2d b = xy(order[i]);
                const Vec2d c = xy(order[j]);
                const bool  has_d = j + 1 < n;
                const double before = dist(a, b) + (has_d ? dist(c, xy(order[j + 1])) : 0.);
                const double after  = dist(a, c) + (has_d ? dist(b, xy(order[j + 1])) : 0.);
                if (after + 1e-9 < before) {
                    std::reverse(order.begin() + i, order.begin() + j + 1);
                    improved = true;
                }
            }
        }
    }
    droplets = std::move(order);
}

double droplet_pulse_us(double volume_nl, double nl_per_us)
{
    return nl_per_us > 0. ? volume_nl / nl_per_us : 0.;
}

std::string droplet_sets_to_text(const DropletSets &sets)
{
    std::string out;
    char buf[256];
    for (const DropletSet &set : sets) {
        // The name takes the rest of the line, so strip line breaks out of it.
        std::string name = set.name;
        for (char &c : name)
            if (c == '\n' || c == '\r')
                c = ' ';
        std::snprintf(buf, sizeof(buf), "set %d %d %d ", set.extruder, set.dip ? 1 : 0, set.keep_order ? 1 : 0);
        out += buf;
        out += name;
        out += '\n';
        for (const DropletPoint &p : set.points) {
            std::snprintf(buf, sizeof(buf), "p %.9g %.9g %.9g %.9g %d\n", p.pos.x(), p.pos.y(), p.pos.z(), p.volume_nl, p.count);
            out += buf;
        }
    }
    return out;
}

DropletSets droplet_sets_from_text(const std::string &text)
{
    DropletSets sets;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (! line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.compare(0, 4, "set ") == 0) {
            DropletSet set;
            int dip = 0, keep = 0, consumed = 0;
            if (std::sscanf(line.c_str() + 4, "%d %d %d %n", &set.extruder, &dip, &keep, &consumed) >= 3) {
                set.dip        = dip != 0;
                set.keep_order = keep != 0;
                set.name       = line.substr(4 + size_t(consumed));
                sets.emplace_back(std::move(set));
            }
        } else if (line.compare(0, 2, "p ") == 0 && ! sets.empty()) {
            DropletPoint p;
            double x, y, z;
            if (std::sscanf(line.c_str() + 2, "%lf %lf %lf %lf %d", &x, &y, &z, &p.volume_nl, &p.count) == 5) {
                p.pos = Vec3d(x, y, z);
                sets.back().points.emplace_back(p);
            }
        }
    }
    return sets;
}

} // namespace Slic3r
