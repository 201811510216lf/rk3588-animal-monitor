# FindRknn.cmake
#
# 查找 RKNN Runtime（librknnrt），创建导入目标 Rknn::rknnrt。
# 提示变量：RK_SDK_ROOT / RKNN_ROOT。
# 状态：UNVERIFIED（未在真实 BSP 验证）。

if(Rknn_FOUND)
    return()
endif()

find_path(RKNN_INCLUDE_DIR
    NAMES rknn_api.h
    HINTS
        ${RKNN_ROOT}/include
        ${RK_SDK_ROOT}/rknn/include
        ${RK_SDK_ROOT}/include
)

find_library(RKNN_RT_LIBRARY
    NAMES rknnrt
    HINTS
        ${RKNN_ROOT}/lib
        ${RK_SDK_ROOT}/rknn/lib
        ${RK_SDK_ROOT}/lib
)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(Rknn
    REQUIRED_VARS RKNN_INCLUDE_DIR RKNN_RT_LIBRARY
)

if(Rknn_FOUND AND NOT TARGET Rknn::rknnrt)
    add_library(Rknn::rknnrt SHARED IMPORTED)
    set_target_properties(Rknn::rknnrt PROPERTIES
        IMPORTED_LOCATION ${RKNN_RT_LIBRARY}
        INTERFACE_INCLUDE_DIRECTORIES ${RKNN_INCLUDE_DIR}
    )
endif()

mark_as_advanced(RKNN_INCLUDE_DIR RKNN_RT_LIBRARY)
