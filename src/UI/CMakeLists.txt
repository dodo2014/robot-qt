# ============================================================
# UI 鈥?鐣岄潰灞?(Qt6)
# HMI 浠〃鐩樸€佹墜鍔ㄧず鏁欍€佸伐鑹虹紪杈戙€佽澶囬厤缃?# ============================================================
set(UI_SOURCES
    MainWindow.cpp
    AutoRunPage.cpp
    ManualControlPage.cpp
    ProcessPage.cpp
    VisionTestPage.cpp
    ConfigPage.cpp
    ToggleSwitch.cpp
)

set(UI_HEADERS
    MainWindow.h
    AutoRunPage.h
    ManualControlPage.h
    ProcessPage.h
    VisionTestPage.h
    ConfigPage.h
    ToggleSwitch.h
    KinematicsHelper.h
)

add_library(UI STATIC
    ${UI_SOURCES}
    ${UI_HEADERS}
)

target_include_directories(UI PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(UI PUBLIC
    Qt6::Core
    Qt6::Widgets
    Config
    Logic
    Core
    HAL
    nlohmann_json::nlohmann_json
    spdlog::spdlog
)

if(SIMULATION_MODE)
    target_compile_definitions(UI PRIVATE SIM_MODE)
endif()

target_compile_definitions(UI PRIVATE
    PROJECT_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)
