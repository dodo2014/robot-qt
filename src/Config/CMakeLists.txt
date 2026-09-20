# ============================================================
# Config — 配置管理
# JSON 配置文件的加载/保存/路径导航/去抖写入
# ============================================================
set(CONFIG_SOURCES
    ConfigManager.cpp
    ConfigPaths.cpp
    ProcessManager.cpp
)

set(CONFIG_HEADERS
    ConfigManager.h
    ConfigPaths.h
    ProcessManager.h
)

add_library(Config STATIC
    ${CONFIG_SOURCES}
    ${CONFIG_HEADERS}
)

target_include_directories(Config PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(Config PUBLIC
    Qt6::Core
    nlohmann_json::nlohmann_json
    spdlog::spdlog
)

target_compile_definitions(Config PRIVATE
    PROJECT_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)
