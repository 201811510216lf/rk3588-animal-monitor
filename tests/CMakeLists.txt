# 单元测试：host 构建，不依赖 Rockchip SDK。
# 每个测试可执行文件自带 main()（见 test_harness.h），注册到 CTest。

add_executable(test_bounded_queue unit/test_bounded_queue.cpp)
target_link_libraries(test_bounded_queue PRIVATE animal_monitor_core)
add_test(NAME test_bounded_queue COMMAND test_bounded_queue)

add_executable(test_timestamp_aligner unit/test_timestamp_aligner.cpp)
target_link_libraries(test_timestamp_aligner PRIVATE animal_monitor_core)
add_test(NAME test_timestamp_aligner COMMAND test_timestamp_aligner)

add_executable(test_modbus_crc unit/test_modbus_crc.cpp)
target_link_libraries(test_modbus_crc PRIVATE animal_monitor_core)
add_test(NAME test_modbus_crc COMMAND test_modbus_crc)

add_executable(test_config unit/test_config.cpp)
target_link_libraries(test_config PRIVATE animal_monitor_core)
add_test(NAME test_config COMMAND test_config)

add_executable(test_metrics unit/test_metrics.cpp)
target_link_libraries(test_metrics PRIVATE animal_monitor_core)
add_test(NAME test_metrics COMMAND test_metrics)
