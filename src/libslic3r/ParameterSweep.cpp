#include "ParameterSweep.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <locale>
#include <sstream>

#include "ClipperUtils.hpp"
#include "Model.hpp"
#include "Polygon.hpp"

namespace Slic3r {

namespace {

constexpr int kMaxSweepCopies = 200;

// Segments of a seven segment digit: a top, b upper right, c lower right, d bottom, e lower left, f upper left,
// g middle.
enum : unsigned { SegA = 1, SegB = 2, SegC = 4, SegD = 8, SegE = 16, SegF = 32, SegG = 64 };
constexpr unsigned kDigitSegments[10] = {
    SegA | SegB | SegC | SegD | SegE | SegF,
    SegB | SegC,
    SegA | SegB | SegG | SegE | SegD,
    SegA | SegB | SegG | SegC | SegD,
    SegF | SegG | SegB | SegC,
    SegA | SegF | SegG | SegC | SegD,
    SegA | SegF | SegG | SegE | SegC | SegD,
    SegA | SegB | SegC,
    SegA | SegB | SegC | SegD | SegE | SegF | SegG,
    SegA | SegB | SegC | SegD | SegF | SegG,
};

Polygon rectangle(double x0, double y0, double x1, double y1)
{
    Polygon rect;
    rect.points = { scaled<coord_t>(Vec2d(x0, y0)), scaled<coord_t>(Vec2d(x1, y0)),
                    scaled<coord_t>(Vec2d(x1, y1)), scaled<coord_t>(Vec2d(x0, y1)) };
    return rect;
}

// A digit of height h: strokes of h / 6, so that two vertical strokes and three horizontal ones make the height.
double digit_width(double h) { return 0.6 * h; }
double digit_gap(double h) { return 0.3 * h; }

double label_width(size_t digits, double h)
{
    return digits == 0 ? 0. : digits * digit_width(h) + (digits - 1) * digit_gap(h);
}

Polygons digit_polygons(int digit, double x0, double y0, double h)
{
    const double w  = h / 6.;
    const double hv = h / 4.;
    const double W  = digit_width(h);
    const unsigned s = kDigitSegments[std::clamp(digit, 0, 9)];
    const double y_lo = y0 + w, y_mid = y0 + w + hv, y_up = y0 + 2. * w + hv, y_top = y0 + 2. * w + 2. * hv;

    Polygons out;
    auto add = [&out](double xa, double ya, double xb, double yb) { out.emplace_back(rectangle(xa, ya, xb, yb)); };
    if (s & SegD) add(x0, y0, x0 + W, y0 + w);
    if (s & SegE) add(x0, y_lo, x0 + w, y_mid);
    if (s & SegC) add(x0 + W - w, y_lo, x0 + W, y_mid);
    if (s & SegG) add(x0, y_mid, x0 + W, y_up);
    if (s & SegF) add(x0, y_up, x0 + w, y_top);
    if (s & SegB) add(x0 + W - w, y_up, x0 + W, y_top);
    if (s & SegA) add(x0, y_top, x0 + W, y0 + h);
    // Without the middle stroke, strokes above each other on one side are joined by a square.
    if (! (s & SegG)) {
        if ((s & SegE) && (s & SegF)) add(x0, y_mid, x0 + w, y_up);
        if ((s & SegC) && (s & SegB)) add(x0 + W - w, y_mid, x0 + W, y_up);
    }
    return out;
}

Polygons label_polygons(const std::string &label, double x0, double y0, double h)
{
    Polygons out;
    double x = x0;
    for (char c : label)
        if (c >= '0' && c <= '9') {
            append(out, digit_polygons(c - '0', x, y0, h));
            x += digit_width(h) + digit_gap(h);
        }
    return out;
}

struct Piece
{
    indexed_triangle_set mesh;
    DropletSets          droplets;
    std::string          error;
};

Piece make_piece(const SweepParams &p, double value)
{
    Piece piece;
    switch (p.subject) {
    case SweepSubject::WallThickness: {
        WallThicknessCouponParams wall = p.wall;
        wall.first_thickness = value;
        wall.step            = 0.;
        wall.count           = 1;
        Coupon coupon = make_wall_thickness_coupon(wall);
        piece.error = coupon.error;
        piece.mesh  = std::move(coupon.mesh);
        break;
    }
    case SweepSubject::ChannelDiameter:
    case SweepSubject::ChannelPitch: {
        ChannelCouponParams channel = p.channel;
        channel.profile = ChannelProfile::Round;
        channel.step    = 0.;
        if (p.subject == SweepSubject::ChannelDiameter) {
            channel.first_width = value;
            channel.count       = 1;
            channel.pitch       = 0.;
        } else {
            channel.count = 2;
            channel.pitch = value;
        }
        Coupon coupon = make_channel_coupon(channel);
        piece.error = coupon.error;
        piece.mesh  = std::move(coupon.mesh);
        break;
    }
    case SweepSubject::LayerHeight: {
        if (value < 0.02 || value > 2.) {
            piece.error = "The layer height must be between 0.02 and 2 mm.";
            break;
        }
        WallThicknessCouponParams wall = p.wall;
        wall.first_thickness = p.layer_height_wall;
        wall.step            = 0.;
        wall.count           = 1;
        Coupon coupon = make_wall_thickness_coupon(wall);
        piece.error = coupon.error;
        piece.mesh  = std::move(coupon.mesh);
        break;
    }
    case SweepSubject::DropletVolume: {
        if (value <= 0.) {
            piece.error = "The droplet volume must be above zero.";
            break;
        }
        WellArrayParams well = p.well;
        well.cols      = 1;
        well.rows      = 1;
        well.volume_nl = value;
        WellArray array = make_well_array(well);
        piece.mesh     = std::move(array.mesh);
        piece.droplets = std::move(array.droplets);
        if (piece.mesh.indices.empty())
            piece.error = "The well has no wall.";
        break;
    }
    }
    return piece;
}

void translate(indexed_triangle_set &its, const Vec3f &by)
{
    for (stl_vertex &v : its.vertices)
        v += by;
}

void translate(DropletSets &sets, const Vec3d &by)
{
    for (DropletSet &set : sets)
        for (DropletPoint &point : set.points)
            point.pos += by;
}

std::string format_number(double v, int decimals = 3)
{
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << std::fixed << std::setprecision(decimals) << v;
    return out.str();
}

} // namespace

std::string sweep_subject_name(SweepSubject subject)
{
    switch (subject) {
    case SweepSubject::WallThickness:   return "wall";
    case SweepSubject::ChannelDiameter: return "channel";
    case SweepSubject::ChannelPitch:    return "channel pair";
    case SweepSubject::DropletVolume:   return "droplet well";
    case SweepSubject::LayerHeight:     return "wall";
    }
    return {};
}

std::string sweep_parameter_name(SweepSubject subject)
{
    switch (subject) {
    case SweepSubject::WallThickness:   return "wall thickness";
    case SweepSubject::ChannelDiameter: return "channel diameter";
    case SweepSubject::ChannelPitch:    return "channel pitch";
    case SweepSubject::DropletVolume:   return "droplet volume";
    case SweepSubject::LayerHeight:     return "layer height";
    }
    return {};
}

std::string sweep_parameter_unit(SweepSubject subject)
{
    return subject == SweepSubject::DropletVolume ? "nl" : "mm";
}

double sweep_label_area(const std::string &label, double label_height)
{
    const ExPolygons digits = union_ex(label_polygons(label, 0., 0., label_height));
    double area = 0.;
    for (const ExPolygon &ex : digits)
        area += unscaled(unscaled(ex.area()));
    return area;
}

SweepResult make_parameter_sweep(const SweepParams &p)
{
    SweepResult result;
    result.subject_name = sweep_subject_name(p.subject);
    result.parameter    = sweep_parameter_name(p.subject);
    result.unit         = sweep_parameter_unit(p.subject);

    if (p.count < 1 || p.count > kMaxSweepCopies) {
        result.error = "The number of copies must be between 1 and " + std::to_string(kMaxSweepCopies) + ".";
        return result;
    }
    if (p.spacing < 0. || p.plate_size.minCoeff() <= 0.) {
        result.error = "The plate and the spacing must be above zero.";
        return result;
    }
    if (p.engrave_index && (p.label_height <= 0. || p.label_thickness <= 0. || p.label_depth <= 0. || p.label_depth >= p.label_thickness)) {
        result.error = "The label must have a height and a thickness, and an engraving shallower than the thickness.";
        return result;
    }

    const double margin = p.label_height / 3.;
    for (int i = 0; i < p.count; ++ i) {
        SweepItem item;
        item.index = i + 1;
        item.label = std::to_string(item.index);
        item.value = p.start + i * p.step;
        if (p.subject == SweepSubject::LayerHeight)
            item.layer_height_mm = item.value;

        Piece piece = make_piece(p, item.value);
        if (! piece.error.empty() || piece.mesh.indices.empty()) {
            result.error = "Copy " + item.label + " (" + result.parameter + " " + format_number(item.value) + " " + result.unit + "): "
                + (piece.error.empty() ? std::string("nothing was made.") : piece.error);
            result.items.clear();
            return result;
        }
        const BoundingBoxf3 piece_box = TriangleMesh(piece.mesh).bounding_box();
        translate(piece.mesh, Vec3f(float(-piece_box.min.x()), float(-piece_box.min.y()), 0.f));
        translate(piece.droplets, Vec3d(-piece_box.min.x(), -piece_box.min.y(), 0.));

        if (p.engrave_index) {
            const double text_w  = label_width(item.label.size(), p.label_height);
            const double plate_w = std::max(piece_box.size().x(), text_w + 2. * margin);
            const double plate_d = p.label_height + 2. * margin;
            const Polygons digits = label_polygons(item.label, (plate_w - text_w) * 0.5, margin, p.label_height);
            const Polygons plate  { rectangle(0., 0., plate_w, plate_d) };

            item.mesh = make_wall_mesh({ ExPolygon(plate.front()) }, 0., p.label_thickness - p.label_depth);
            its_merge(item.mesh, make_wall_mesh(diff_ex(plate, digits), p.label_thickness - p.label_depth, p.label_thickness));

            const double dx = (plate_w - piece_box.size().x()) * 0.5;
            translate(piece.mesh, Vec3f(float(dx), float(plate_d), 0.f));
            translate(piece.droplets, Vec3d(dx, plate_d, 0.));
            its_merge(item.mesh, piece.mesh);
        } else
            item.mesh = std::move(piece.mesh);
        item.droplets = std::move(piece.droplets);

        const BoundingBoxf3 box = TriangleMesh(item.mesh).bounding_box();
        item.size       = box.size();
        item.volume_mm3 = double(its_volume(item.mesh));
        result.items.emplace_back(std::move(item));
    }

    // Rows of equal cells, as many columns as fit the plate.
    double cell_w = 0., cell_d = 0.;
    for (const SweepItem &item : result.items) {
        cell_w = std::max(cell_w, item.size.x());
        cell_d = std::max(cell_d, item.size.y());
    }
    const int cols = std::clamp(int(std::floor((p.plate_size.x() + p.spacing) / (cell_w + p.spacing))), 1, p.count);
    const int rows = (p.count + cols - 1) / cols;
    result.arrangement_size = Vec2d(cols * cell_w + (cols - 1) * p.spacing, rows * cell_d + (rows - 1) * p.spacing);
    result.fits = result.arrangement_size.x() <= p.plate_size.x() + 1e-9 && result.arrangement_size.y() <= p.plate_size.y() + 1e-9;
    if (! result.fits)
        result.error = "The " + std::to_string(p.count) + " copies need " + format_number(result.arrangement_size.x(), 1) + " x "
            + format_number(result.arrangement_size.y(), 1) + " mm and do not fit the " + format_number(p.plate_size.x(), 1) + " x "
            + format_number(p.plate_size.y(), 1) + " mm plate. Use fewer copies or a smaller spacing.";
    for (size_t i = 0; i < result.items.size(); ++ i) {
        SweepItem &item = result.items[i];
        const int col = int(i) % cols, row = int(i) / cols;
        item.offset = Vec2d(col * (cell_w + p.spacing) + cell_w * 0.5 - result.arrangement_size.x() * 0.5,
                            row * (cell_d + p.spacing) + cell_d * 0.5 - result.arrangement_size.y() * 0.5);
        item.plate_position = p.plate_size * 0.5 + item.offset;
    }

    std::ostringstream csv;
    csv << "index,label,subject,parameter,value,unit,x_mm,y_mm,width_mm,depth_mm,height_mm,volume_mm3,droplets,layer_height_mm\n";
    for (const SweepItem &item : result.items) {
        size_t droplets = 0;
        for (const DropletSet &set : item.droplets)
            droplets += set.points.size();
        csv << item.index << ',' << item.label << ',' << result.subject_name << ',' << result.parameter << ',' << format_number(item.value) << ','
            << result.unit << ',' << format_number(item.plate_position.x()) << ',' << format_number(item.plate_position.y()) << ','
            << format_number(item.size.x()) << ',' << format_number(item.size.y()) << ',' << format_number(item.size.z()) << ','
            << format_number(item.volume_mm3) << ',' << droplets << ',' << (item.layer_height_mm > 0. ? format_number(item.layer_height_mm) : std::string()) << '\n';
    }
    result.csv = csv.str();
    return result;
}

void add_sweep_to_model(Model &model, const SweepResult &sweep, const Vec2d &center, int droplet_extruder)
{
    for (const SweepItem &item : sweep.items) {
        ModelObject *object = model.add_object();
        object->name = "Sweep " + item.label + " of " + std::to_string(sweep.items.size()) + ": " + sweep.parameter + " "
            + format_number(item.value, 2) + " " + sweep.unit;
        ModelVolume *volume = object->add_volume(TriangleMesh(item.mesh));
        volume->name = sweep.subject_name;
        for (DropletSet set : item.droplets) {
            set.extruder = droplet_extruder;
            set.dip      = true;
            object->droplet_sets.emplace_back(std::move(set));
        }
        if (item.layer_height_mm > 0.)
            object->config.set_key_value("layer_height", new ConfigOptionFloat(item.layer_height_mm));
        object->center_around_origin();
        ModelInstance *instance = object->add_instance();
        instance->set_offset(Vec3d(center.x() + item.offset.x(), center.y() + item.offset.y(), item.size.z() * 0.5));
    }
}

} // namespace Slic3r
