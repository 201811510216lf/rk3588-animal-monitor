#include "animal_monitor/app/video_pipeline_service.h"

#include <chrono>
#include <mutex>

#include "animal_monitor/common/bounded_queue.h"
#include "animal_monitor/logging/logger.h"

namespace animal_monitor
{

namespace
{

constexpr std::size_t kEncInputQueueCapacity = 4;

bool AsBool(const config::ConfigValue* parent, const char* key, bool fallback)
{
    if (parent == nullptr)
    {
        return fallback;
    }
    const config::ConfigValue* entry = parent->Find(key);
    bool                       value = fallback;
    if (entry != nullptr)
    {
        entry->AsBool(&value);
    }
    return value;
}

std::int64_t AsInt(const config::ConfigValue* parent, const char* key, std::int64_t fallback)
{
    if (parent == nullptr)
    {
        return fallback;
    }
    const config::ConfigValue* entry = parent->Find(key);
    std::int64_t               value = fallback;
    if (entry != nullptr)
    {
        entry->AsInt64(&value);
    }
    return value;
}

std::string AsString(const config::ConfigValue* parent, const char* key,
                     const std::string& fallback)
{
    if (parent == nullptr)
    {
        return fallback;
    }
    const config::ConfigValue* entry = parent->Find(key);
    if (entry != nullptr && entry->IsScalar())
    {
        return entry->Scalar();
    }
    return fallback;
}

}  // namespace

class VideoPipelineService::Impl
{
   public:
    std::unique_ptr<VideoSource>       sources[2];
    std::unique_ptr<FrameSynchronizer> synchronizer;
    std::unique_ptr<FrameCompositor>   compositor;
    std::unique_ptr<VideoEncoder>      encoder;
    std::unique_ptr<VideoPublisher>    publisher;

    BoundedQueue<VideoFramePtr> enc_input{kEncInputQueueCapacity,
                                          BoundedQueue<VideoFramePtr>::PushPolicy::kDropOldest};

    std::thread capture_threads[2];
    std::thread sync_thread;
    std::thread enc_in_thread;
    std::thread enc_out_thread;

    std::atomic<bool> stop_requested{false};

    mutable std::mutex stats_mutex;
    std::uint64_t      composed          = 0;
    std::uint64_t      encoded           = 0;
    std::uint64_t      published_packets = 0;
    std::uint64_t      published_bytes   = 0;
    std::uint64_t      encode_errors     = 0;
    std::uint64_t      compose_errors    = 0;
};

VideoPipelineService::~VideoPipelineService() { Stop(); }

const char* VideoPipelineService::Name() const { return "video_pipeline"; }

Status VideoPipelineService::BuildFromConfig(const config::ConfigValue&             root,
                                             std::unique_ptr<VideoPipelineService>* out)
{
    if (out == nullptr)
    {
        return Status(StatusCode::kInvalidArgument, "null output");
    }

    auto service   = std::unique_ptr<VideoPipelineService>(new VideoPipelineService());
    service->impl_ = std::make_unique<Impl>();

    // 双路视频源（配置缺失视为错误，见 ConfigValidator）。
    const config::ConfigValue* cameras = root.Find("cameras");
    if (cameras == nullptr || !cameras->IsMap())
    {
        return Status(StatusCode::kConfigurationError, "missing cameras section");
    }
    for (int id = 0; id < 2; ++id)
    {
        const char*                key = (id == 0) ? "camera0" : "camera1";
        const config::ConfigValue* cam = cameras->Find(key);
        if (cam == nullptr)
        {
            return Status(StatusCode::kConfigurationError, std::string("missing cameras.") + key);
        }
        service->impl_->sources[id] = CreateVideoSource(*cam, id);
        if (service->impl_->sources[id] == nullptr)
        {
            return Status(StatusCode::kUnimplemented,
                          std::string("unsupported camera source for ") + key +
                              " (host build supports 'mock'; v4l2 requires target build)");
        }
    }

    // 同步器：容差来自 composition.sync_tolerance_ms。
    const config::ConfigValue* composition = root.Find("composition");
    FrameSynchronizer::Config  sync_config;
    sync_config.tolerance_ns   = AsInt(composition, "sync_tolerance_ms", 40) * 1'000'000;
    sync_config.queue_capacity = static_cast<std::size_t>(AsInt(composition, "queue_capacity", 8));
    service->impl_->synchronizer = std::make_unique<FrameSynchronizer>(sync_config);

    // 合成器。
    CompositorConfig compose_config;
    compose_config.out_width = static_cast<std::uint32_t>(AsInt(composition, "output_width", 1280));
    compose_config.out_height =
        static_cast<std::uint32_t>(AsInt(composition, "output_height", 720));
    compose_config.gap_px    = static_cast<std::uint32_t>(AsInt(composition, "gap_px", 0));
    const std::string layout = AsString(composition, "layout", "side_by_side");
    compose_config.layout =
        (layout == "top_bottom") ? CompositorLayout::kTopBottom : CompositorLayout::kSideBySide;
    service->impl_->compositor = std::make_unique<FrameCompositor>(compose_config);

    // 编码器（配置来自 encoder 段）。
    const config::ConfigValue* enc_cfg = root.Find("encoder");
    EncoderConfig              encoder_config;
    encoder_config.width       = static_cast<std::uint32_t>(AsInt(enc_cfg, "width", 1280));
    encoder_config.height      = static_cast<std::uint32_t>(AsInt(enc_cfg, "height", 720));
    encoder_config.fps         = static_cast<std::uint32_t>(AsInt(enc_cfg, "fps", 30));
    encoder_config.bitrate_bps = AsInt(enc_cfg, "bitrate_bps", 2'000'000);
    encoder_config.gop         = static_cast<std::uint32_t>(AsInt(enc_cfg, "gop", 60));
    service->impl_->encoder    = CreateVideoEncoder(encoder_config);
    if (service->impl_->encoder == nullptr)
    {
        return Status(StatusCode::kUnimplemented, "failed to create video encoder");
    }

    // 发布器（配置来自 network 段）。
    const config::ConfigValue* network = root.Find("network");
    const config::ConfigValue* rtsp =
        (network != nullptr && network->IsMap()) ? network->Find("rtsp") : nullptr;
    VideoPublisher::Config publisher_config;
    if (rtsp != nullptr && rtsp->IsMap())
    {
        publisher_config.rtsp_port = static_cast<std::uint16_t>(AsInt(rtsp, "port", 8554));
        publisher_config.app       = AsString(rtsp, "app", "live");
        publisher_config.stream    = AsString(rtsp, "stream", "composite");
    }
    service->impl_->publisher = CreateVideoPublisher(publisher_config);
    if (service->impl_->publisher == nullptr)
    {
        return Status(StatusCode::kUnimplemented, "failed to create video publisher");
    }

    *out = std::move(service);
    return Status::MakeOk();
}

Status VideoPipelineService::Start()
{
    if (impl_->sync_thread.joinable())
    {
        return Status(StatusCode::kAlreadyExists, "video_pipeline already started");
    }
    for (auto& source : impl_->sources)
    {
        const Status status = source->Start();
        if (!status)
        {
            AM_LOG_ERROR() << "source " << source->Name() << " start failed: " << status.ToString();
            Stop();
            return status;
        }
    }

    impl_->stop_requested.store(false, std::memory_order_relaxed);
    // 线程：camera0/1 采集 → 同步+合成 → 编码输入 → 编码输出（见 thread_model.md）。
    impl_->capture_threads[0] = std::thread(&VideoPipelineService::CaptureLoop, this, 0);
    impl_->capture_threads[1] = std::thread(&VideoPipelineService::CaptureLoop, this, 1);
    impl_->sync_thread        = std::thread(&VideoPipelineService::SyncLoop, this);
    impl_->enc_in_thread      = std::thread(&VideoPipelineService::EncInLoop, this);
    impl_->enc_out_thread     = std::thread(&VideoPipelineService::EncOutLoop, this);
    return Status::MakeOk();
}

void VideoPipelineService::Stop()
{
    if (impl_ == nullptr)
    {
        return;
    }
    if (!impl_->stop_requested.load(std::memory_order_relaxed))
    {
        impl_->stop_requested.store(true, std::memory_order_relaxed);
        for (auto& source : impl_->sources)
        {
            source->Stop();  // 唤醒 WaitForFrame → 采集线程退出
        }
        impl_->synchronizer->Close();
        impl_->enc_input.Close();
    }
    for (auto& thread : impl_->capture_threads)
    {
        if (thread.joinable())
        {
            thread.join();
        }
    }
    if (impl_->sync_thread.joinable())
    {
        impl_->sync_thread.join();
    }
    if (impl_->enc_in_thread.joinable())
    {
        impl_->enc_in_thread.join();
    }
    if (impl_->enc_out_thread.joinable())
    {
        impl_->enc_out_thread.join();
    }
    impl_->publisher->Close();
}

VideoPipelineService::Snapshot VideoPipelineService::GetSnapshot() const
{
    Snapshot snapshot;
    snapshot.sync = impl_->synchronizer->GetMetrics();
    std::lock_guard<std::mutex> lock(impl_->stats_mutex);
    snapshot.composed          = impl_->composed;
    snapshot.encoded           = impl_->encoded;
    snapshot.published_packets = impl_->published_packets;
    snapshot.published_bytes   = impl_->published_bytes;
    snapshot.encode_errors     = impl_->encode_errors;
    snapshot.compose_errors    = impl_->compose_errors;
    return snapshot;
}

void VideoPipelineService::CaptureLoop(int camera_id)
{
    using namespace std::chrono_literals;
    while (!impl_->stop_requested.load(std::memory_order_relaxed))
    {
        VideoFramePtr frame;
        const Status  status = impl_->sources[camera_id]->WaitForFrame(&frame, 100ms);
        if (status.Code() == StatusCode::kTimeout)
        {
            continue;
        }
        if (status.Code() == StatusCode::kCancelled)
        {
            break;
        }
        if (!status)
        {
            AM_LOG_WARNING() << "capture" << camera_id << " error: " << status.ToString();
            break;
        }
        impl_->synchronizer->PushFrame(camera_id, std::move(frame));
    }
}

void VideoPipelineService::SyncLoop()
{
    using namespace std::chrono_literals;
    VideoFramePtr left;
    VideoFramePtr right;
    while (!impl_->stop_requested.load(std::memory_order_relaxed))
    {
        const Status status = impl_->synchronizer->PopPair(&left, &right, 200ms);
        if (status.Code() == StatusCode::kTimeout)
        {
            continue;
        }
        if (status.Code() == StatusCode::kCancelled)
        {
            break;
        }
        if (!status)
        {
            AM_LOG_WARNING() << "sync error: " << status.ToString();
            continue;
        }

        std::vector<VideoFramePtr> inputs;
        inputs.push_back(std::move(left));
        inputs.push_back(std::move(right));
        VideoFramePtr composed;
        const Status  compose_status = impl_->compositor->Compose(inputs, &composed);
        if (!compose_status)
        {
            std::lock_guard<std::mutex> lock(impl_->stats_mutex);
            ++impl_->compose_errors;
            continue;
        }
        {
            std::lock_guard<std::mutex> lock(impl_->stats_mutex);
            ++impl_->composed;
        }
        impl_->enc_input.Push(std::move(composed), 100ms);
    }
    impl_->enc_input.Close();
}

void VideoPipelineService::EncInLoop()
{
    using namespace std::chrono_literals;
    VideoFramePtr frame;
    while (!impl_->stop_requested.load(std::memory_order_relaxed))
    {
        if (!impl_->enc_input.Pop(frame, 200ms))
        {
            if (impl_->enc_input.IsClosed())
            {
                break;
            }
            continue;
        }
        const Status status = impl_->encoder->Encode(*frame);
        if (!status)
        {
            std::lock_guard<std::mutex> lock(impl_->stats_mutex);
            ++impl_->encode_errors;
        }
        else
        {
            std::lock_guard<std::mutex> lock(impl_->stats_mutex);
            ++impl_->encoded;
        }
    }
}

void VideoPipelineService::EncOutLoop()
{
    using namespace std::chrono_literals;
    EncodedPacket packet;
    while (!impl_->stop_requested.load(std::memory_order_relaxed))
    {
        const Status status = impl_->encoder->GetPacket(&packet, 100ms);
        if (status.Code() == StatusCode::kTimeout)
        {
            continue;
        }
        if (status.Code() == StatusCode::kCancelled)
        {
            break;
        }
        if (!status)
        {
            AM_LOG_WARNING() << "encoder get packet error: " << status.ToString();
            continue;
        }
        const Status feed = impl_->publisher->FeedH264(packet.data.data(), packet.data.size(),
                                                       packet.dts_us / 1000, packet.pts_us / 1000);
        if (feed)
        {
            std::lock_guard<std::mutex> lock(impl_->stats_mutex);
            ++impl_->published_packets;
            impl_->published_bytes += packet.data.size();
        }
    }
}

}  // namespace animal_monitor
