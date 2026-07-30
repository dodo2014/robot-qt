# ============================================================
# HAL — 硬件抽象层
# 编译为静态库，包含所有硬件接口定义和仿真实现
# ============================================================
set(HAL_SOURCES
    SimCard.cpp
)

set(HAL_HEADERS
    IMotionCard.h
    SimCard.h
    IAxisServo.h
    IEndEffector.h
    ICamera.h
    IPuffAlgorithm.h
    HALFactory.h
)

add_library(HAL STATIC
    ${HAL_SOURCES}
    ${HAL_HEADERS}
)

target_include_directories(HAL PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(HAL PUBLIC
    spdlog::spdlog
)

# 在仿真模式下传递宏给 HAL 自身
if(SIMULATION_MODE)
    target_compile_definitions(HAL PRIVATE SIM_MODE)
endif()
