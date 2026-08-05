# ============================================================
# HAL — 硬件抽象层
# 编译为静态库，包含所有硬件接口定义、仿真实现与品牌实现
# ============================================================
option(USE_BOPAI "Build BoPai motion card support" ON)
option(USE_XRSERVO "Build XR serial servo support" ON)

set(HAL_SOURCES
    SimCard.cpp
    SimServo.cpp
    AxisConverter.cpp
    HardwareManager.cpp
)

set(HAL_HEADERS
    IMotionCard.h
    SimCard.h
    SimServo.h
    IAxisServo.h
    IEndEffector.h
    ICamera.h
    IPuffAlgorithm.h
    HALFactory.h
    AxisConverter.h
    AxisMap.h
    HardwareManager.h
)

if(USE_BOPAI)
    list(APPEND HAL_SOURCES BoPaiCard.cpp)
    list(APPEND HAL_HEADERS BoPaiCard.h)
endif()

if(USE_XRSERVO)
    list(APPEND HAL_SOURCES XRServo.cpp)
    list(APPEND HAL_HEADERS XRServo.h)
endif()

add_library(HAL STATIC
    ${HAL_SOURCES}
    ${HAL_HEADERS}
)

target_include_directories(HAL PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src
)

target_include_directories(HAL PRIVATE
    ${CMAKE_SOURCE_DIR}/3rdparty/bopai/include
)

target_link_libraries(HAL PUBLIC
    Qt6::Core
    Config
    spdlog::spdlog
)

if(USE_BOPAI)
    target_compile_definitions(HAL PRIVATE USE_BOPAI)
    target_link_libraries(HAL PRIVATE ${CMAKE_SOURCE_DIR}/3rdparty/bopai/lib/MultiCard.lib)
endif()

if(USE_XRSERVO)
    target_compile_definitions(HAL PRIVATE USE_XRSERVO)
    target_link_libraries(HAL PRIVATE setupapi)
endif()

# 在仿真模式下传递宏给 HAL 自身
if(SIMULATION_MODE)
    target_compile_definitions(HAL PRIVATE SIM_MODE)
endif()

target_compile_definitions(HAL PRIVATE
    PROJECT_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)
