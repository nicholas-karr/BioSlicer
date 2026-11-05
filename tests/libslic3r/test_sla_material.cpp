#include <catch2/catch_test_macros.hpp>

#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/SLAMaterial.hpp"

using namespace Slic3r;

TEST_CASE("SLA region technology classification is role consistent", "[sla-material]")
{
    FullPrintConfig full_config;
    auto &print_config = static_cast<PrintConfig &>(full_config);
    auto &region_config = static_cast<PrintRegionConfig &>(full_config);
    print_config.sla_material_extruder.values = { false, true, true, false };

    SECTION("different FFF extruders remain one technology")
    {
        region_config.perimeter_extruder.value = 1;
        region_config.infill_extruder.value = 4;
        region_config.solid_infill_extruder.value = 4;
        const PrintRegion region(region_config);
        CHECK(classify_region_technology(print_config, region).technology
              == RegionTechnology::FFF);
    }

    SECTION("one SLA extruder across every role is supported")
    {
        region_config.perimeter_extruder.value = 2;
        region_config.infill_extruder.value = 2;
        region_config.solid_infill_extruder.value = 2;
        const PrintRegion region(region_config);
        const RegionTechnologyAssignment assignment =
            classify_region_technology(print_config, region);
        CHECK(assignment.technology == RegionTechnology::SLA);
        CHECK(assignment.sla_extruder == 2);
    }

    SECTION("mixing FFF and SLA roles is rejected")
    {
        region_config.perimeter_extruder.value = 1;
        region_config.infill_extruder.value = 2;
        region_config.solid_infill_extruder.value = 2;
        const PrintRegion region(region_config);
        CHECK(classify_region_technology(print_config, region).technology
              == RegionTechnology::Mixed);
    }

    SECTION("multiple SLA channels in one region are rejected")
    {
        region_config.perimeter_extruder.value = 2;
        region_config.infill_extruder.value = 3;
        region_config.solid_infill_extruder.value = 2;
        const PrintRegion region(region_config);
        CHECK(classify_region_technology(print_config, region).technology
              == RegionTechnology::Mixed);
    }
}

TEST_CASE("Material tokens are upper case ASCII identifiers", "[sla-material]")
{
    CHECK(material_token("fff", "Basic PLA @BIOSLICERTRIDENT") == "FFF_BASIC_PLA_BIOSLICERTRIDENT");
    CHECK(material_token("sla", "GelMA 10% @BIOSLICERTRIDENT") == "SLA_GELMA_10_BIOSLICERTRIDENT");
    CHECK(material_token("sla", "Alginate-GelMA Bioink @BIOSLICERTRIDENT") == "SLA_ALGINATE_GELMA_BIOINK_BIOSLICERTRIDENT");
    CHECK(material_token("", "PLA") == "PLA");
    CHECK(material_token("", "").empty());
    // Bytes outside of ASCII never count as letters, whatever the process locale is.
    CHECK(material_token("fff", "R\xc3\xa9sine \xce\xb1 1") == "FFF_R_SINE_1");
}
