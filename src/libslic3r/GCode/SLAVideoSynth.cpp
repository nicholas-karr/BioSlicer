#include "SLAVideoSynth.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <thread>
#include <vector>

#include <boost/filesystem.hpp>
#include <boost/process.hpp>

#include "libslic3r/Exception.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/format.hpp"
#include "libslic3r/libslic3r.h"

namespace process = boost::process;

namespace Slic3r {

BoundingBoxf sla_proj_bbox(
    const BoundingBoxf &bed_bbox,
    double              display_width_mm,
    double              display_height_mm)
{
    const Vec2d center = (bed_bbox.min + bed_bbox.max) * 0.5;
    BoundingBoxf proj;
    if (display_width_mm > 0.0 && display_height_mm > 0.0) {
        proj.min = Vec2d(center.x() - display_width_mm  * 0.5,
                         center.y() - display_height_mm * 0.5);
        proj.max = Vec2d(center.x() + display_width_mm  * 0.5,
                         center.y() + display_height_mm * 0.5);
    } else {
        const double dim = std::max(bed_bbox.max.x() - bed_bbox.min.x(),
                                    bed_bbox.max.y() - bed_bbox.min.y());
        proj.min = Vec2d(center.x() - dim * 0.5, center.y() - dim * 0.5);
        proj.max = Vec2d(center.x() + dim * 0.5, center.y() + dim * 0.5);
    }
    proj.defined = true;
    return proj;
}

void render_sla_synth_pgm_frame(std::ostream &os, int width, int height)
{
    os << "P5\n" << width << " " << height << "\n255\n";
    std::vector<unsigned char> pixels(size_t(width) * size_t(height), 255u);
    if (!pixels.empty())
        os.write(reinterpret_cast<const char *>(pixels.data()), std::streamsize(pixels.size()));
}

static double mm_to_px_x(double x_mm, double proj_min_x, double proj_max_x, int width)
{
    const double span = std::max(1e-9, proj_max_x - proj_min_x);
    return (x_mm - proj_min_x) * double(width - 1) / span;
}

static double mm_to_px_y(double y_mm, double proj_min_y, double proj_max_y, int height)
{
    const double span = std::max(1e-9, proj_max_y - proj_min_y);
    return (proj_max_y - y_mm) * double(height - 1) / span;
}

void render_sla_slice_pgm_frame(
    std::ostream       &os,
    int                 width,
    int                 height,
    const ExPolygons   &expolys,
    const BoundingBoxf &proj_bbox)
{
    os << "P5\n" << width << " " << height << "\n255\n";

    std::vector<unsigned char> pixels(size_t(width) * size_t(height), 0);

    struct RingPx { std::vector<Vec2d> points; };
    struct ExPolygonPx { std::vector<RingPx> rings; };

    std::vector<ExPolygonPx> expolys_px;
    expolys_px.reserve(expolys.size());

    const double proj_min_x = proj_bbox.min.x();
    const double proj_max_x = proj_bbox.max.x();
    const double proj_min_y = proj_bbox.min.y();
    const double proj_max_y = proj_bbox.max.y();

    auto make_polygon_ring = [&](const Polygon &poly) -> RingPx {
        RingPx ring;
        if (poly.points.size() < 3)
            return ring;
        ring.points.reserve(poly.points.size());
        for (const Point &p : poly.points) {
            const double x_mm = unscale<double>(p.x());
            const double y_mm = unscale<double>(p.y());
            ring.points.emplace_back(
                mm_to_px_x(x_mm, proj_min_x, proj_max_x, width),
                mm_to_px_y(y_mm, proj_min_y, proj_max_y, height)
            );
        }
        return ring;
    };

    for (const ExPolygon &expoly : expolys) {
        ExPolygonPx ep;
        {
            RingPx contour = make_polygon_ring(expoly.contour);
            if (!contour.points.empty())
                ep.rings.emplace_back(std::move(contour));
        }
        for (const Polygon &hole : expoly.holes) {
            RingPx hole_ring = make_polygon_ring(hole);
            if (!hole_ring.points.empty())
                ep.rings.emplace_back(std::move(hole_ring));
        }
        if (!ep.rings.empty())
            expolys_px.emplace_back(std::move(ep));
    }

    std::vector<double> intersections;
    for (int y = 0; y < height; ++y) {
        const double yy = double(y) + 0.5;

        for (const ExPolygonPx &ep : expolys_px) {
            intersections.clear();

            for (const RingPx &ring : ep.rings) {
                const size_t n = ring.points.size();
                for (size_t i = 0, j = n - 1; i < n; j = i++) {
                    const Vec2d &a = ring.points[j];
                    const Vec2d &b = ring.points[i];
                    const double y1 = a.y();
                    const double y2 = b.y();

                    if ((y1 <= yy && yy < y2) || (y2 <= yy && yy < y1)) {
                        const double t = (yy - y1) / (y2 - y1);
                        const double xx = a.x() + t * (b.x() - a.x());
                        intersections.emplace_back(xx);
                    }
                }
            }

            if (intersections.empty())
                continue;

            std::sort(intersections.begin(), intersections.end());
            for (size_t i = 0; i + 1 < intersections.size(); i += 2) {
                int x0 = int(std::ceil(intersections[i]));
                int x1 = int(std::floor(intersections[i + 1]));
                if (x1 < 0 || x0 >= width)
                    continue;
                x0 = std::max(0, x0);
                x1 = std::min(width - 1, x1);
                for (int x = x0; x <= x1; ++x)
                    pixels[size_t(y) * size_t(width) + size_t(x)] = 255;
            }
        }
    }

    if (!pixels.empty())
        os.write(reinterpret_cast<const char *>(pixels.data()), std::streamsize(pixels.size()));
}

void encode_sla_video_with_ffmpeg_or_throw(
    const boost::filesystem::path                     &output_path,
    int                                                fps,
    bool                                               lossless,
    size_t                                             frame_count,
    const std::function<void(std::ostream &, size_t)> &render_frame)
{
    const boost::filesystem::path ffmpeg_path = process::search_path("ffmpeg");
    if (ffmpeg_path.empty())
        throw Slic3r::ExportError("Failed to locate ffmpeg in PATH for native SLA video synthesis.");

    // Use ffv1 (lossless) or libvpx-vp9 (quality) — both ship in GNOME Platform.
    // libx265 is intentionally avoided: it is not bundled in the GNOME Platform
    // flatpak runtime and causes ffmpeg to exit immediately with an unknown-encoder
    // error, which on aarch64 leaves child.wait() stuck on a broken pipe.
    std::vector<std::string> args{
        "-y",
        "-hide_banner",
        "-loglevel", "error",
        "-framerate", std::to_string(std::max(1, fps)),
        "-f", "image2pipe",
        "-vcodec", "pgm",
        "-i", "pipe:0"
    };

    if (lossless) {
        args.insert(args.end(), {"-c:v", "ffv1", "-pix_fmt", "gray"});
    } else {
        args.insert(args.end(), {
            "-c:v", "libvpx-vp9",
            "-crf", "22",
            "-b:v", "0",
            "-pix_fmt", "yuv420p"
        });
    }

    args.insert(args.end(), {"-an", output_path.string()});

    process::opstream ffmpeg_stdin;
    process::ipstream stderr_stream;
    process::child child(ffmpeg_path, process::args(args),
                         process::std_in  < ffmpeg_stdin,
                         process::std_out > process::null,
                         process::std_err > stderr_stream);

    // Drain stderr on a background thread to prevent the pipe buffer filling
    // up and deadlocking while we write frames to stdin.
    std::string stderr_text;
    std::thread stderr_thread([&] {
        std::string line;
        while (std::getline(stderr_stream, line)) {
            stderr_text += line;
            stderr_text += "\n";
        }
    });

    // RAII guard: always join stderr_thread even if an exception is thrown
    // below. A joinable std::thread destroyed without join() calls terminate().
    struct ThreadGuard {
        std::thread &t;
        ~ThreadGuard() { if (t.joinable()) t.join(); }
    } _guard{stderr_thread};

    for (size_t i = 0; i < frame_count && ffmpeg_stdin.good(); ++i)
        render_frame(ffmpeg_stdin, i);

    ffmpeg_stdin.flush();
    ffmpeg_stdin.pipe().close();
    stderr_thread.join();
    child.wait();

    if (child.exit_code() != 0)
        throw Slic3r::ExportError(format("Native SLA synthesis failed during ffmpeg encode: %1%", stderr_text));

    if (!boost::filesystem::exists(output_path))
        throw Slic3r::ExportError(format("Native SLA synthesis finished without output file: %1%", output_path.string()));
}

} // namespace Slic3r
