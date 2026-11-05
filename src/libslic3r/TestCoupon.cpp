#include "TestCoupon.hpp"

#include <algorithm>
#include <cmath>

#include "ClipperUtils.hpp"
#include "DropletPattern.hpp"
#include "Polygon.hpp"

namespace Slic3r {

namespace {

constexpr double kPi = 3.14159265358979323846;

Polygon rectangle(double x0, double y0, double x1, double y1)
{
    Polygon rect;
    rect.points = { scaled<coord_t>(Vec2d(x0, y0)), scaled<coord_t>(Vec2d(x1, y0)),
                    scaled<coord_t>(Vec2d(x1, y1)), scaled<coord_t>(Vec2d(x0, y1)) };
    return rect;
}

Polygon circle(double cx, double cy, double diameter)
{
    Polygon c = make_circle_num_segments(scale_(diameter * 0.5), kCouponCircleSegments);
    c.translate(scaled<coord_t>(cx), scaled<coord_t>(cy));
    return c;
}

Coupon failed(std::string message)
{
    Coupon coupon;
    coupon.error = std::move(message);
    return coupon;
}

// The extrusion of a footprint drawn in the x-z plane along +y from y = 0 to y = depth.
indexed_triangle_set extrude_xz(const ExPolygons &footprint, double depth)
{
    indexed_triangle_set mesh = make_wall_mesh(footprint, 0., depth);
    // A turn of 90 degrees about x takes (x, y, z) to (x, -z, y): the footprint's y becomes z and the extrusion runs along -y.
    for (stl_vertex &v : mesh.vertices)
        v = stl_vertex(v.x(), float(depth) - v.z(), v.y());
    return mesh;
}

std::vector<double> stepped(double first, double step, int count)
{
    std::vector<double> values;
    for (int i = 0; i < count; ++ i)
        values.emplace_back(first + i * step);
    return values;
}

} // namespace

Coupon make_wall_thickness_coupon(const WallThicknessCouponParams &p)
{
    if (p.count < 1)
        return failed("At least one wall is needed.");
    if (p.first_thickness <= 0. || p.first_thickness + (p.count - 1) * p.step < 0.05)
        return failed("The thinnest wall must be at least 0.05 mm.");
    if (p.wall_length <= 0. || p.wall_height <= 0. || p.base_thickness <= 0. || p.gap <= 0.)
        return failed("Lengths must be above zero.");

    Coupon coupon;
    coupon.parameter = "wall thickness";
    coupon.unit      = "mm";
    coupon.values    = stepped(p.first_thickness, p.step, p.count);

    double total = p.gap;
    for (double t : coupon.values)
        total += t + p.gap;
    const double depth = p.wall_length + 2. * p.gap;

    Polygons walls;
    double x = p.gap;
    for (double t : coupon.values) {
        walls.emplace_back(rectangle(x, p.gap, x + t, p.gap + p.wall_length));
        x += t + p.gap;
    }
    coupon.mesh = make_wall_mesh({ ExPolygon(rectangle(0., 0., total, depth)) }, 0., p.base_thickness);
    its_merge(coupon.mesh, make_wall_mesh(union_ex(walls), p.base_thickness, p.base_thickness + p.wall_height));
    return coupon;
}

Coupon make_channel_coupon(const ChannelCouponParams &p)
{
    if (p.count < 1)
        return failed("At least one channel is needed.");
    const std::vector<double> widths = stepped(p.first_width, p.step, p.count);
    const double narrowest = *std::min_element(widths.begin(), widths.end());
    const double widest    = *std::max_element(widths.begin(), widths.end());
    if (narrowest < 0.05)
        return failed("The narrowest channel must be at least 0.05 mm.");
    if (p.wall <= 0. || p.length <= 0.)
        return failed("Lengths must be above zero.");
    const double pitch = p.pitch > 0. ? p.pitch : widest + p.wall;
    if (p.count > 1 && pitch < widest + 0.1)
        return failed("The pitch must leave at least 0.1 mm between the widest channels.");

    Coupon coupon;
    coupon.parameter = "channel width";
    coupon.unit      = "mm";
    coupon.values    = widths;

    const double width  = (p.count - 1) * pitch + widest + 2. * p.wall;
    const double height = widest + 2. * p.wall;
    Polygons holes;
    for (int i = 0; i < p.count; ++ i) {
        const double cx = p.wall + widest * 0.5 + i * pitch;
        const double cz = height * 0.5;
        if (p.profile == ChannelProfile::Round)
            holes.emplace_back(circle(cx, cz, widths[i]));
        else
            holes.emplace_back(rectangle(cx - widths[i] * 0.5, cz - widths[i] * 0.5, cx + widths[i] * 0.5, cz + widths[i] * 0.5));
    }
    coupon.mesh = extrude_xz(diff_ex(Polygons{ rectangle(0., 0., width, height) }, holes), p.length);
    return coupon;
}

Coupon make_overhang_coupon(const OverhangCouponParams &p)
{
    if (p.count < 1)
        return failed("At least one arm is needed.");
    const std::vector<double> angles = stepped(p.first_angle, p.step, p.count);
    for (double a : angles)
        if (a <= 0. || a > 85.)
            return failed("Overhang angles must be between 0 and 85 degrees.");
    if (p.arm_length <= 0. || p.arm_thickness <= 0. || p.post_width <= 0. || p.depth <= 0. || p.base_thickness <= 0. || p.gap <= 0.)
        return failed("Lengths must be above zero.");
    if (p.arm_start + p.arm_thickness > p.post_height)
        return failed("The arm must leave the post below its top.");

    Coupon coupon;
    coupon.parameter = "overhang angle";
    coupon.unit      = "degrees";
    coupon.values    = angles;

    // Arms lean to +x, so a station is a post and the horizontal reach of its arm.
    Polygons stations;
    double x = p.gap;
    for (double angle : angles) {
        const double a  = angle * kPi / 180.;
        const double dx = p.arm_length * std::sin(a);
        const double dz = p.arm_length * std::cos(a);
        const double x0 = x + p.post_width;
        Polygon arm;
        arm.points = { scaled<coord_t>(Vec2d(x0, p.arm_start)), scaled<coord_t>(Vec2d(x0 + dx, p.arm_start + dz)),
                       scaled<coord_t>(Vec2d(x0 + dx, p.arm_start + dz + p.arm_thickness)), scaled<coord_t>(Vec2d(x0, p.arm_start + p.arm_thickness)) };
        stations.emplace_back(rectangle(x, 0., x0, p.post_height));
        stations.emplace_back(std::move(arm));
        x = x0 + dx + p.gap;
    }
    const double total = x;

    coupon.mesh = make_wall_mesh({ ExPolygon(rectangle(0., 0., total, p.depth)) }, 0., p.base_thickness);
    indexed_triangle_set posts = extrude_xz(union_ex(stations), p.depth);
    for (stl_vertex &v : posts.vertices)
        v.z() += float(p.base_thickness);
    its_merge(coupon.mesh, posts);
    return coupon;
}

Coupon make_resolution_coupon(const ResolutionCouponParams &p)
{
    if (p.count < 1)
        return failed("At least one feature is needed.");
    const double smallest = p.first_diameter - (p.count - 1) * p.step;
    if (smallest < 0.05 || p.first_diameter < smallest)
        return failed("The smallest feature must be at least 0.05 mm and the features must get smaller.");
    if (p.pin_height <= 0. || p.plate_thickness <= 0. || p.margin <= 0.)
        return failed("Lengths must be above zero.");

    Coupon coupon;
    coupon.parameter = "feature diameter";
    coupon.unit      = "mm";
    coupon.values    = stepped(p.first_diameter, -p.step, p.count);

    const double pitch = p.first_diameter + p.margin;
    const double width = p.count * pitch + p.margin;
    const double depth = 2. * pitch + p.margin;
    Polygons pins, holes;
    for (int i = 0; i < p.count; ++ i) {
        const double cx = p.margin + p.first_diameter * 0.5 + i * pitch;
        pins.emplace_back(circle(cx, p.margin + p.first_diameter * 0.5, coupon.values[i]));
        holes.emplace_back(circle(cx, p.margin + p.first_diameter * 0.5 + pitch, coupon.values[i]));
    }
    coupon.mesh = make_wall_mesh(diff_ex(Polygons{ rectangle(0., 0., width, depth) }, holes), 0., p.plate_thickness);
    its_merge(coupon.mesh, make_wall_mesh(union_ex(pins), p.plate_thickness, p.plate_thickness + p.pin_height));
    return coupon;
}

} // namespace Slic3r
