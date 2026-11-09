#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "animal_monitor/common/frame_meta.h"

namespace animal_monitor
{

// 底层缓冲所有权句柄：目标实现持有 DMABUF fd / mmap / MPP buffer / RGA handle，
// 析构即释放；宿主实现可为空（CPU 内存）。
// 生命周期契约见 docs/architecture/buffer_lifecycle.md：
// 只要 VideoFramePtr 仍被下游持有，底层缓冲就不得归还/覆写。
class BufferOwner
{
   public:
    virtual ~BufferOwner() = default;
};

// CPU 可访问的平面数据视图（主机/mock/预处理路径使用）。
// 目标板零拷贝路径（RGA/MPP fd 直传）下 planes 可为空，由实现文档说明。
struct PlaneView
{
    const std::uint8_t* data = nullptr;
    std::size_t         size = 0;
    // 行字节步长（=0 表示按 meta.stride 或紧凑排列）
    std::uint32_t row_stride = 0;
};

// 帧：元数据 + 引用计数的底层缓冲。跨线程共享一律使用 VideoFramePtr。
class VideoFrame
{
   public:
    VideoFrame() = default;
    explicit VideoFrame(FrameMeta meta) : meta(meta) {}

    FrameMeta                    meta;
    std::shared_ptr<BufferOwner> owner;
    std::vector<PlaneView>       planes;
};

// 堆内存缓冲（host/mock 路径与单元测试使用；目标板零拷贝路径不使用）。
class HeapBuffer : public BufferOwner
{
   public:
    explicit HeapBuffer(std::vector<std::uint8_t> bytes) : bytes_(std::move(bytes)) {}

    const std::vector<std::uint8_t>& Bytes() const { return bytes_; }
    std::vector<std::uint8_t>&       Bytes() { return bytes_; }

   private:
    std::vector<std::uint8_t> bytes_;
};

using VideoFramePtr = std::shared_ptr<VideoFrame>;

}  // namespace animal_monitor
