#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <memory>
#include <vector>

#include "libslic3r/CSGMesh/CSGMesh.hpp"
#include "libslic3r/CSGMesh/PerformCSGMeshBooleans.hpp"
#include "libslic3r/MeshBoolean.hpp"
#include "libslic3r/TriangleMesh.hpp"

using namespace Slic3r;
using Catch::Approx;

namespace {

using Parts = std::vector<csg::CSGPart>;

// A cube of edge `edge` mm whose min corner is moved to (offset, offset, offset).
csg::CSGPart cube_part(const indexed_triangle_set &cube, float offset, csg::CSGType op,
                       csg::CSGStackOp stack = csg::CSGStackOp::Continue)
{
    csg::CSGPart part(&cube, op, Transform3f(Eigen::Translation3f(offset, offset, offset)));
    part.stack_operation = stack;
    return part;
}

double result_volume(const Parts &parts)
{
    auto cgal = csg::perform_csgmesh_booleans(range(parts));
    REQUIRE(cgal);
    return double(its_volume(MeshBoolean::cgal::cgal_to_indexed_triangle_set(*cgal)));
}

} // namespace

TEST_CASE("CSG parts of two overlapping cubes combine with exact volumes", "[CSGMesh]")
{
    // Two 10 mm cubes overlap in a 5 mm cube: 125 mm^3.
    const indexed_triangle_set cube = its_make_cube(10., 10., 10.);

    Parts parts;
    parts.emplace_back(cube_part(cube, 0.f, csg::CSGType::Union));

    SECTION("Union: 1000 + 1000 - 125") {
        parts.emplace_back(cube_part(cube, 5.f, csg::CSGType::Union));
        CHECK(result_volume(parts) == Approx(1875.).epsilon(1e-4));
    }
    SECTION("Difference: 1000 - 125") {
        parts.emplace_back(cube_part(cube, 5.f, csg::CSGType::Difference));
        CHECK(result_volume(parts) == Approx(875.).epsilon(1e-4));
    }
    SECTION("Intersection: 125") {
        parts.emplace_back(cube_part(cube, 5.f, csg::CSGType::Intersection));
        CHECK(result_volume(parts) == Approx(125.).epsilon(1e-4));
    }
    SECTION("A cube moved away entirely leaves the first one") {
        parts.emplace_back(cube_part(cube, 50.f, csg::CSGType::Difference));
        CHECK(result_volume(parts) == Approx(1000.).epsilon(1e-4));
    }
}

TEST_CASE("CSG push and pop group parts like parentheses", "[CSGMesh]")
{
    // A - (B + C): A = [0,10]^3, B = [5,15]^3, C = [-4,6]^3.
    // B overlaps A in 125 mm^3, C overlaps A in 216 mm^3, and B and C overlap each other in [5,6]^3 = 1 mm^3.
    // Inside A the union of B and C is 125 + 216 - 1 = 340, so the result is 1000 - 340 = 660.
    const indexed_triangle_set cube = its_make_cube(10., 10., 10.);

    Parts parts;
    parts.emplace_back(cube_part(cube, 0.f, csg::CSGType::Union));
    parts.emplace_back(cube_part(cube, 5.f, csg::CSGType::Difference, csg::CSGStackOp::Push));
    parts.emplace_back(cube_part(cube, -4.f, csg::CSGType::Union, csg::CSGStackOp::Pop));
    CHECK(result_volume(parts) == Approx(660.).epsilon(1e-4));
}

TEST_CASE("Positive CSG parts can be merged without booleans", "[CSGMesh]")
{
    const indexed_triangle_set cube = its_make_cube(10., 10., 10.);

    Parts parts;
    parts.emplace_back(cube_part(cube, 0.f, csg::CSGType::Union));
    parts.emplace_back(cube_part(cube, 20.f, csg::CSGType::Union));
    CHECK(csg::is_all_positive(parts));

    // Disjoint cubes: the merged mesh is two shells of 1000 mm^3 each.
    const indexed_triangle_set merged = csg::csgmesh_merge_positive_parts(parts);
    CHECK(merged.indices.size() == 2 * cube.indices.size());
    CHECK(double(its_volume(merged)) == Approx(2000.).epsilon(1e-4));
    const BoundingBoxf3 bb = TriangleMesh(merged).bounding_box();
    CHECK(bb.min.x() == Approx(0.).margin(1e-4));
    CHECK(bb.max.x() == Approx(30.).margin(1e-4));

    // A negative part is not merged and makes the collection not all positive.
    parts.emplace_back(cube_part(cube, 5.f, csg::CSGType::Difference));
    CHECK_FALSE(csg::is_all_positive(parts));
    CHECK(csg::csgmesh_merge_positive_parts(parts).indices.size() == merged.indices.size());
}

TEST_CASE("CSG parts that cannot be combined are reported", "[CSGMesh]")
{
    const indexed_triangle_set cube = its_make_cube(10., 10., 10.);
    indexed_triangle_set open = cube;
    // Dropping a face leaves an open shell that bounds no volume.
    open.indices.pop_back();

    Parts good;
    good.emplace_back(cube_part(cube, 0.f, csg::CSGType::Union));
    CHECK(csg::check_csgmesh_booleans(range(good)) == good.end());

    Parts bad;
    bad.emplace_back(cube_part(cube, 0.f, csg::CSGType::Union));
    bad.emplace_back(cube_part(open, 5.f, csg::CSGType::Difference));
    size_t visited = 0;
    auto it = csg::check_csgmesh_booleans(range(bad), [&visited](auto) { ++visited; });
    CHECK(it == bad.begin() + 1);
    CHECK(visited == 1);
}
