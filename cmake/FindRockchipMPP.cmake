# FindRockchipMPP.cmake
#
# 查找 Rockchip MPP（Media Process Platform）开发包并创建
# 导入目标 RockchipMPP::rockchip_mpp。
#
# 输入提示变量：
#   RK_SDK_ROOT        — Rockchip BSP SDK 根目录（可选）
#   ROCKCHIP_MPP_ROOT  — MPP 安装根目录（可选，覆盖 SDK 推断）
#
# 输出变量：
#   ROCKCHIP_MPP_INCLUDE_DIR — 头文件目录（rk_mpi.h 所在）
#   ROCKCHIP_MPP_LIBRARY     — librockchip_mpp 库文件
#   ROCKCHIP_MPP_VERSION     — 版本（UNVERIFIED，需从 SDK 确认）
#   RockchipMPP_FOUND
#
# 注意：本 Find 模块在真实板卡 BSP 上尚未验证（UNVERIFIED）。

if(RockchipMPP_FOUND)
    return()
endif()

find_path(ROCKCHIP_MPP_INCLUDE_DIR
    NAMES rk_mpi.h
    HINTS
        ${ROCKCHIP_MPP_ROOT}/include
        ${RK_SDK_ROOT}/mpp/include
        ${RK_SDK_ROOT}/include
    PATH_SUFFIXES rockchip mpp
)

find_library(ROCKCHIP_MPP_LIBRARY
    NAMES rockchip_mpp
    HINTS
        ${ROCKCHIP_MPP_ROOT}/lib
        ${RK_SDK_ROOT}/mpp/lib
        ${RK_SDK_ROOT}/lib
)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(RockchipMPP
    REQUIRED_VARS ROCKCHIP_MPP_INCLUDE_DIR ROCKCHIP_MPP_LIBRARY
)

if(RockchipMPP_FOUND AND NOT TARGET RockchipMPP::rockchip_mpp)
    add_library(RockchipMPP::rockchip_mpp SHARED IMPORTED)
    set_target_properties(RockchipMPP::rockchip_mpp PROPERTIES
        IMPORTED_LOCATION ${ROCKCHIP_MPP_LIBRARY}
        INTERFACE_INCLUDE_DIRECTORIES ${ROCKCHIP_MPP_INCLUDE_DIR}
    )
endif()

mark_as_advanced(ROCKCHIP_MPP_INCLUDE_DIR ROCKCHIP_MPP_LIBRARY)
