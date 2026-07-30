# ============================================================
# Config 鈥?閰嶇疆绠＄悊
# JSON 閰嶇疆鏂囦欢鐨勫姞杞?淇濆瓨/璺緞瀵艰埅/鍘绘姈鍐欏叆
# ============================================================
set(CONFIG_SOURCES
    ConfigManager.cpp
    ProcessManager.cpp
)

set(CONFIG_HEADERS
    ConfigManager.h
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
