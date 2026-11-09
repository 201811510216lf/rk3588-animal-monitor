#include <string>

#include "test_harness.h"

#include "animal_monitor/config/config_loader.h"

using animal_monitor::config::ConfigLoader;
using animal_monitor::config::ConfigValidator;

namespace
{

const char* kValidConfig = R"(
board_profile: generic-rk3588

cameras:
  camera0:
    source: mock
    device: /dev/video0
    width: 1920
    height: 1080
    fps: 30
  camera1:
    source: mock
    device: /dev/video1
    width: 1920
    height: 1080
    fps: 30

composition:
  layout: side_by_side
  sync_tolerance_ms: 40

encoder:
  codec: h264
  bitrate_bps: 2000000
  gop: 60

network:
  rtsp_port: 8554
  mqtt:
    broker: "mqtt://192.0.2.10:1883"   # 占位地址，不含任何真实凭据

models:
  enabled: false

sensors:
  mlx90640:
    device: /dev/thermal0
    refresh_rate_hz: 4
  htu21d:
    iio_device: iio:device0

peripherals:
storage:
ota:

logging:
  level: info

health:
  interval_ms: 5000

queues:
  default_capacity: 8

app:
  video_pipeline:
    enabled: true
  mock_pipeline:
    enabled: true
    producer_interval_ms: 10
    duration_ms: 500
)";

AM_TEST(parse_valid_config)
{
    animal_monitor::config::ConfigValue root;
    const auto                          status = ConfigLoader::ParseString(kValidConfig, &root);
    AM_EXPECT(status.Ok());
    AM_EXPECT(root.IsMap());
}

AM_TEST(validator_accepts_valid_config)
{
    animal_monitor::config::ConfigValue root;
    AM_EXPECT(ConfigLoader::ParseString(kValidConfig, &root).Ok());

    std::string problems;
    const auto  status = ConfigValidator::Validate(root, &problems);
    AM_EXPECT(status.Ok());
    AM_EXPECT(problems.empty());
}

AM_TEST(nested_map_and_sequence_round_trip)
{
    const std::string                   text = R"(
streams:
  - name: camera0
    url: "rtsp://192.0.2.1/live/0"
  - name: camera1
    url: "rtsp://192.0.2.1/live/1"
retry:
  max_attempts: 3
  enabled: true
)";
    animal_monitor::config::ConfigValue root;
    AM_EXPECT(ConfigLoader::ParseString(text, &root).Ok());

    const auto* streams = root.Find("streams");
    AM_EXPECT(streams != nullptr);
    AM_EXPECT(streams->IsSequence());
    AM_EXPECT_EQ(streams->Sequence().size(), 2u);

    const auto& first = streams->Sequence()[0];
    AM_EXPECT(first.IsMap());
    AM_EXPECT_EQ(first.Find("name")->Scalar(), "camera0");
    AM_EXPECT_EQ(first.Find("url")->Scalar(), "rtsp://192.0.2.1/live/0");

    std::int64_t attempts = 0;
    AM_EXPECT(root.Find("retry")->Find("max_attempts")->AsInt64(&attempts));
    AM_EXPECT_EQ(attempts, 3);

    bool enabled = false;
    AM_EXPECT(root.Find("retry")->Find("enabled")->AsBool(&enabled));
    AM_EXPECT(enabled);
}

AM_TEST(sequence_of_scalars)
{
    const std::string                   text = R"(
ports:
  - 8554
  - 8000
  - 8001
)";
    animal_monitor::config::ConfigValue root;
    AM_EXPECT(ConfigLoader::ParseString(text, &root).Ok());

    const auto* ports = root.Find("ports");
    AM_EXPECT(ports != nullptr && ports->IsSequence());
    AM_EXPECT_EQ(ports->Sequence().size(), 3u);

    std::int64_t value = 0;
    AM_EXPECT(ports->Sequence()[0].AsInt64(&value));
    AM_EXPECT_EQ(value, 8554);
}

AM_TEST(inline_comment_and_quoted_string)
{
    const std::string                   text = R"(
name: "device #0"   # 行内注释
path: /dev/video0
)";
    animal_monitor::config::ConfigValue root;
    AM_EXPECT(ConfigLoader::ParseString(text, &root).Ok());
    AM_EXPECT_EQ(root.Find("name")->Scalar(), "device #0");
    AM_EXPECT_EQ(root.Find("path")->Scalar(), "/dev/video0");
}

AM_TEST(duplicate_key_is_error)
{
    const std::string                   text = "a: 1\na: 2\n";
    animal_monitor::config::ConfigValue root;
    const auto                          status = ConfigLoader::ParseString(text, &root);
    AM_EXPECT(!status.Ok());
}

AM_TEST(tab_indentation_is_error)
{
    const std::string                   text = "a:\n\tb: 1\n";
    animal_monitor::config::ConfigValue root;
    const auto                          status = ConfigLoader::ParseString(text, &root);
    AM_EXPECT(!status.Ok());
}

AM_TEST(content_under_scalar_is_error)
{
    const std::string                   text = "a: 1\n  b: 2\n";
    animal_monitor::config::ConfigValue root;
    const auto                          status = ConfigLoader::ParseString(text, &root);
    AM_EXPECT(!status.Ok());
}

AM_TEST(validator_reports_missing_keys)
{
    const std::string text = "cameras:\n";
    // 顶层键缺失与 cameras.camera0 缺失都应在 problems 中
    animal_monitor::config::ConfigValue root;
    AM_EXPECT(ConfigLoader::ParseString(text, &root).Ok());

    std::string problems;
    const auto  status = ConfigValidator::Validate(root, &problems);
    AM_EXPECT(!status.Ok());
    AM_EXPECT(problems.find("board_profile") != std::string::npos);
    AM_EXPECT(problems.find("cameras.camera0") != std::string::npos);
}

AM_TEST(root_must_be_mapping)
{
    const std::string                   text = "- item1\n- item2\n";
    animal_monitor::config::ConfigValue root;
    const auto                          status = ConfigLoader::ParseString(text, &root);
    AM_EXPECT(!status.Ok());
}

AM_TEST(unterminated_quote_is_error)
{
    const std::string                   text = "a: \"abc\n";
    animal_monitor::config::ConfigValue root;
    const auto                          status = ConfigLoader::ParseString(text, &root);
    AM_EXPECT(!status.Ok());
}

}  // namespace

int main() { return am_test::RunAll(); }
