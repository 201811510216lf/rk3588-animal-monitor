// tests/unit/test_model_registry.cpp
// 状态：implemented（host）
// 说明：临时目录构造 manifest 场景：正常校验、模型缺失、sha256 不匹配、
//       占位 sha256、非 yaml 跳过、非 manifest 格式、重复 model_id。

#include <chrono>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

#include "test_harness.h"

#include "animal_monitor/common/sha256.h"
#include "animal_monitor/inference/model_manifest.h"
#include "animal_monitor/inference/model_registry.h"

using animal_monitor::Sha256File;
using animal_monitor::Status;
using animal_monitor::StatusCode;
using animal_monitor::inference::LoadManifestFile;
using animal_monitor::inference::ModelManifest;
using animal_monitor::inference::ModelRegistry;
using animal_monitor::inference::VerifyManifestFileSha256;

namespace {

// 唯一临时目录；析构递归清理（error_code 静默，测试进程对 TEMP 有写权限）。
class TempDir
{
  public:
    TempDir()
    {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path_            = std::filesystem::temp_directory_path() /
                ("am_model_registry_test_" + std::to_string(stamp));
        std::filesystem::create_directories(path_);
    }

    ~TempDir()
    {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    std::string Write(const std::string& name, const std::string& content)
    {
        const std::filesystem::path file_path = path_ / name;
        std::ofstream               file(file_path, std::ios::binary);
        file << content;
        return file_path.generic_string();  // 正斜杠路径：YAML 引号内反斜杠会被当转义
    }

    std::string Dir() const { return path_.generic_string(); }

  private:
    std::filesystem::path path_;
};

std::string MakeManifest(const std::string& model_id, const std::string& file,
                         const std::string& sha256)
{
    std::string yaml;
    yaml += "model_id: \"" + model_id + "\"\n";
    yaml += "model_kind: detector\n";
    yaml += "file: \"" + file + "\"\n";
    yaml += "sha256: \"" + sha256 + "\"\n";
    yaml += "classes: [person]\n";
    yaml += "thresholds:\n  confidence: 0.25\n  nms: 0.45\n";
    return yaml;
}

AM_TEST(registry_verifies_ok_model)
{
    TempDir           temp;
    const std::string model_path = temp.Write("model_a.rknn", "hello model a");
    std::string       sha;
    AM_EXPECT(Sha256File(model_path, &sha).Ok());
    temp.Write("a.yaml", MakeManifest("det_a", model_path, sha));

    ModelRegistry registry;
    const Status  status = registry.LoadFromDir(temp.Dir());
    AM_EXPECT(status.Ok());
    AM_EXPECT_EQ(registry.Count(), 1u);

    const std::vector<std::string> available = registry.AvailableModels();
    AM_EXPECT_EQ(available.size(), 1u);
    if (available.size() == 1)
    {
        AM_EXPECT_EQ(available[0], "det_a");
    }
    const ModelManifest* manifest = registry.Find("det_a");
    AM_EXPECT(manifest != nullptr);
    if (manifest != nullptr)
    {
        AM_EXPECT_EQ(manifest->model_id, "det_a");
        AM_EXPECT_EQ(manifest->classes.size(), 1u);
        if (manifest->classes.size() == 1)
        {
            AM_EXPECT_EQ(manifest->classes[0], "person");
        }
    }
    const auto records = registry.StatusRecords();
    AM_EXPECT(records.count("det_a") == 1);
    if (records.count("det_a") == 1)
    {
        AM_EXPECT_EQ(records.at("det_a").status, "ok");
    }
}

AM_TEST(registry_missing_model_not_blocking)
{
    TempDir temp;
    temp.Write("b.yaml",
               MakeManifest("det_b", temp.Dir() + "/not_exist.rknn",
                            "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"));

    ModelRegistry registry;
    AM_EXPECT(registry.LoadFromDir(temp.Dir()).Ok());
    AM_EXPECT_EQ(registry.Count(), 1u);
    AM_EXPECT(registry.Find("det_b") != nullptr);  // 缺失不阻断注册

    const auto available = registry.AvailableModels();
    AM_EXPECT(available.empty());  // status != ok → 不可用

    const auto records = registry.StatusRecords();
    AM_EXPECT(records.count("det_b") == 1);
    if (records.count("det_b") == 1)
    {
        AM_EXPECT_EQ(records.at("det_b").status, "model_missing");
        AM_EXPECT(records.at("det_b").message.find("无 AI 模式") != std::string::npos);
    }
}

AM_TEST(registry_sha256_mismatch_and_placeholder)
{
    TempDir           temp;
    const std::string model_path = temp.Write("model_c.rknn", "content c");
    // 不匹配：sha256 填 64 个 '0'。
    temp.Write("c.yaml", MakeManifest("det_c", model_path, std::string(64, '0')));
    // 占位：NOT_COMPUTED。
    temp.Write("d.yaml", MakeManifest("det_d", model_path, "NOT_COMPUTED"));

    ModelRegistry registry;
    AM_EXPECT(registry.LoadFromDir(temp.Dir()).Ok());
    const auto records = registry.StatusRecords();
    AM_EXPECT_EQ(records.at("det_c").status, "sha256_mismatch");
    AM_EXPECT_EQ(records.at("det_d").status, "sha256_unknown");
}

AM_TEST(registry_skips_non_yaml_and_invalid_manifest)
{
    TempDir temp;
    temp.Write("readme.txt", "not a manifest");
    // legacy_detectors.yaml 风格：顶层 models 列表，无 model_id → manifest_invalid。
    temp.Write("legacy.yaml", "models:\n  - model_id: x\n    file: y\n");

    ModelRegistry registry;
    AM_EXPECT(registry.LoadFromDir(temp.Dir()).Ok());
    AM_EXPECT_EQ(registry.Count(), 0u);

    const auto records = registry.StatusRecords();
    AM_EXPECT(records.count("file:readme.txt") == 1);
    if (records.count("file:readme.txt") == 1)
    {
        AM_EXPECT_EQ(records.at("file:readme.txt").status, "skipped_non_yaml");
    }
    AM_EXPECT(records.count("file:legacy.yaml") == 1);
    if (records.count("file:legacy.yaml") == 1)
    {
        AM_EXPECT_EQ(records.at("file:legacy.yaml").status, "manifest_invalid");
    }
}

AM_TEST(registry_rejects_duplicate_model_id)
{
    TempDir temp;
    temp.Write("a1.yaml", MakeManifest("dup_id", temp.Dir() + "/m1.rknn", std::string(64, '1')));
    temp.Write("a2.yaml", MakeManifest("dup_id", temp.Dir() + "/m2.rknn", std::string(64, '2')));

    ModelRegistry registry;
    AM_EXPECT(registry.LoadFromDir(temp.Dir()).Ok());
    AM_EXPECT_EQ(registry.Count(), 1u);
    const auto records = registry.StatusRecords();
    AM_EXPECT_EQ(records.at("dup_id").status, "duplicate_model_id");
}

AM_TEST(verify_missing_file_message_mentions_no_ai_mode)
{
    ModelManifest manifest;
    manifest.model_id   = "det_x";
    manifest.file       = "definitely_missing_model_file.rknn";
    manifest.sha256     = std::string(64, 'a');
    const Status status = VerifyManifestFileSha256(manifest);
    AM_EXPECT_EQ(static_cast<int>(status.Code()), static_cast<int>(StatusCode::kNotFound));
    AM_EXPECT(status.Message().find("无 AI 模式") != std::string::npos);

    manifest.file                  = "";
    const Status empty_file_status = VerifyManifestFileSha256(manifest);
    AM_EXPECT_EQ(static_cast<int>(empty_file_status.Code()),
                 static_cast<int>(StatusCode::kNotFound));
}

AM_TEST(verify_sha256_case_insensitive)
{
    TempDir           temp;
    const std::string model_path = temp.Write("model_e.rknn", "content e");
    std::string       sha;
    AM_EXPECT(Sha256File(model_path, &sha).Ok());
    std::string upper;
    for (const char c : sha)
    {
        upper.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    }

    ModelManifest manifest;
    manifest.model_id = "det_e";
    manifest.file     = model_path;
    manifest.sha256   = upper;
    AM_EXPECT(VerifyManifestFileSha256(manifest).Ok());
}

AM_TEST(load_manifest_parses_template_fields)
{
    TempDir           temp;
    const std::string yaml =
        "model_id: \"grooming_v1\"\n"
        "model_kind: behavior\n"
        "task: grooming\n"
        "file: \"/srv/models/grooming.rknn\"\n"
        "sha256: \"" +
        std::string(64, 'f') +
        "\"\n"
        "rknn:\n  sdk_version: 1.6.0\n  npu_core_mask: 7\n  wants_float: false\n"
        "input:\n  - name: images\n    shape: [1, 640, 640, 3]\n    layout: NHWC\n"
        "    data_type: uint8\n    color_format: RGB\n    letterbox: false\n    mean: []\n    std: "
        "[]\n"
        "output:\n  - name: output0\n    shape: UNKNOWN\n    data_type: int8\n"
        "    quantization_scale: UNKNOWN\n    quantization_zp: UNKNOWN\n    postprocess: yolov5\n"
        "classes: [grooming, other]\n"
        "thresholds:\n  confidence: 0.25\n  nms: 0.45\n"
        "version: \"1.0\"\n"
        "status: UNKNOWN\n";
    const std::string path = temp.Write("grooming.yaml", yaml);

    ModelManifest manifest;
    const Status  status = LoadManifestFile(path, &manifest);
    AM_EXPECT(status.Ok());
    AM_EXPECT_EQ(manifest.model_id, "grooming_v1");
    AM_EXPECT_EQ(manifest.task, "grooming");
    AM_EXPECT_EQ(manifest.rknn.sdk_version, "1.6.0");
    AM_EXPECT_EQ(manifest.rknn.npu_core_mask, 7);
    AM_EXPECT(!manifest.rknn.wants_float);
    AM_EXPECT_EQ(manifest.inputs.size(), 1u);
    if (manifest.inputs.size() == 1)
    {
        AM_EXPECT_EQ(manifest.inputs[0].shape.size(), 4u);
        if (manifest.inputs[0].shape.size() == 4)
        {
            AM_EXPECT_EQ(manifest.inputs[0].shape[1], 640);
            AM_EXPECT_EQ(manifest.inputs[0].shape[2], 640);
            AM_EXPECT_EQ(manifest.inputs[0].shape[3], 3);
        }
        AM_EXPECT(!manifest.inputs[0].letterbox);
    }
    AM_EXPECT_EQ(manifest.outputs.size(), 1u);
    if (manifest.outputs.size() == 1)
    {
        AM_EXPECT(!manifest.outputs[0].has_quantization_scale);
        AM_EXPECT(!manifest.outputs[0].has_quantization_zp);
        AM_EXPECT_EQ(manifest.outputs[0].postprocess, "yolov5");
    }
    AM_EXPECT_EQ(manifest.classes.size(), 2u);
    AM_EXPECT_EQ(manifest.version, "1.0");
}

}  // namespace

int main() { return am_test::RunAll(); }
