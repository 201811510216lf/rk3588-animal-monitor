# FindZlm.cmake
#
# 查找 ZLMediaKit C API（libmk_api.so + libZLToolKit.so），创建导入目标
# Zlm::mk_api 与 Zlm::ZLToolKit。
# 提示变量：RK_SDK_ROOT / ZLM_ROOT / ZLM_INCLUDE_DIR / ZLM_LIB_DIR。
# 状态：UNVERIFIED（未在真实 BSP 验证）。

if(Zlm_FOUND)
    return()
endif()

find_path(ZLM_INCLUDE_DIR
    NAMES mk_media.h
    HINTS
        ${ZLM_ROOT}/include
        ${RK_SDK_ROOT}/zlmediakit/include
        ${RK_SDK_ROOT}/include/zlmediakit
)

find_library(ZLM_MK_API_LIBRARY
    NAMES mk_api
    HINTS
        ${ZLM_ROOT}/lib
        ${RK_SDK_ROOT}/zlmediakit/lib
        ${RK_SDK_ROOT}/lib
)

find_library(ZLM_TOOLKIT_LIBRARY
    NAMES ZLToolKit
    HINTS
        ${ZLM_ROOT}/lib
        ${RK_SDK_ROOT}/zlmediakit/lib
        ${RK_SDK_ROOT}/lib
)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(Zlm
    REQUIRED_VARS ZLM_INCLUDE_DIR ZLM_MK_API_LIBRARY ZLM_TOOLKIT_LIBRARY
)

if(Zlm_FOUND AND NOT TARGET Zlm::mk_api)
    add_library(Zlm::mk_api SHARED IMPORTED)
    set_target_properties(Zlm::mk_api PROPERTIES
        IMPORTED_LOCATION ${ZLM_MK_API_LIBRARY}
        INTERFACE_INCLUDE_DIRECTORIES ${ZLM_INCLUDE_DIR}
    )
    add_library(Zlm::ZLToolKit SHARED IMPORTED)
    set_target_properties(Zlm::ZLToolKit PROPERTIES
        IMPORTED_LOCATION ${ZLM_TOOLKIT_LIBRARY}
    )
endif()

mark_as_advanced(ZLM_INCLUDE_DIR ZLM_MK_API_LIBRARY ZLM_TOOLKIT_LIBRARY)
