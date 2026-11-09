// 文件名：include/animal_monitor/media/rga_processor.h
// 状态：implemented（模板）/ hardware-blocked —— 未经目标板编译与实测（UNVERIFIED）
// 来源：按 docs/migration/source_audit.md 与 _audit_reports/audit_ref_RKMediaProject.md
// 审计结论重写；
//       API 形状参考 .ref_audit_RKMediaProject（RgaInstance），未逐行拷贝。
// 修复的参考实现缺陷：
//  - stride 显式传递（meta.stride → RGA wstride/hstride 像素换算），禁止忽略 stride；
//  - 输出 BufferOwner 持 dma-heap fd（RAII 释放），不再依赖外部池与裸指针归还；
//  - 输入优先 importbuffer_fd + wrapbuffer_handle 零拷贝，失败回退虚拟地址；
//  - im2d 调用返回值全部检查（imStrError_t 上报）。
// 说明：本头文件不引入任何 RGA 类型（pimpl 隔离）；仅当
//       ANIMAL_MONITOR_ENABLE_TARGET_DEPS=ON 时参与构建。
#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "animal_monitor/common/status.h"
#include "animal_monitor/media/video_frame.h"

namespace animal_monitor
{

// RGA 配置（dma-heap 路径参数化；其余能力由 imcheckHeader/querystring 探测）。
struct RgaProcessorConfig
{
    std::string dma_heap_path = "/dev/dma_heap/system";
};

// RGA im2d 封装：颜色转换 + 缩放 + 双画面左右拼接。
// 线程安全：RGA 设备为全局共享资源，本类方法需外部串行化（管线中由 RGA 线程独占）。
// 输出帧 owner 为 DmaHeapFrameOwner（dma-heap fd 常驻，编码器可后续 import）。
class RgaProcessor
{
   public:
    explicit RgaProcessor(RgaProcessorConfig config = {});
    ~RgaProcessor();

    RgaProcessor(const RgaProcessor&)            = delete;
    RgaProcessor& operator=(const RgaProcessor&) = delete;

    // 探测 librga 运行时版本匹配（imcheckHeader）并打印能力（querystring）。
    Status Initialize();

    // 颜色转换 + 缩放（同格式时单次 imresize；跨格式时 imcvtcolor + imresize 两步）。
    // 输出 meta.stride 按目标格式 16 字节对齐；支持 NV12/NV21/I420/BGR888/RGB888。
    Status CvtColorResize(const VideoFrame& in, std::uint32_t width, std::uint32_t height,
                          PixelFormat dst_format, VideoFramePtr* out);

    // 双画面左右拼接：两路输入缩放到各占一半宽度，输出 NV12 单画布
    // （画布 = 左半 + 右半，虚拟地址分区域 wrap，单次分配、零中间缓冲）。
    // 输入必须同为 NV12（不同格式先 CvtColorResize 归一化）。
    Status CompositeSideBySide(const VideoFrame& left, const VideoFrame& right, VideoFramePtr* out);

   private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace animal_monitor
