# tests/CMakeLists.txt — 独立验证工具（不参与主程序依赖链，ninja CreamPuffRobot 不会编译它们）

# 运动学 FK/IK 验证：内置真机摆位回归用例 + fk/ik 手动查询模式
add_executable(test_kinematics_check test_kinematics_check.cpp)
target_link_libraries(test_kinematics_check PRIVATE
    Core
    spdlog::spdlog
    nlohmann_json::nlohmann_json
)
target_compile_definitions(test_kinematics_check PRIVATE
    PROJECT_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)
