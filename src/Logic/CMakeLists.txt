# ============================================================
# Logic — 业务逻辑层
# 状态机 (PickCycle)、示教系统、流程编排
# ============================================================
set(LOGIC_SOURCES
    PickCycleController.cpp
    SequenceWorker.cpp
)

set(LOGIC_HEADERS
    PickCycleController.h
    SequenceWorker.h
)

add_library(Logic STATIC
    ${LOGIC_SOURCES}
    ${LOGIC_HEADERS}
)

target_include_directories(Logic PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(Logic PUBLIC
    Core
    HAL
    spdlog::spdlog
)

if(SIMULATION_MODE)
    target_compile_definitions(Logic PRIVATE SIM_MODE)
endif()
