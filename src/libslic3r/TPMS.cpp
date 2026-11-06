#include "TPMS.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

#include <openvdb/openvdb.h>
#include <openvdb/tools/Dense.h>
#include <openvdb/tools/VolumeToMesh.h>
#include <tbb/parallel_for.h>

namespace Slic3r {
namespace tpms {

namespace {

constexpr double kTwoPi = 6.28318530717958647692;

// The values of the function on a fine grid over one period, sorted: the quantile of a fraction is the threshold.
const std::vector<float> &sorted_samples(Type type)
{
    static const auto build = [](Type t) {
        constexpr int n = 96;
        std::vector<float> samples;
        samples.reserve(size_t(n) * n * n);
        for (int i = 0; i < n; ++ i)
            for (int j = 0; j < n; ++ j)
                for (int k = 0; k < n; ++ k)
                    samples.emplace_back(float(field(t, Vec3d((i + 0.5) * kTwoPi / n, (j + 0.5) * kTwoPi / n, (k + 0.5) * kTwoPi / n))));
        std::sort(samples.begin(), samples.end());
        return samples;
    };
    static const std::vector<float> schwarz  = build(Type::SchwarzP);
    static const std::vector<float> diamond  = build(Type::Diamond);
    return type == Type::SchwarzP ? schwarz : diamond;
}

} // namespace

double field(Type type, const Vec3d &p)
{
    const double sx = std::sin(p.x()), cx = std::cos(p.x());
    const double sy = std::sin(p.y()), cy = std::cos(p.y());
    const double sz = std::sin(p.z()), cz = std::cos(p.z());
    if (type == Type::SchwarzP)
        return cx + cy + cz;
    return sx * sy * sz + sx * cy * cz + cx * sy * cz + cx * cy * sz;
}

double threshold_for_fraction(Type type, double fraction)
{
    const std::vector<float> &samples = sorted_samples(type);
    const double position = std::clamp(fraction, 0., 1.) * double(samples.size() - 1);
    const size_t lo = size_t(std::floor(position));
    const size_t hi = std::min(lo + 1, samples.size() - 1);
    const double t  = position - double(lo);
    return double(samples[lo]) * (1. - t) + double(samples[hi]) * t;
}

double local_density(const Params &p, double position)
{
    if (p.density_end < 0.)
        return p.density;
    const double length = p.size[std::clamp(p.axis, 0, 2)];
    const double t      = length > 0. ? std::clamp(position / length, 0., 1.) : 0.;
    return p.density + (p.density_end - p.density) * t;
}

double expected_volume_fraction(const Params &p)
{
    return p.density_end < 0. ? p.density : 0.5 * (p.density + p.density_end);
}

indexed_triangle_set make_lattice(const Params &p, std::string *error)
{
    auto fail = [error](const char *message) {
        if (error != nullptr)
            *error = message;
        return indexed_triangle_set();
    };

    const double voxel = p.voxel > 0. ? p.voxel : p.cell / 20.;
    if (p.cell <= 0. || p.size.minCoeff() <= 0.)
        return fail("The size and the cell must be above zero.");
    if (p.size.minCoeff() < p.cell)
        return fail("The box must be at least one cell on every side.");
    if (p.density < 0.02 || p.density > 0.98 || (p.density_end >= 0. && (p.density_end < 0.02 || p.density_end > 0.98)))
        return fail("The density must be between 2 % and 98 %.");
    if (p.axis < 0 || p.axis > 2)
        return fail("The gradient axis must be x, y or z.");

    constexpr int pad = 3;
    Vec3i dims;
    for (int a = 0; a < 3; ++ a)
        dims[a] = int(std::ceil(p.size[a] / voxel)) + 2 * pad;
    if (double(dims.x()) * dims.y() * dims.z() > 5e7)
        return fail("The lattice needs too many samples. Use a larger cell or a smaller box.");

    // Node i sits at origin + i * voxel; the odd offset keeps the faces of the box away from the nodes.
    const double origin = -(pad - 0.63) * voxel;
    const double scale  = p.cell / kTwoPi;

    // Thresholds for the density along the axis, looked up by the cell of the axis a node is in.
    const int           axis = p.axis;
    std::vector<double> thresholds(size_t(dims[axis]));
    for (int i = 0; i < dims[axis]; ++ i)
        thresholds[size_t(i)] = threshold_for_fraction(p.type, local_density(p, origin + i * voxel));

    openvdb::initialize();
    openvdb::tools::Dense<float> dense(openvdb::CoordBBox(openvdb::Coord(0, 0, 0), openvdb::Coord(dims.x() - 1, dims.y() - 1, dims.z() - 1)));
    tbb::parallel_for(0, dims.x(), [&](int i) {
        for (int j = 0; j < dims.y(); ++ j)
            for (int k = 0; k < dims.z(); ++ k) {
                const Vec3d pos(origin + i * voxel, origin + j * voxel, origin + k * voxel);
                const int   idx[3] = { i, j, k };
                const double f = field(p.type, pos * (kTwoPi / p.cell)) - thresholds[size_t(idx[axis])];
                // Negative inside the solid. The box limits the solid with a distance to its faces.
                double box = -pos.x();
                box = std::max(box, pos.x() - p.size.x());
                box = std::max(box, std::max(-pos.y(), pos.y() - p.size.y()));
                box = std::max(box, std::max(-pos.z(), pos.z() - p.size.z()));
                dense.setValue(i, j, k, float(std::max(f * scale, box)));
            }
    });

    openvdb::FloatGrid::Ptr grid = openvdb::FloatGrid::create(1e3f);
    grid->setTransform(openvdb::math::Transform::createLinearTransform(voxel));
    openvdb::tools::copyFromDense(dense, *grid, 0.f);
    grid->transform().postTranslate(openvdb::Vec3d(origin, origin, origin));

    std::vector<openvdb::Vec3s> points;
    std::vector<openvdb::Vec3I> triangles;
    std::vector<openvdb::Vec4I> quads;
    openvdb::tools::volumeToMesh(*grid, points, triangles, quads, 0., 0., true);

    indexed_triangle_set mesh;
    mesh.vertices.reserve(points.size());
    mesh.indices.reserve(triangles.size() + quads.size() * 2);
    for (const openvdb::Vec3s &v : points)
        mesh.vertices.emplace_back(v.x(), v.y(), v.z());
    for (const openvdb::Vec3I &t : triangles)
        mesh.indices.emplace_back(int(t[0]), int(t[1]), int(t[2]));
    for (const openvdb::Vec4I &q : quads) {
        mesh.indices.emplace_back(int(q[2]), int(q[1]), int(q[0]));
        mesh.indices.emplace_back(int(q[3]), int(q[2]), int(q[0]));
    }
    // A solid has a positive volume.
    if (its_volume(mesh) < 0.f)
        for (stl_triangle_vertex_indices &face : mesh.indices)
            std::swap(face[1], face[2]);
    return mesh;
}

} // namespace tpms
} // namespace Slic3r
