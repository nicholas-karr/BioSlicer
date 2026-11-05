#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "libslic3r/Model.hpp"
#include "libslic3r/Format/3mf.hpp"
#include "libslic3r/Format/STL.hpp"
#include "libslic3r/Vessel/VesselTree.hpp"
#include "libslic3r/miniz_extension.hpp"

#include <boost/filesystem/operations.hpp>

#include <string>

using namespace Slic3r;

namespace {

struct ArchiveWriteResult {
    bool ok{ false };
    std::string error;
};

std::string make_minimal_namespaced_volumetric_model_xml()
{
        return R"(<?xml version="1.0" encoding="UTF-8"?>
<model unit="millimeter" xml:lang="en-US"
             xmlns="http://schemas.microsoft.com/3dmanufacturing/core/2015/02"
             xmlns:vol="http://schemas.example.com/volumetric/2025">
    <resources>
        <object id="1" type="model" name="volumetric_object">
            <mesh vol:volumeid="7">
                <vertices>
                    <vertex x="0" y="0" z="0"/>
                    <vertex x="10" y="0" z="0"/>
                    <vertex x="0" y="10" z="0"/>
                </vertices>
                <triangles>
                    <triangle v1="0" v2="1" v3="2"/>
                </triangles>
            </mesh>
        </object>
        <vol:volumedata vol:id="7">
            <vol:composite vol:basematerialid="1">
                <vol:materialmapping vol:functionid="11" vol:channel="bio_a"/>
            </vol:composite>
            <vol:property vol:name="density" vol:functionid="11" vol:channel="bio_a" vol:required="1"/>
        </vol:volumedata>
        <vol:functionfromimage3d vol:id="11" vol:displayname="f_density" vol:image3did="21"/>
        <vol:image3d vol:id="21" vol:name="density_map"/>
    </resources>
    <build>
        <item objectid="1" transform="1 0 0 0 1 0 0 0 1 0 0 0" printable="1"/>
    </build>
</model>
)";
}

std::string make_two_volume_namespaced_volumetric_model_xml()
{
        return R"(<?xml version="1.0" encoding="UTF-8"?>
<model unit="millimeter" xml:lang="en-US"
             xmlns="http://schemas.microsoft.com/3dmanufacturing/core/2015/02"
             xmlns:vol="http://schemas.example.com/volumetric/2025">
    <resources>
        <object id="1" type="model" name="volumetric_two_volume_object">
            <mesh vol:volumeid="9">
                <vertices>
                    <vertex x="0" y="0" z="0"/>
                    <vertex x="10" y="0" z="0"/>
                    <vertex x="10" y="10" z="0"/>
                    <vertex x="0" y="10" z="0"/>
                </vertices>
                <triangles>
                    <triangle v1="0" v2="1" v3="2"/>
                    <triangle v1="0" v2="2" v3="3"/>
                </triangles>
            </mesh>
        </object>
        <vol:volumedata vol:id="9">
            <vol:composite vol:basematerialid="1">
                <vol:materialmapping vol:functionid="31" vol:channel="bio_same"/>
                <vol:materialmapping vol:functionid="32" vol:channel="bio_same"/>
            </vol:composite>
        </vol:volumedata>
        <vol:functionfromimage3d vol:id="31" vol:displayname="f_a" vol:image3did="41"/>
        <vol:functionfromimage3d vol:id="32" vol:displayname="f_b" vol:image3did="41"/>
        <vol:image3d vol:id="41" vol:name="shared_map"/>
    </resources>
    <build>
        <item objectid="1" transform="1 0 0 0 1 0 0 0 1 0 0 0" printable="1"/>
    </build>
</model>
)";
}

std::string make_two_volume_namespaced_volumetric_model_xml_distinct_channels()
{
        return R"(<?xml version="1.0" encoding="UTF-8"?>
<model unit="millimeter" xml:lang="en-US"
             xmlns="http://schemas.microsoft.com/3dmanufacturing/core/2015/02"
             xmlns:vol="http://schemas.example.com/volumetric/2025">
    <resources>
        <object id="1" type="model" name="volumetric_two_volume_distinct_channels_object">
            <mesh vol:volumeid="10">
                <vertices>
                    <vertex x="0" y="0" z="0"/>
                    <vertex x="10" y="0" z="0"/>
                    <vertex x="10" y="10" z="0"/>
                    <vertex x="0" y="10" z="0"/>
                </vertices>
                <triangles>
                    <triangle v1="0" v2="1" v3="2"/>
                    <triangle v1="0" v2="2" v3="3"/>
                </triangles>
            </mesh>
        </object>
        <vol:volumedata vol:id="10">
            <vol:composite vol:basematerialid="1">
                <vol:materialmapping vol:functionid="51" vol:channel="bio_a"/>
                <vol:materialmapping vol:functionid="52" vol:channel="bio_b"/>
            </vol:composite>
        </vol:volumedata>
        <vol:functionfromimage3d vol:id="51" vol:displayname="f_a" vol:image3did="61"/>
        <vol:functionfromimage3d vol:id="52" vol:displayname="f_b" vol:image3did="61"/>
        <vol:image3d vol:id="61" vol:name="shared_map"/>
    </resources>
    <build>
        <item objectid="1" transform="1 0 0 0 1 0 0 0 1 0 0 0" printable="1"/>
    </build>
</model>
)";
}

std::string make_two_volume_model_config_xml()
{
        return R"(<?xml version="1.0" encoding="UTF-8"?>
<config>
    <object id="1" instances_count="1">
        <volume firstid="0" lastid="0">
            <mesh edges_fixed="0" degenerate_facets="0" facets_removed="0" facets_reversed="0" backwards_edges="0"/>
        </volume>
        <volume firstid="1" lastid="1">
            <mesh edges_fixed="0" degenerate_facets="0" facets_removed="0" facets_reversed="0" backwards_edges="0"/>
        </volume>
    </object>
</config>
)";
}

std::string make_minimal_relationships_xml()
{
        return R"(<?xml version="1.0" encoding="UTF-8"?>
<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">
    <Relationship Target="/3D/3dmodel.model" Id="rel-1" Type="http://schemas.microsoft.com/3dmanufacturing/2013/01/3dmodel"/>
</Relationships>
)";
}

std::string make_minimal_content_types_xml()
{
        return R"(<?xml version="1.0" encoding="UTF-8"?>
<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">
    <Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/>
    <Default Extension="model" ContentType="application/vnd.ms-package.3dmanufacturing-3dmodel+xml"/>
</Types>
)";
}

ArchiveWriteResult write_test_3mf_archive(const std::string &path)
{
    mz_zip_archive archive{};
    if (!mz_zip_writer_init_file(&archive, path.c_str(), 0))
        return { false, std::string("init_file failed: ") + mz_zip_get_error_string(archive.m_last_error) };

        const std::string content_types = make_minimal_content_types_xml();
        const std::string rels = make_minimal_relationships_xml();
        const std::string model = make_minimal_namespaced_volumetric_model_xml();

        if (!mz_zip_writer_add_mem(&archive, "[Content_Types].xml", content_types.data(), content_types.size(), MZ_DEFAULT_COMPRESSION)) {
            const std::string error = std::string("add [Content_Types].xml failed: ") + mz_zip_get_error_string(archive.m_last_error);
            mz_zip_writer_end(&archive);
            return { false, error };
        }

        if (!mz_zip_writer_add_mem(&archive, "_rels/.rels", rels.data(), rels.size(), MZ_DEFAULT_COMPRESSION)) {
            const std::string error = std::string("add _rels/.rels failed: ") + mz_zip_get_error_string(archive.m_last_error);
            mz_zip_writer_end(&archive);
            return { false, error };
        }

        if (!mz_zip_writer_add_mem(&archive, "3D/3dmodel.model", model.data(), model.size(), MZ_DEFAULT_COMPRESSION)) {
            const std::string error = std::string("add 3D/3dmodel.model failed: ") + mz_zip_get_error_string(archive.m_last_error);
            mz_zip_writer_end(&archive);
            return { false, error };
        }

        if (!mz_zip_writer_finalize_archive(&archive)) {
            const std::string error = std::string("finalize failed: ") + mz_zip_get_error_string(archive.m_last_error);
            mz_zip_writer_end(&archive);
            return { false, error };
        }

        if (!mz_zip_writer_end(&archive))
            return { false, std::string("writer_end failed: ") + mz_zip_get_error_string(archive.m_last_error) };

        return { true, "" };
}

    ArchiveWriteResult write_test_3mf_archive_two_volume_duplicate_channels(const std::string &path)
    {
        mz_zip_archive archive{};
        if (!mz_zip_writer_init_file(&archive, path.c_str(), 0))
            return { false, std::string("init_file failed: ") + mz_zip_get_error_string(archive.m_last_error) };

        const std::string content_types = make_minimal_content_types_xml();
        const std::string rels = make_minimal_relationships_xml();
        const std::string model = make_two_volume_namespaced_volumetric_model_xml();
        const std::string model_config = make_two_volume_model_config_xml();

        if (!mz_zip_writer_add_mem(&archive, "[Content_Types].xml", content_types.data(), content_types.size(), MZ_DEFAULT_COMPRESSION)) {
            const std::string error = std::string("add [Content_Types].xml failed: ") + mz_zip_get_error_string(archive.m_last_error);
            mz_zip_writer_end(&archive);
            return { false, error };
        }

        if (!mz_zip_writer_add_mem(&archive, "_rels/.rels", rels.data(), rels.size(), MZ_DEFAULT_COMPRESSION)) {
            const std::string error = std::string("add _rels/.rels failed: ") + mz_zip_get_error_string(archive.m_last_error);
            mz_zip_writer_end(&archive);
            return { false, error };
        }

        if (!mz_zip_writer_add_mem(&archive, "3D/3dmodel.model", model.data(), model.size(), MZ_DEFAULT_COMPRESSION)) {
            const std::string error = std::string("add 3D/3dmodel.model failed: ") + mz_zip_get_error_string(archive.m_last_error);
            mz_zip_writer_end(&archive);
            return { false, error };
        }

        if (!mz_zip_writer_add_mem(&archive, "Metadata/Slic3r_PE_model.config", model_config.data(), model_config.size(), MZ_DEFAULT_COMPRESSION)) {
            const std::string error = std::string("add Metadata/Slic3r_PE_model.config failed: ") + mz_zip_get_error_string(archive.m_last_error);
            mz_zip_writer_end(&archive);
            return { false, error };
        }

        if (!mz_zip_writer_finalize_archive(&archive)) {
            const std::string error = std::string("finalize failed: ") + mz_zip_get_error_string(archive.m_last_error);
            mz_zip_writer_end(&archive);
            return { false, error };
        }

        if (!mz_zip_writer_end(&archive))
            return { false, std::string("writer_end failed: ") + mz_zip_get_error_string(archive.m_last_error) };

        return { true, "" };
    }

    ArchiveWriteResult write_test_3mf_archive_two_volume_distinct_channels(const std::string &path)
    {
        mz_zip_archive archive{};
        if (!mz_zip_writer_init_file(&archive, path.c_str(), 0))
            return { false, std::string("init_file failed: ") + mz_zip_get_error_string(archive.m_last_error) };

        const std::string content_types = make_minimal_content_types_xml();
        const std::string rels = make_minimal_relationships_xml();
        const std::string model = make_two_volume_namespaced_volumetric_model_xml_distinct_channels();
        const std::string model_config = make_two_volume_model_config_xml();

        if (!mz_zip_writer_add_mem(&archive, "[Content_Types].xml", content_types.data(), content_types.size(), MZ_DEFAULT_COMPRESSION)) {
            const std::string error = std::string("add [Content_Types].xml failed: ") + mz_zip_get_error_string(archive.m_last_error);
            mz_zip_writer_end(&archive);
            return { false, error };
        }

        if (!mz_zip_writer_add_mem(&archive, "_rels/.rels", rels.data(), rels.size(), MZ_DEFAULT_COMPRESSION)) {
            const std::string error = std::string("add _rels/.rels failed: ") + mz_zip_get_error_string(archive.m_last_error);
            mz_zip_writer_end(&archive);
            return { false, error };
        }

        if (!mz_zip_writer_add_mem(&archive, "3D/3dmodel.model", model.data(), model.size(), MZ_DEFAULT_COMPRESSION)) {
            const std::string error = std::string("add 3D/3dmodel.model failed: ") + mz_zip_get_error_string(archive.m_last_error);
            mz_zip_writer_end(&archive);
            return { false, error };
        }

        if (!mz_zip_writer_add_mem(&archive, "Metadata/Slic3r_PE_model.config", model_config.data(), model_config.size(), MZ_DEFAULT_COMPRESSION)) {
            const std::string error = std::string("add Metadata/Slic3r_PE_model.config failed: ") + mz_zip_get_error_string(archive.m_last_error);
            mz_zip_writer_end(&archive);
            return { false, error };
        }

        if (!mz_zip_writer_finalize_archive(&archive)) {
            const std::string error = std::string("finalize failed: ") + mz_zip_get_error_string(archive.m_last_error);
            mz_zip_writer_end(&archive);
            return { false, error };
        }

        if (!mz_zip_writer_end(&archive))
            return { false, std::string("writer_end failed: ") + mz_zip_get_error_string(archive.m_last_error) };

        return { true, "" };
    }

std::string make_malformed_object_id_model_xml()
{
        return R"(<?xml version="1.0" encoding="UTF-8"?>
<model unit="millimeter" xml:lang="en-US" xmlns="http://schemas.microsoft.com/3dmanufacturing/core/2015/02">
    <resources>
        <object id="1x" type="model">
            <mesh>
                <vertices>
                    <vertex x="0" y="0" z="0"/>
                    <vertex x="10" y="0" z="0"/>
                    <vertex x="0" y="10" z="0"/>
                </vertices>
                <triangles>
                    <triangle v1="0" v2="1" v3="2"/>
                </triangles>
            </mesh>
        </object>
    </resources>
    <build>
        <item objectid="1" transform="1 0 0 0 1 0 0 0 1 0 0 0" printable="1"/>
    </build>
</model>
)";
}

std::string make_missing_object_id_model_xml()
{
        return R"(<?xml version="1.0" encoding="UTF-8"?>
<model unit="millimeter" xml:lang="en-US" xmlns="http://schemas.microsoft.com/3dmanufacturing/core/2015/02">
    <resources>
        <object type="model">
            <mesh>
                <vertices>
                    <vertex x="0" y="0" z="0"/>
                    <vertex x="10" y="0" z="0"/>
                    <vertex x="0" y="10" z="0"/>
                </vertices>
                <triangles>
                    <triangle v1="0" v2="1" v3="2"/>
                </triangles>
            </mesh>
        </object>
    </resources>
    <build>
        <item objectid="1" transform="1 0 0 0 1 0 0 0 1 0 0 0" printable="1"/>
    </build>
</model>
)";
}

    ArchiveWriteResult write_test_3mf_archive_with_model_xml(const std::string &path, const std::string &model)
    {
        mz_zip_archive archive{};
        if (!mz_zip_writer_init_file(&archive, path.c_str(), 0))
            return { false, std::string("init_file failed: ") + mz_zip_get_error_string(archive.m_last_error) };

        const std::string content_types = make_minimal_content_types_xml();
        const std::string rels = make_minimal_relationships_xml();

        if (!mz_zip_writer_add_mem(&archive, "[Content_Types].xml", content_types.data(), content_types.size(), MZ_DEFAULT_COMPRESSION)) {
            const std::string error = std::string("add [Content_Types].xml failed: ") + mz_zip_get_error_string(archive.m_last_error);
            mz_zip_writer_end(&archive);
            return { false, error };
        }

        if (!mz_zip_writer_add_mem(&archive, "_rels/.rels", rels.data(), rels.size(), MZ_DEFAULT_COMPRESSION)) {
            const std::string error = std::string("add _rels/.rels failed: ") + mz_zip_get_error_string(archive.m_last_error);
            mz_zip_writer_end(&archive);
            return { false, error };
        }

        if (!mz_zip_writer_add_mem(&archive, "3D/3dmodel.model", model.data(), model.size(), MZ_DEFAULT_COMPRESSION)) {
            const std::string error = std::string("add 3D/3dmodel.model failed: ") + mz_zip_get_error_string(archive.m_last_error);
            mz_zip_writer_end(&archive);
            return { false, error };
        }

        if (!mz_zip_writer_finalize_archive(&archive)) {
            const std::string error = std::string("finalize failed: ") + mz_zip_get_error_string(archive.m_last_error);
            mz_zip_writer_end(&archive);
            return { false, error };
        }

        if (!mz_zip_writer_end(&archive))
            return { false, std::string("writer_end failed: ") + mz_zip_get_error_string(archive.m_last_error) };

        return { true, "" };
    }

} // namespace

SCENARIO("Reading 3mf file", "[3mf]") {
    GIVEN("umlauts in the path of the file") {
        Model model;
        WHEN("3mf model is read") {
        	std::string path = std::string(TEST_DATA_DIR) + "/test_3mf/Geräte/Büchse.3mf";
        	DynamicPrintConfig config;
            ConfigSubstitutionContext ctxt{ ForwardCompatibilitySubstitutionRule::Disable };
            boost::optional<Semver> version;
            bool ret = load_3mf(path.c_str(), config, ctxt, &model, false, version);
            THEN("load should succeed") {
                REQUIRE(ret);
            }
        }
    }
}

SCENARIO("Export+Import geometry to/from 3mf file cycle", "[3mf]") {
    GIVEN("world vertices coordinates before save") {
        // load a model from stl file
        Model src_model;
        std::string src_file = std::string(TEST_DATA_DIR) + "/test_3mf/Prusa.stl";
        load_stl(src_file.c_str(), &src_model);
        src_model.add_default_instances();

        ModelObject* src_object = src_model.objects.front();

        // apply generic transformation to the 1st volume
        Geometry::Transformation src_volume_transform;
        src_volume_transform.set_offset({ 10.0, 20.0, 0.0 });
        src_volume_transform.set_rotation({ Geometry::deg2rad(25.0), Geometry::deg2rad(35.0), Geometry::deg2rad(45.0) });
        src_volume_transform.set_scaling_factor({ 1.1, 1.2, 1.3 });
        src_volume_transform.set_mirror({ -1.0, 1.0, -1.0 });
        src_object->volumes.front()->set_transformation(src_volume_transform);

        // apply generic transformation to the 1st instance
        Geometry::Transformation src_instance_transform;
        src_instance_transform.set_offset({ 5.0, 10.0, 0.0 });
        src_instance_transform.set_rotation({ Geometry::deg2rad(12.0), Geometry::deg2rad(13.0), Geometry::deg2rad(14.0) });
        src_instance_transform.set_scaling_factor({ 0.9, 0.8, 0.7 });
        src_instance_transform.set_mirror({ 1.0, -1.0, -1.0 });
        src_object->instances.front()->set_transformation(src_instance_transform);

        WHEN("model is saved+loaded to/from 3mf file") {
            // save the model to 3mf file
            std::string test_file = std::string(TEST_DATA_DIR) + "/test_3mf/prusa.3mf";
            store_3mf(test_file.c_str(), &src_model, nullptr, false);

            // load back the model from the 3mf file
            Model dst_model;
            DynamicPrintConfig dst_config;
            {
                ConfigSubstitutionContext ctxt{ ForwardCompatibilitySubstitutionRule::Disable };
                boost::optional<Semver> version;
                load_3mf(test_file.c_str(), dst_config, ctxt, &dst_model, false, version);
            }
            boost::filesystem::remove(test_file);

            // compare meshes
            TriangleMesh src_mesh = src_model.mesh();
            TriangleMesh dst_mesh = dst_model.mesh();

            bool res = src_mesh.its.vertices.size() == dst_mesh.its.vertices.size();
            if (res) {
                for (size_t i = 0; i < dst_mesh.its.vertices.size(); ++i) {
                    res &= dst_mesh.its.vertices[i].isApprox(src_mesh.its.vertices[i]);
                }
            }
            THEN("world vertices coordinates after load match") {
                REQUIRE(res);
            }
        }
    }
}

SCENARIO("Droplets of the model and of its objects survive a 3mf file cycle", "[3mf]") {
    GIVEN("a model with an object holding droplets and droplets of its own") {
        Model src_model;
        load_stl((std::string(TEST_DATA_DIR) + "/test_3mf/Prusa.stl").c_str(), &src_model);
        src_model.add_default_instances();
        // A second object without droplets, to see the droplets stay with the right one.
        src_model.add_object(*src_model.objects.front())->droplet_sets.clear();
        src_model.objects.front()->droplet_sets.clear();
        src_model.objects.back()->droplet_sets.clear();

        DropletSet on_object;
        on_object.name = "well A1";
        on_object.extruder = 9;
        on_object.dip = true;
        on_object.keep_order = true;
        DropletPoint p1; p1.pos = Vec3d(1.25, -2.5, 0.4); p1.volume_nl = 12.5; p1.count = 3;
        DropletPoint p2; p2.pos = Vec3d(4., 5., 6.); p2.volume_nl = 50.; p2.count = 1;
        on_object.points = { p1, p2 };
        src_model.objects.back()->droplet_sets.push_back(on_object);

        DropletSet on_bed;
        on_bed.name = "spots";
        on_bed.extruder = 9;
        DropletPoint p3; p3.pos = Vec3d(100., 90., 0.); p3.volume_nl = 20.; p3.count = 2;
        on_bed.points = { p3 };
        src_model.droplet_sets.push_back(on_bed);

        WHEN("it is saved and loaded") {
            const std::string test_file = std::string(TEST_DATA_DIR) + "/test_3mf/droplets.3mf";
            REQUIRE(store_3mf(test_file.c_str(), &src_model, nullptr, false));
            Model dst_model;
            DynamicPrintConfig dst_config;
            {
                ConfigSubstitutionContext ctxt{ ForwardCompatibilitySubstitutionRule::Disable };
                boost::optional<Semver> version;
                load_3mf(test_file.c_str(), dst_config, ctxt, &dst_model, false, version);
            }
            boost::filesystem::remove(test_file);

            THEN("every droplet is where it was") {
                REQUIRE(dst_model.objects.size() == 2);
                CHECK(dst_model.objects.front()->droplet_sets.empty());
                CHECK(dst_model.objects.back()->droplet_sets == src_model.objects.back()->droplet_sets);
                CHECK(dst_model.droplet_sets == src_model.droplet_sets);
            }
        }
    }
}

SCENARIO("2D convex hull of sinking object", "[3mf]") {
    GIVEN("model") {
        // load a model
        Model model;
        std::string src_file = std::string(TEST_DATA_DIR) + "/test_3mf/Prusa.stl";
        load_stl(src_file.c_str(), &model);
        model.add_default_instances();

        WHEN("model is rotated, scaled and set as sinking") {
            ModelObject* object = model.objects.front();
            object->center_around_origin(false);

            // set instance's attitude so that it is rotated, scaled and sinking
            ModelInstance* instance = object->instances.front();
            instance->set_rotation(X, -M_PI / 4.0);
            instance->set_offset(Vec3d::Zero());
            instance->set_scaling_factor({ 2.0, 2.0, 2.0 });

            // calculate 2D convex hull
            Polygon hull_2d = object->convex_hull_2d(instance->get_transformation().get_matrix());

            // verify result
            Points result = {
                { -91501496, -15914144 },
                { 91501496, -15914144 },
                { 91501496, 4243 },
                { 78229680, 4246883 },
                { 56898100, 4246883 },
                { -85501496, 4242641 },
                { -91501496, 4243 }
            };

            // Allow 1um error due to floating point rounding.
            bool res = hull_2d.points.size() == result.size();
            if (res)
                for (size_t i = 0; i < result.size(); ++ i) {
                    const Point &p1 = result[i];
                    const Point &p2 = hull_2d.points[i];
                    if (std::abs(p1.x() - p2.x()) > 1 || std::abs(p1.y() - p2.y()) > 1) {
                        res = false;
                        break;
                    }
                }

            THEN("2D convex hull should match with reference") {
                REQUIRE(res);
            }
        }
    }
}

SCENARIO("Import namespaced volumetric 3mf", "[3mf]") {
    GIVEN("a minimal 3mf archive with prefixed volumetric element and attribute names") {
        const boost::filesystem::path test_file =
            boost::filesystem::current_path() /
            boost::filesystem::unique_path("bioslicer-namespaced-volumetric-%%%%-%%%%-%%%%.3mf");
        const ArchiveWriteResult write_result = write_test_3mf_archive(test_file.string());
        INFO(write_result.error);
        REQUIRE(write_result.ok);

        Model model;
        DynamicPrintConfig config;
        ConfigSubstitutionContext ctxt{ ForwardCompatibilitySubstitutionRule::Disable };
        boost::optional<Semver> version;

        WHEN("the archive is loaded") {
            const bool ret = load_3mf(test_file.string().c_str(), config, ctxt, &model, false, version);
            boost::filesystem::remove(test_file);

            THEN("import succeeds without inventing an extruder assignment") {
                REQUIRE(ret);
                REQUIRE(model.objects.size() == 1);
                REQUIRE(model.objects.front()->volumes.size() == 1);

                const ConfigOption* object_extruder_opt = model.objects.front()->config.option("extruder");
                REQUIRE(object_extruder_opt == nullptr);

                const ConfigOption* volume_extruder_opt = model.objects.front()->volumes.front()->config.option("extruder");
                REQUIRE(volume_extruder_opt == nullptr);
            }
        }
    }
}

SCENARIO("Import namespaced volumetric 3mf with duplicate channel labels", "[3mf]") {
    GIVEN("a minimal 3mf archive with two volume ranges and duplicate composite channel labels") {
        const boost::filesystem::path test_file =
            boost::filesystem::current_path() /
            boost::filesystem::unique_path("bioslicer-namespaced-volumetric-duplicate-%%%%-%%%%-%%%%.3mf");
        const ArchiveWriteResult write_result = write_test_3mf_archive_two_volume_duplicate_channels(test_file.string());
        INFO(write_result.error);
        REQUIRE(write_result.ok);

        Model model;
        DynamicPrintConfig config;
        ConfigSubstitutionContext ctxt{ ForwardCompatibilitySubstitutionRule::Disable };
        boost::optional<Semver> version;

        WHEN("the archive is loaded") {
            const bool ret = load_3mf(test_file.string().c_str(), config, ctxt, &model, false, version);
            boost::filesystem::remove(test_file);

            THEN("volumetric labels do not invent mesh-volume channel assignments") {
                REQUIRE(ret);
                REQUIRE(model.objects.size() == 1);
                REQUIRE(model.objects.front()->volumes.size() == 2);

                const ConfigOption* object_extruder_opt = model.objects.front()->config.option("extruder");
                REQUIRE(object_extruder_opt == nullptr);

                const ConfigOption* volume0_extruder_opt = model.objects.front()->volumes[0]->config.option("extruder");
                const ConfigOption* volume1_extruder_opt = model.objects.front()->volumes[1]->config.option("extruder");
                REQUIRE(volume0_extruder_opt == nullptr);
                REQUIRE(volume1_extruder_opt == nullptr);
            }
        }
    }
}

SCENARIO("Import namespaced volumetric 3mf with distinct channel labels", "[3mf]") {
    GIVEN("a minimal 3mf archive with two volume ranges and distinct composite channel labels") {
        const boost::filesystem::path test_file =
            boost::filesystem::current_path() /
            boost::filesystem::unique_path("bioslicer-namespaced-volumetric-distinct-%%%%-%%%%-%%%%.3mf");
        const ArchiveWriteResult write_result = write_test_3mf_archive_two_volume_distinct_channels(test_file.string());
        INFO(write_result.error);
        REQUIRE(write_result.ok);

        Model model;
        DynamicPrintConfig config;
        ConfigSubstitutionContext ctxt{ ForwardCompatibilitySubstitutionRule::Disable };
        boost::optional<Semver> version;

        WHEN("the archive is loaded") {
            const bool ret = load_3mf(test_file.string().c_str(), config, ctxt, &model, false, version);
            boost::filesystem::remove(test_file);

            THEN("distinct labels remain metadata until spatial evaluation is supported") {
                REQUIRE(ret);
                REQUIRE(model.objects.size() == 1);
                REQUIRE(model.objects.front()->volumes.size() == 2);

                const ConfigOption* object_extruder_opt = model.objects.front()->config.option("extruder");
                REQUIRE(object_extruder_opt == nullptr);

                const ConfigOption* volume0_extruder_opt = model.objects.front()->volumes[0]->config.option("extruder");
                const ConfigOption* volume1_extruder_opt = model.objects.front()->volumes[1]->config.option("extruder");
                REQUIRE(volume0_extruder_opt == nullptr);
                REQUIRE(volume1_extruder_opt == nullptr);
            }
        }
    }
}

// Regression coverage for C6-2: get_attribute_value_int() (used for the
// primary, non-namespaced <object id="..."> resource attribute) must not
// silently default a malformed or missing id to zero. Manually reproduced
// against the built CLI (build-default/src/prusa-slicer --info) before
// adding this test: both files below were previously imported successfully
// with a silent id=0, and are now cleanly rejected.
SCENARIO("Importing a 3mf with a malformed object id fails cleanly", "[3mf]") {
    GIVEN("a minimal 3mf archive whose <object id=\"...\"> is not a valid integer") {
        const boost::filesystem::path test_file =
            boost::filesystem::current_path() /
            boost::filesystem::unique_path("bioslicer-malformed-object-id-%%%%-%%%%-%%%%.3mf");
        const ArchiveWriteResult write_result =
            write_test_3mf_archive_with_model_xml(test_file.string(), make_malformed_object_id_model_xml());
        INFO(write_result.error);
        REQUIRE(write_result.ok);

        Model model;
        DynamicPrintConfig config;
        ConfigSubstitutionContext ctxt{ ForwardCompatibilitySubstitutionRule::Disable };
        boost::optional<Semver> version;

        WHEN("the archive is loaded") {
            const bool ret = load_3mf(test_file.string().c_str(), config, ctxt, &model, false, version);
            boost::filesystem::remove(test_file);

            THEN("import fails instead of silently treating the object as id zero") {
                REQUIRE_FALSE(ret);
            }
        }
    }
}

SCENARIO("Importing a 3mf with a missing object id fails cleanly", "[3mf]") {
    GIVEN("a minimal 3mf archive whose <object> has no id attribute at all") {
        const boost::filesystem::path test_file =
            boost::filesystem::current_path() /
            boost::filesystem::unique_path("bioslicer-missing-object-id-%%%%-%%%%-%%%%.3mf");
        const ArchiveWriteResult write_result =
            write_test_3mf_archive_with_model_xml(test_file.string(), make_missing_object_id_model_xml());
        INFO(write_result.error);
        REQUIRE(write_result.ok);

        Model model;
        DynamicPrintConfig config;
        ConfigSubstitutionContext ctxt{ ForwardCompatibilitySubstitutionRule::Disable };
        boost::optional<Semver> version;

        WHEN("the archive is loaded") {
            const bool ret = load_3mf(test_file.string().c_str(), config, ctxt, &model, false, version);
            boost::filesystem::remove(test_file);

            THEN("import fails instead of silently treating the object as id zero") {
                REQUIRE_FALSE(ret);
            }
        }
    }
}

SCENARIO("Vessel tree parameters of an object survive a 3mf file cycle", "[3mf][Vessel]") {
    GIVEN("a model with a plain object and an object made by the vessel tree generator") {
        Model src_model;
        load_stl((std::string(TEST_DATA_DIR) + "/test_3mf/Prusa.stl").c_str(), &src_model);
        src_model.add_default_instances();
        ModelObject *plain = src_model.objects.front();
        ModelObject *tree  = src_model.add_object(*plain);
        tree->name = "Vessel tree";

        vessel::VesselSpec spec;
        spec.tree.depth       = 4;
        spec.tree.root_radius = 2.5 / 3.; // not a short decimal
        spec.tree.angle_jitter = 7.;
        spec.tree.seed        = 77;
        spec.mesh.wall_thickness = 0.4;
        spec.bounds_object    = "Heart wall";
        tree->vessel_tree_spec = vessel::spec_to_text(spec);

        WHEN("the model is copied") {
            Model copy = src_model;
            THEN("the parameters are copied with the object") {
                REQUIRE(copy.objects[0]->vessel_tree_spec.empty());
                REQUIRE(copy.objects[1]->vessel_tree_spec == tree->vessel_tree_spec);
            }
        }

        WHEN("the model is saved and loaded as a 3mf file") {
            const std::string test_file = std::string(TEST_DATA_DIR) + "/test_3mf/vessel_tree.3mf";
            REQUIRE(store_3mf(test_file.c_str(), &src_model, nullptr, false));

            Model dst_model;
            DynamicPrintConfig dst_config;
            {
                ConfigSubstitutionContext ctxt{ ForwardCompatibilitySubstitutionRule::Disable };
                boost::optional<Semver> version;
                REQUIRE(load_3mf(test_file.c_str(), dst_config, ctxt, &dst_model, false, version));
            }
            boost::filesystem::remove(test_file);

            THEN("only the generated object has parameters and they regenerate the same tree") {
                REQUIRE(dst_model.objects.size() == 2);
                REQUIRE(dst_model.objects[0]->vessel_tree_spec.empty());
                vessel::VesselSpec loaded;
                REQUIRE(vessel::spec_from_text(dst_model.objects[1]->vessel_tree_spec, loaded));
                REQUIRE(loaded.tree.depth == 4);
                REQUIRE(loaded.tree.seed == 77);
                REQUIRE(loaded.mesh.wall_thickness == Catch::Approx(0.4));
                REQUIRE(loaded.bounds_object == "Heart wall");
                const vessel::Skeleton a = vessel::generate_tree(spec.tree), b = vessel::generate_tree(loaded.tree);
                // Every branch is the same to the last bit.
                REQUIRE(vessel::skeleton_to_csv(a) == vessel::skeleton_to_csv(b));
                REQUIRE(a.branches.size() == b.branches.size());
                for (size_t i = 0; i < a.branches.size(); ++i) {
                    REQUIRE(a.branches[i].start() == b.branches[i].start());
                    REQUIRE(a.branches[i].end() == b.branches[i].end());
                    REQUIRE(a.branches[i].start_radius() == b.branches[i].start_radius());
                }
                // So is the mesh made from it.
                vessel::VesselMeshParams small_mesh = spec.mesh;
                small_mesh.sides = 12;
                vessel::TreeParams small = spec.tree;
                small.depth = 2;
                REQUIRE(vessel::make_tree_mesh(vessel::generate_tree(small), small_mesh).vertices ==
                        vessel::make_tree_mesh(vessel::generate_tree(small), small_mesh).vertices);
            }
        }
    }
}
