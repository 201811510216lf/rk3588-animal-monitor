#include "test_harness.h"

// ============================================================================
// 文件：tests/unit/test_roi_mapper.cpp
// 依据：任务书 CODEX_MASTER_TASK.md §14（host tests：ROI mapper）；
//       include/animal_monitor/calibration/roi_mapper.h
// 覆盖：已知映射点/框数值、非法 scale 拒绝、config 加载往返与错误路径。
// ============================================================================

#include <cmath>
#include <string>

#include "animal_monitor/calibration/roi_mapper.h"

using animal_monitor::Status;
using animal_monitor::StatusCode;
using animal_monitor::calibration::CameraThermalMapping;
using animal_monitor::calibration::LoadMappingFromConfig;
using animal_monitor::calibration::MapBboxToThermal;
using animal_monitor::calibration::MapPointToThermal;
using animal_monitor::calibration::ThermalRoi;
using animal_monitor::calibration::ValidateMapping;
using animal_monitor::config::ConfigValue;

namespace
{

bool Near(float a, float b) { return std::fabs(a - b) <= 1e-4f; }

CameraThermalMapping KnownMapping()
{
    CameraThermalMapping mapping;
    mapping.scale_x  = 2.0f;
    mapping.scale_y  = 2.0f;
    mapping.offset_x = 10.0f;
    mapping.offset_y = 10.0f;
    return mapping;
}

AM_TEST(map_point_with_known_mapping)
{
    const CameraThermalMapping mapping = KnownMapping();
    float                      tx      = 0.0f;
    float                      ty      = 0.0f;
    const Status               status  = MapPointToThermal(mapping, 3.0f, 4.0f, &tx, &ty);
    AM_EXPECT(status.Ok());
    AM_EXPECT(Near(tx, 16.0f));  // 3*2+10
    AM_EXPECT(Near(ty, 18.0f));  // 4*2+10
}

AM_TEST(map_bbox_with_known_mapping)
{
    const CameraThermalMapping mapping = KnownMapping();
    const float                bbox[4] = {2.0f, 3.0f, 4.0f, 5.0f};
    ThermalRoi                 roi;
    const Status               status = MapBboxToThermal(mapping, bbox, &roi);
    AM_EXPECT(status.Ok());
    AM_EXPECT(Near(roi.x0, 14.0f));  // 2*2+10
    AM_EXPECT(Near(roi.y0, 16.0f));  // 3*2+10
    AM_EXPECT(Near(roi.x1, 22.0f));  // (2+4)*2+10
    AM_EXPECT(Near(roi.y1, 26.0f));  // (3+5)*2+10
}

AM_TEST(rejects_zero_or_negative_scale)
{
    CameraThermalMapping mapping = KnownMapping();
    mapping.scale_x              = 0.0f;
    std::string err;
    AM_EXPECT(!ValidateMapping(mapping, &err).Ok());
    AM_EXPECT(!err.empty());

    mapping         = KnownMapping();
    mapping.scale_y = -1.0f;
    AM_EXPECT(!ValidateMapping(mapping, &err).Ok());
    AM_EXPECT(!err.empty());

    // 映射函数同样拒绝非法参数
    float        tx    = 0.0f;
    float        ty    = 0.0f;
    const Status point = MapPointToThermal(mapping, 1.0f, 1.0f, &tx, &ty);
    AM_EXPECT_EQ(point.Code(), StatusCode::kInvalidArgument);
}

AM_TEST(rejects_out_of_range_offset)
{
    CameraThermalMapping mapping = KnownMapping();
    mapping.offset_x             = 1e6f;
    std::string err;
    AM_EXPECT(!ValidateMapping(mapping, &err).Ok());
    AM_EXPECT(!err.empty());
}

AM_TEST(rejects_negative_bbox_size)
{
    const CameraThermalMapping mapping = KnownMapping();
    const float                bbox[4] = {0.0f, 0.0f, -1.0f, 10.0f};
    ThermalRoi                 roi;
    const Status               status = MapBboxToThermal(mapping, bbox, &roi);
    AM_EXPECT_EQ(status.Code(), StatusCode::kInvalidArgument);
}

AM_TEST(config_round_trip)
{
    ConfigValue root = ConfigValue::MakeMap();
    root.Insert("scale_x", ConfigValue::MakeScalar("0.25"));
    root.Insert("scale_y", ConfigValue::MakeScalar("0.5"));
    root.Insert("offset_x", ConfigValue::MakeScalar("3"));
    root.Insert("offset_y", ConfigValue::MakeScalar("-7"));

    CameraThermalMapping mapping;
    const Status         status = LoadMappingFromConfig(root, &mapping);
    AM_EXPECT(status.Ok());
    AM_EXPECT(Near(mapping.scale_x, 0.25f));
    AM_EXPECT(Near(mapping.scale_y, 0.5f));
    AM_EXPECT(Near(mapping.offset_x, 3.0f));
    AM_EXPECT(Near(mapping.offset_y, -7.0f));
}

AM_TEST(config_missing_key_rejected)
{
    ConfigValue root = ConfigValue::MakeMap();
    root.Insert("scale_x", ConfigValue::MakeScalar("0.25"));
    root.Insert("scale_y", ConfigValue::MakeScalar("0.5"));
    root.Insert("offset_x", ConfigValue::MakeScalar("3"));
    // 缺少 offset_y
    CameraThermalMapping mapping;
    const Status         status = LoadMappingFromConfig(root, &mapping);
    AM_EXPECT_EQ(status.Code(), StatusCode::kConfigurationError);
}

AM_TEST(config_non_numeric_rejected)
{
    ConfigValue root = ConfigValue::MakeMap();
    root.Insert("scale_x", ConfigValue::MakeScalar("abc"));
    root.Insert("scale_y", ConfigValue::MakeScalar("0.5"));
    root.Insert("offset_x", ConfigValue::MakeScalar("3"));
    root.Insert("offset_y", ConfigValue::MakeScalar("-7"));
    CameraThermalMapping mapping;
    const Status         status = LoadMappingFromConfig(root, &mapping);
    AM_EXPECT_EQ(status.Code(), StatusCode::kConfigurationError);
}

AM_TEST(config_non_map_rejected)
{
    const ConfigValue    scalar = ConfigValue::MakeScalar("1");
    CameraThermalMapping mapping;
    const Status         status = LoadMappingFromConfig(scalar, &mapping);
    AM_EXPECT_EQ(status.Code(), StatusCode::kConfigurationError);
}

AM_TEST(config_out_of_range_value_rejected)
{
    ConfigValue root = ConfigValue::MakeMap();
    root.Insert("scale_x", ConfigValue::MakeScalar("0.25"));
    root.Insert("scale_y", ConfigValue::MakeScalar("0.5"));
    root.Insert("offset_x", ConfigValue::MakeScalar("999999"));
    root.Insert("offset_y", ConfigValue::MakeScalar("-7"));
    CameraThermalMapping mapping;
    const Status         status = LoadMappingFromConfig(root, &mapping);
    AM_EXPECT_EQ(status.Code(), StatusCode::kInvalidArgument);
}

}  // namespace

int main() { return am_test::RunAll(); }
