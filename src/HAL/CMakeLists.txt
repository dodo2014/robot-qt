# ============================================================
# HAL — 硬件抽象层
# 编译为静态库，包含所有硬件接口定义、仿真实现与品牌实现
# ============================================================
option(USE_BOPAI "Build BoPai motion card support" ON)
option(USE_XRSERVO "Build XR serial servo support" ON)
# Orbbec 3D 相机（Gemini 336）。**默认 OFF**：SDK 为闭源二进制，且仅真机环境需要；
# OFF 时 `OrbbecCamera.cpp` 不参与编译，`simulation.cameraType` 回落 `SimCamera`
# 即可一键回到仿真（doc/camera_integration_plan.md §七 回退）。
option(USE_ORBBEC "Build Orbbec 3D camera support" OFF)

set(HAL_SOURCES
    motioncard/SimCard.cpp
    servo/SimServo.cpp
    camera/SimCamera.cpp
    algorithm/SimAlgo.cpp
    camera/CameraManager.cpp
    camera/CameraCaptureWorker.cpp
    camera/FrameSaver.cpp
    camera/FrameConverter.cpp
    core/AxisConverter.cpp
    core/AxisConfigService.cpp
    core/HardwareManager.cpp
)

set(HAL_HEADERS
    interfaces/IMotionCard.h
    interfaces/IAxisServo.h
    interfaces/IEndEffector.h
    interfaces/ICamera.h
    interfaces/IPuffAlgorithm.h
    motioncard/SimCard.h
    servo/SimServo.h
    camera/SimCamera.h
    algorithm/SimAlgo.h
    camera/SimVision.h
    camera/CameraManager.h
    camera/CameraCaptureWorker.h
    camera/FrameSaver.h
    camera/FrameConverter.h
    core/HALFactory.h
    core/AxisConverter.h
    core/AxisConfigService.h
    core/AxisMap.h
    core/HardwareManager.h
)

if(USE_BOPAI)
    list(APPEND HAL_SOURCES motioncard/BoPaiCard.cpp)
    list(APPEND HAL_HEADERS motioncard/BoPaiCard.h)
endif()

if(USE_XRSERVO)
    list(APPEND HAL_SOURCES servo/XRServo.cpp)
    list(APPEND HAL_HEADERS servo/XRServo.h)
endif()

if(USE_ORBBEC)
    list(APPEND HAL_SOURCES camera/OrbbecCamera.cpp)
    list(APPEND HAL_HEADERS camera/OrbbecCamera.h)
endif()

add_library(HAL STATIC
    ${HAL_SOURCES}
    ${HAL_HEADERS}
)

target_include_directories(HAL PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/interfaces
    ${CMAKE_CURRENT_SOURCE_DIR}/core
    ${CMAKE_CURRENT_SOURCE_DIR}/motioncard
    ${CMAKE_CURRENT_SOURCE_DIR}/servo
    ${CMAKE_CURRENT_SOURCE_DIR}/camera
    ${CMAKE_CURRENT_SOURCE_DIR}/algorithm
    ${CMAKE_SOURCE_DIR}/src
)

target_include_directories(HAL PRIVATE
    ${CMAKE_SOURCE_DIR}/3rdparty/bopai/include
)

target_link_libraries(HAL PUBLIC
    Qt6::Core
    Qt6::Gui
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

if(USE_ORBBEC)
    # SDK 头以 <libobsensor/...> 形式包含，故 include 根是 3rdparty/orbbec/include
    target_include_directories(HAL PRIVATE ${CMAKE_SOURCE_DIR}/3rdparty/orbbec/include)
    target_compile_definitions(HAL PRIVATE USE_ORBBEC)
    target_link_libraries(HAL PRIVATE ${CMAKE_SOURCE_DIR}/3rdparty/orbbec/lib/OrbbecSDK.lib)
endif()

# 在仿真模式下传递宏给 HAL 自身
if(SIMULATION_MODE)
    target_compile_definitions(HAL PRIVATE SIM_MODE)
endif()

target_compile_definitions(HAL PRIVATE
    PROJECT_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)
