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

# P6 节点树数据模型自测（离线）：NodeType/children 树形、`actions` → `nodes` 读取兼容与
# 自动升级、编号规则（§7.2）、组内校验（§6.2 / §6.4 / §Q5）。
# ⚠ 往返用例会改写 `config/process.json` 后恢复（RAII），**不要在真机调试过程中运行**。
add_executable(test_node_tree_check test_node_tree_check.cpp)
target_link_libraries(test_node_tree_check PRIVATE
    Config
    spdlog::spdlog
)
target_compile_definitions(test_node_tree_check PRIVATE
    PROJECT_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)
# 输出到构建根目录（与 CreamPuffRobot.exe 同级）：本目标链接 Config，用到 `QString`，
# 需要 windeployqt 部署在构建根目录的 Qt DLL；落在 `tests/` 子目录会启动即失败。
set_target_properties(test_node_tree_check PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)

# 相机真机验证（P3 / S2–S4）：枚举设备 + 开流取帧 + 帧格式校验。
# 依赖 HAL 拿到 OrbbecCamera；HAL 是静态库且注册靠 `/WHOLEARCHIVE`（见根 CMakeLists），
# 此处同样用 `$<LINK_LIBRARY:WHOLE_ARCHIVE,HAL>`，否则 OrbbecCamera.cpp.obj 会被丢弃。
# ⚠ `USE_ORBBEC=OFF` 时 OrbbecCamera 不在 HAL 里 → 本目标会链接失败，
#    故用 if(USE_ORBBEC) 保护（与 HAL 的开关保持一致）。
if(USE_ORBBEC)
    add_executable(test_camera_check test_camera_check.cpp)
    target_link_libraries(test_camera_check PRIVATE
        $<LINK_LIBRARY:WHOLE_ARCHIVE,HAL>
        Core
        Config
        spdlog::spdlog
    )
    target_compile_definitions(test_camera_check PRIVATE
        PROJECT_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
    )
    # 输出到构建根目录（与 CreamPuffRobot.exe 同级）——那里已有 windeployqt 部署的
    # Qt DLL 与 POST_BUILD 拷来的 `OrbbecSDK.dll` + `extensions/`，
    # 否则默认落在 `tests/` 子目录会因找不到 DLL 直接启动失败。
    set_target_properties(test_camera_check PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    )
endif()
