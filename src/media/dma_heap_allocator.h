// 文件名：src/media/dma_heap_allocator.h
// 状态：implemented（模板）/ hardware-blocked —— 未经目标板编译与实测（UNVERIFIED）
// 来源：按 docs/migration/source_audit.md §4.3 审计结论重写；
//       API 形状参考 .ref_audit_RKMediaProject 的 dma-heap 用法与
//       多路视频/demo_multhread_decode_infer_mulmodel/include/dma_alloc.hpp（只读审计），未逐行拷贝。
// 说明：仅供 src/camera 与 src/media 内部实现共用的私有头，不对外发布；
//       dma-heap 分配、DMA_BUF_IOCTL_SYNC 与“带 fd 的帧所有权”在此统一封装。
#pragma once

#include <fcntl.h>
#include <linux/dma-buf.h>
#include <linux/dma-heap.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>

#include "animal_monitor/common/status.h"
#include "animal_monitor/logging/logger.h"
#include "animal_monitor/media/video_frame.h"

namespace animal_monitor {

// 单块 dma-heap 缓冲的 RAII 封装：持有 dmabuf fd 与用户态 mmap 映射。
// 析构自动 munmap + close；只可移动、不可复制（所有权唯一，禁止裸 owning 指针）。
// 注意：同一 fd 在 DmaBuffer 之外不得再被 close；需要转交时使用移动语义。
class DmaBuffer
{
  public:
    DmaBuffer() = default;
    ~DmaBuffer();

    DmaBuffer(const DmaBuffer&)            = delete;
    DmaBuffer& operator=(const DmaBuffer&) = delete;
    DmaBuffer(DmaBuffer&& other) noexcept;
    DmaBuffer& operator=(DmaBuffer&& other) noexcept;

    // 提前显式释放（munmap + close），幂等；析构会再次调用。
    void Reset();

    bool            Valid() const { return fd_ >= 0 && va_ != nullptr && size_ > 0; }
    int             Fd() const { return fd_; }
    void*           Address() const { return va_; }
    std::uint8_t*   ByteAddress() const { return static_cast<std::uint8_t*>(va_); }
    std::size_t     Size() const { return size_; }

  private:
    friend class DmaHeapAllocator;

    int         fd_   = -1;
    void*       va_   = nullptr;
    std::size_t size_ = 0;
};

// dma-heap 分配器：路径可配置（默认 /dev/dma_heap/system，与 V4L2/RGA 共用）。
// 每次 Allocate：open(heap) → DMA_HEAP_IOCTL_ALLOC → close(heap) → mmap。
// 设计依据：source_audit.md §4.3 —— heap fd 用完即关、不跨调用复用；
//           heap 路径参数化（AGENTS：避免硬编码设备路径，默认值仅作板级配置兜底）。
class DmaHeapAllocator
{
  public:
    static constexpr const char* kDefaultHeapPath = "/dev/dma_heap/system";

    explicit DmaHeapAllocator(std::string heap_path = kDefaultHeapPath)
        : heap_path_(std::move(heap_path))
    {
    }

    // 分配 size 字节（失败返回 Status，绝不返回裸 -1）。
    Status Allocate(std::size_t size, DmaBuffer* out);

    const std::string& HeapPath() const { return heap_path_; }

  private:
    std::string heap_path_;
};

// dma-buf CPU 同步（DMA_BUF_IOCTL_SYNC）。
// 采集流程：DQBUF 后 SyncStartRead 开始 CPU 读窗口，交给下游前（或归还 QBUF 前）SyncEndRead 结束。
// system heap 为一致性内存时该 ioctl 近似空操作，但保留调用以兼容非一致性 heap（UNVERIFIED）。
Status DmaSyncStartRead(int fd);
Status DmaSyncEndRead(int fd);

// 带 fd 的帧所有权句柄：RGA importbuffer_fd / 零拷贝路径通过它取底层 dmabuf fd。
// VideoFrame.owner 可 dynamic_cast 到本类型：成功则优先 fd 零拷贝，失败回退虚拟地址。
class FdBufferOwner : public BufferOwner
{
  public:
    virtual int          Fd() const   = 0;
    virtual std::size_t  Size() const = 0;
};

// dma-heap 帧所有权：独占持有 DmaBuffer，析构即释放（RGA 输出、合成画布等场景）。
class DmaHeapFrameOwner : public FdBufferOwner
{
  public:
    DmaHeapFrameOwner() = default;
    explicit DmaHeapFrameOwner(DmaBuffer buffer) : buffer_(std::move(buffer))
    {
    }

    int          Fd() const override { return buffer_.Fd(); }
    std::size_t  Size() const override { return buffer_.Size(); }
    void*        Address() const { return buffer_.Address(); }

  private:
    DmaBuffer buffer_;
};

// ---------------------------------------------------------------------------
// 内联实现（本头为 v4l2_camera/rga_processor 共用的私有实现头，无独立 .cpp）。
// ---------------------------------------------------------------------------

inline DmaBuffer::~DmaBuffer()
{
    Reset();
}

inline DmaBuffer::DmaBuffer(DmaBuffer&& other) noexcept
    : fd_(other.fd_), va_(other.va_), size_(other.size_)
{
    other.fd_   = -1;
    other.va_   = nullptr;
    other.size_ = 0;
}

inline DmaBuffer& DmaBuffer::operator=(DmaBuffer&& other) noexcept
{
    if (this != &other)
    {
        Reset();
        fd_       = other.fd_;
        va_       = other.va_;
        size_     = other.size_;
        other.fd_ = -1;
        other.va_ = nullptr;
        other.size_ = 0;
    }
    return *this;
}

inline void DmaBuffer::Reset()
{
    if (va_ != nullptr && size_ > 0)
    {
        if (::munmap(va_, size_) != 0)
        {
            AM_LOG_WARNING() << "[dma-heap] munmap fd=" << fd_ << " 失败: "
                             << std::strerror(errno);
        }
        va_ = nullptr;
    }
    if (fd_ >= 0)
    {
        if (::close(fd_) != 0)
        {
            AM_LOG_WARNING() << "[dma-heap] close fd=" << fd_ << " 失败: "
                             << std::strerror(errno);
        }
        fd_ = -1;
    }
    size_ = 0;
}

inline Status DmaHeapAllocator::Allocate(std::size_t size, DmaBuffer* out)
{
    if (size == 0 || out == nullptr)
    {
        return Status(StatusCode::kInvalidArgument,
                      "[dma-heap] 无效分配参数（size=0 或 out 为空）");
    }

    out->Reset();

    const int heap_fd = ::open(heap_path_.c_str(), O_RDWR | O_CLOEXEC);
    if (heap_fd < 0)
    {
        return Status(StatusCode::kHardwareUnavailable,
                      std::string("[dma-heap] 打开 ") + heap_path_ + " 失败: " +
                          std::strerror(errno));
    }

    // 注意：dma_heap_allocation_data 字段布局随内核头版本略有差异（3 字段/4 字段），
    // 使用系统 <linux/dma-heap.h> 的定义并由构建 sysroot 决定（UNVERIFIED）。
    dma_heap_allocation_data request{};
    request.len      = static_cast<__u64>(size);
    request.fd_flags = O_RDWR | O_CLOEXEC;

    if (::ioctl(heap_fd, DMA_HEAP_IOCTL_ALLOC, &request) < 0)
    {
        const std::string message = std::string("[dma-heap] 分配 ") +
                                    std::to_string(size) + " 字节失败: " +
                                    std::strerror(errno);
        ::close(heap_fd);
        return Status(StatusCode::kResourceExhausted, message);
    }
    ::close(heap_fd);

    void* mapped = ::mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, request.fd, 0);
    if (mapped == MAP_FAILED)
    {
        const std::string message = std::string("[dma-heap] mmap fd=") +
                                    std::to_string(request.fd) + " 失败: " +
                                    std::strerror(errno);
        ::close(request.fd);
        return Status(StatusCode::kInternal, message);
    }

    out->fd_   = static_cast<int>(request.fd);
    out->va_   = mapped;
    out->size_ = size;
    return Status::MakeOk();
}

inline Status DmaSyncStartRead(int fd)
{
    if (fd < 0)
    {
        return Status(StatusCode::kInvalidArgument, "[dma-buf] 无效 fd");
    }
    dma_buf_sync sync{};
    sync.flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ;
    if (::ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync) < 0)
    {
        return Status(StatusCode::kInternal,
                      std::string("[dma-buf] SYNC_START(READ) fd=") + std::to_string(fd) +
                          " 失败: " + std::strerror(errno));
    }
    return Status::MakeOk();
}

inline Status DmaSyncEndRead(int fd)
{
    if (fd < 0)
    {
        return Status(StatusCode::kInvalidArgument, "[dma-buf] 无效 fd");
    }
    dma_buf_sync sync{};
    sync.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ;
    if (::ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync) < 0)
    {
        return Status(StatusCode::kInternal,
                      std::string("[dma-buf] SYNC_END(READ) fd=") + std::to_string(fd) +
                          " 失败: " + std::strerror(errno));
    }
    return Status::MakeOk();
}

}  // namespace animal_monitor
