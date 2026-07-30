cmake_minimum_required(VERSION 3.20)

# ============================================================
# Vcpkg 工具链绑定
# 通过内置的 Git Submodule 方式管理 vcpkg
# 首次使用执行: git submodule add https://github.com/microsoft/vcpkg.git vcpkg
# ============================================================
set(CMAKE_TOOLCHAIN_FILE
    "${CMAKE_CURRENT_SOURCE_DIR}/vcpkg/scripts/buildsystems/vcpkg.cmake"
    CACHE STRING "Vcpkg toolchain file")

project(CreamPuffRobot
    VERSION 1.0.0
    DESCRIPTION "Cream Puff Picking SCARA Robot Control System"
    LANGUAGES CXX)

# ============================================================
# 全局 C++ 标准 — C++17, 关闭编译器扩展
# ============================================================
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Qt 相关
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

# ============================================================
# 全局编译选项：仿真模式开关
# ============================================================
option(SIMULATION_MODE "Enable simulation mode (use SimCard instead of real hardware)" ON)
if(SIMULATION_MODE)
    add_compile_definitions(SIM_MODE)
    message(STATUS "[CreamPuffRobot] SIMULATION_MODE = ON — using simulated hardware")
else()
    message(STATUS "[CreamPuffRobot] SIMULATION_MODE = OFF — using real hardware")
endif()

# ============================================================
# 平台特定编译选项
# ============================================================
if(WIN32)
    # 强制 MSVC 输入/输出编码为 UTF-8
    add_compile_options("$<$<CXX_COMPILER_ID:MSVC>:/utf-8>")
    set(CMAKE_MSVC_ENCODING "utf-8" CACHE STRING "MSVC source/output encoding")
    # 启用 MSVC 数学常量 (M_PI, M_PI_2 等)
    add_compile_definitions("$<$<CXX_COMPILER_ID:MSVC>:_USE_MATH_DEFINES>")
    # 最高警告级别
    add_compile_options("$<$<CXX_COMPILER_ID:MSVC>:/W4>")
    # 将警告视为错误 (生产环境建议开启)
    # add_compile_options("$<$<CXX_COMPILER_ID:MSVC>:/WX>")
    # 检查是否为 64 位
    if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
        message(FATAL_ERROR "[CreamPuffRobot] Only 64-bit builds are supported. Please use x64 configuration.")
    endif()
    message(STATUS "[CreamPuffRobot] Platform: Windows x64 — UTF-8 /W4 enabled")
endif()

# ============================================================
# 查找第三方库
# ============================================================
# Qt6
find_package(Qt6 REQUIRED COMPONENTS Core Widgets)
message(STATUS "[CreamPuffRobot] Qt6 found: ${Qt6_VERSION}")

# Eigen3 — 矩阵运算
find_package(Eigen3 REQUIRED)
if(Eigen3_FOUND)
    add_compile_definitions(HAS_EIGEN)
    message(STATUS "[CreamPuffRobot] Eigen3 found: ${Eigen3_VERSION}")
endif()

# nlohmann_json — JSON 解析
find_package(nlohmann_json REQUIRED)
message(STATUS "[CreamPuffRobot] nlohmann_json found")

# OpenCV — 视觉
find_package(OpenCV REQUIRED)
message(STATUS "[CreamPuffRobot] OpenCV found: ${OpenCV_VERSION}")

# spdlog — 日志
find_package(spdlog REQUIRED)
message(STATUS "[CreamPuffRobot] spdlog found")

# ============================================================
# 子模块
# ============================================================
add_subdirectory(src/HAL)
add_subdirectory(src/Core)
add_subdirectory(src/Logic)
add_subdirectory(src/Config)
add_subdirectory(src/UI)

# ============================================================
# 主可执行文件
# ============================================================
set(MAIN_SOURCES
    src/main.cpp
)

add_executable(${PROJECT_NAME}
    ${MAIN_SOURCES}
)

target_link_libraries(${PROJECT_NAME} PRIVATE
    HAL
    Core
    Logic
    UI
    Qt6::Core
    Qt6::Widgets
    Eigen3::Eigen
    nlohmann_json::nlohmann_json
    ${OpenCV_LIBS}
    spdlog::spdlog
)

target_compile_definitions(${PROJECT_NAME} PRIVATE
    PROJECT_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)

# ============================================================
# 构建后事件：自动复制闭源 SDK DLL 到输出目录
# 源目录: 3rdparty/BoPai_Motion/bin/*.dll
# 目标: ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/${CMAKE_BUILD_TYPE}/
# ============================================================
set(BOPAI_DLL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/BoPai_Motion/bin")
file(GLOB BOPAI_DLLS "${BOPAI_DLL_DIR}/*.dll")
if(BOPAI_DLLS)
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${BOPAI_DLLS}
            $<TARGET_FILE_DIR:${PROJECT_NAME}>
        COMMENT "[POST_BUILD] Copying BoPai Motion SDK DLLs to output directory..."
    )
else()
    message(WARNING "[POST_BUILD] No DLLs found in ${BOPAI_DLL_DIR} — skipping DLL copy")
endif()

# Qt6 DLL 部署：使用 windeployqt 自动复制运行所需的所有 Qt DLL
set(QT6_BIN_DIR "D:/Qt/6.11.1/msvc2022_64/bin")
if(EXISTS "${QT6_BIN_DIR}/windeployqt.exe")
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND "${QT6_BIN_DIR}/windeployqt.exe"
            --no-compiler-runtime --no-system-d3d-compiler --no-opengl-sw
            "$<TARGET_FILE_DIR:${PROJECT_NAME}>"
        COMMENT "[POST_BUILD] Deploying Qt6 DLLs via windeployqt..."
    )
endif()

# 复制配置文件
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/config/config.json"
        "$<TARGET_FILE_DIR:${PROJECT_NAME}>/config.json"
    COMMENT "[POST_BUILD] Copying config.json to output directory..."
)
