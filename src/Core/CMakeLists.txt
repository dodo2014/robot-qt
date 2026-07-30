# ============================================================
# Core — 核心算法层
# 运动学 (Eigen3)、轨迹插补、坐标转换 (OpenCV)
# ============================================================
set(CORE_SOURCES
    Kinematics.cpp
    CoordTransform.cpp
    Trajectory.cpp
)

set(CORE_HEADERS
    Kinematics.h
    CoordTransform.h
    Trajectory.h
)

add_library(Core STATIC
    ${CORE_SOURCES}
    ${CORE_HEADERS}
)

target_include_directories(Core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(Core PUBLIC
    Eigen3::Eigen
    ${OpenCV_LIBS}
    spdlog::spdlog
    HAL
)

if(SIMULATION_MODE)
    target_compile_definitions(Core PRIVATE SIM_MODE)
endif()
