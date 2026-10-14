# ============================================================
# HAL — 硬件抽象层
# 编译为静态库，包含所有硬件接口定义、仿真实现与品牌实现
# ============================================================
# ---------------------------------------------------------------------------
# 品牌实现开关：默认值按「第三方 SDK 是否就位」**自动探测**（2026-09-24，TR-117）
#
# 动机：这些 SDK 都是**闭源二进制且不入库**（`.lib` / `.dll` 有全局 gitignore 规则，
# `3rdparty/orbbec/` 另已整体排除）。于是「默认 ON」在新克隆上必然缺库、「默认 OFF」又让
# 开发机每次 CMakeCache 重建都**静默丢掉硬件支持**（TR-117：视觉页「相机型号」下拉框
# 只剩 `SimCamera`，全程零报错）。改为按存在性探测后：装了 SDK 的机器自动 `ON`（不必记开关），
# 新克隆自动 `OFF`（仍可正常编译），两种环境都不需要人工干预。
#
# ⚠ **边界**：`EXISTS` 在 **configure 期**求值，结果写进 `CMakeCache.txt` ⇒ 后来才装 SDK 时
#    必须**重新 configure**（删缓存，或显式 `-DUSE_XXX=ON`）；已配置过的目录不会自己翻转。
# ⚠ 探测结论一律**显式播报**（`message(STATUS)` / `FATAL_ERROR`）——TR-117 的教训就是「降级不吭声」。
# ---------------------------------------------------------------------------
set(_bopai_root  "${CMAKE_SOURCE_DIR}/3rdparty/bopai")
set(_orbbec_root "${CMAKE_SOURCE_DIR}/3rdparty/orbbec")

if(EXISTS "${_bopai_root}/lib/MultiCard.lib" AND EXISTS "${_bopai_root}/include")
    set(_bopai_default ON)
else()
    set(_bopai_default OFF)
endif()

if(EXISTS "${_orbbec_root}/lib/OrbbecSDK.lib" AND EXISTS "${_orbbec_root}/include")
    set(_orbbec_default ON)
else()
    set(_orbbec_default OFF)
endif()

option(USE_BOPAI "Build BoPai motion card support（默认按 SDK 存在性自动探测）" ${_bopai_default})
# XR 串口舵机**无第三方 SDK 依赖**（`XRServo.cpp` 只用 `<Windows.h>`，链接的是系统库 `setupapi`）
# ⇒ 没有可守卫的对象，保持默认 ON。
option(USE_XRSERVO "Build XR serial servo support" ON)
# Orbbec 3D 相机（Gemini 336）。OFF 时 `OrbbecCamera.cpp` 不参与编译，
# `simulation.cameraType` 回落 `SimCamera` 即可一键回到仿真（doc/camera_integration_plan.md §七 回退）。
option(USE_ORBBEC "Build Orbbec 3D camera support（默认按 SDK 存在性自动探测）" ${_orbbec_default})

# 自动探测为 OFF ⇒ 播报原因（否则「功能没了但没人知道」会重演）。
if(NOT _bopai_default)
    message(STATUS "HAL: 未找到 BoPai SDK（${_bopai_root}/lib/MultiCard.lib）→ USE_BOPAI 默认 OFF"
                   "（仿真可用；真机运动卡不支持）")
endif()
if(NOT _orbbec_default)
    message(STATUS "HAL: 未找到 Orbbec SDK（${_orbbec_root}/lib/OrbbecSDK.lib）→ USE_ORBBEC 默认 OFF"
                   "（simulation.cameraType 回落 SimCamera）")
endif()

# 显式要求 ON 而 SDK 缺失 ⇒ 立刻在 **configure 期**给出可读错误，
# 而不是拖到链接期报一句「无法打开 MultiCard.lib / OrbbecSDK.lib」。
if(USE_BOPAI AND NOT _bopai_default)
    message(FATAL_ERROR
        "USE_BOPAI=ON 但未找到 BoPai SDK：\n"
        "  需要 ${_bopai_root}/lib/MultiCard.lib 与 ${_bopai_root}/include/\n"
        "  请把 SDK 放到 3rdparty/bopai/，或改用 -DUSE_BOPAI=OFF 构建仿真版。")
endif()
if(USE_ORBBEC AND NOT _orbbec_default)
    message(FATAL_ERROR
        "USE_ORBBEC=ON 但未找到 Orbbec SDK：\n"
        "  需要 ${_orbbec_root}/lib/OrbbecSDK.lib 与 ${_orbbec_root}/include/\n"
        "  请把 SDK 放到 3rdparty/orbbec/，或改用 -DUSE_ORBBEC=OFF 构建仿真版。")
endif()

set(HAL_SOURCES
    motioncard/SimCard.cpp
    servo/SimServo.cpp
    camera/SimCamera.cpp
    algorithm/SimAlgo.cpp
    algorithm/DepthPuffAlgo.cpp
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
    algorithm/DepthPuffAlgo.h
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

# OpenCV（2026-10-14，深度法算法移植）。
#
# `DepthPuffAlgo` 需要 `cv::connectedComponentsWithStats` / `morphologyEx` /
# `getStructuringElement`。⚠ 此前 HAL **不链接 OpenCV**（只有 Core 与主程序链），
# 是本仓一条隐性边界——移植前必须补上，否则链接期报未解析符号。
# 运行时 DLL 已由根 `CMakeLists.txt` 的 POST_BUILD 拷进 exe 目录，
# `build_release.bat` 的 `copy *.dll` 会带走 ⇒ **部署无新增动作**。
target_link_libraries(HAL PRIVATE ${OpenCV_LIBS})

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
