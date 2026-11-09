# FindRockchipRGA.cmake
#
# 查找 Rockchip RGA（im2d）开发包，创建导入目标 RockchipRGA::librga。
# 提示变量：RK_SDK_ROOT / ROCKCHIP_RGA_ROOT。
# 状态：UNVERIFIED（未在真实 BSP 验证）。

if(RockchipRGA_FOUND)
    return()
endif()

find_path(ROCKCHIP_RGA_INCLUDE_DIR
    NAMES im2d.h
    HINTS
        ${ROCKCHIP_RGA_ROOT}/include
        ${RK_SDK_ROOT}/rga/include
        ${RK_SDK_ROOT}/include/rga
    PATH_SUFFIXES rga
)

find_library(ROCKCHIP_RGA_LIBRARY
    NAMES rga
    HINTS
        ${ROCKCHIP_RGA_ROOT}/lib
        ${RK_SDK_ROOT}/rga/lib
        ${RK_SDK_ROOT}/lib
)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(RockchipRGA
    REQUIRED_VARS ROCKCHIP_RGA_INCLUDE_DIR ROCKCHIP_RGA_LIBRARY
)

if(RockchipRGA_FOUND AND NOT TARGET RockchipRGA::librga)
    add_library(RockchipRGA::librga SHARED IMPORTED)
    set_target_properties(RockchipRGA::librga PROPERTIES
        IMPORTED_LOCATION ${ROCKCHIP_RGA_LIBRARY}
        INTERFACE_INCLUDE_DIRECTORIES ${ROCKCHIP_RGA_INCLUDE_DIR}
    )
endif()

mark_as_advanced(ROCKCHIP_RGA_INCLUDE_DIR ROCKCHIP_RGA_LIBRARY)
