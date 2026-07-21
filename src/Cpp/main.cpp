#include "AsstCaller.h"

#include <filesystem>
#include <iostream>
#include <stdio.h>
#include <string>
#include <string_view>
#include <thread>

#ifdef SMOKE_TESTING
#include "Task/Miscellaneous/BlackFlowStoreTaskGraph.h"
#include "Vision/Roguelike/BlackFlowStoreImageAnalyzer.h"
#endif

#ifdef SMOKE_TESTING
namespace
{
bool run_black_flow_custom_admission_smoke_test(AsstHandle handle, std::string_view client_type)
{
    const bool supported = client_type == "Official" || client_type == "Bilibili";
    const auto params_for = [client_type](std::string_view task_name) {
        return std::string("{\"task_names\":[\"") + std::string(task_name) + "\"],\"client_type\":\"" +
               std::string(client_type) + "\"}";
    };

    const auto stable_task_id = AsstAppendTask(
        handle,
        "Custom",
        params_for("MiniGame@BlackFlow@Begin").c_str());
    if ((stable_task_id != 0) != supported) {
        std::cerr << "BlackFlow Custom admission mismatch for " << client_type << std::endl;
        return false;
    }

    const auto legacy_task_id = AsstAppendTask(
        handle,
        "Custom",
        params_for("BlackFlowTemporary@Begin").c_str());
    if (legacy_task_id != 0) {
        std::cerr << "Legacy BlackFlow Custom task was admitted for " << client_type << std::endl;
        return false;
    }

    return true;
}
} // namespace
#endif

int main([[maybe_unused]] int argc, char** argv)
{
    auto working_path = std::filesystem::path(argv[0]).parent_path();

    if (!std::filesystem::exists(working_path / "resource")) {
        std::cerr << "resource folder not found!" << std::endl;
        return -1;
    }

    // 可以将日志、调试图片等存到别的目录下,需要在最一开始调用。不调用默认保存到资源同目录
    // AsstSetUserDir(working_path.c_str());

    // 这里默认读取的是可执行文件同目录下 resource 文件夹里的资源
    if (!AsstLoadResource(working_path.string().c_str())) {
        std::cerr << "-------- load resource failed: official --------" << std::endl;
        return -1;
    }

#ifdef ASST_DEBUG
    if (argc > 1) {
        const std::string arg(argv[1]);

        if (arg == "Official" || arg == "Bilibili") {
            std::cout << arg << " type detected, using default resources." << std::endl;
        }
        else {
            std::cout << "load overseas_type: " << arg << std::endl;

            const auto overseas_path = working_path / "resource" / "global" / arg;
            if (!AsstLoadResource(overseas_path.string().c_str())) {
                std::cerr << "-------- load resource failed: " << arg << " --------" << std::endl;
                return -1;
            }
        }
    }
#endif

    auto ptr = AsstCreate();
    if (ptr == nullptr) {
        std::cerr << "create failed" << std::endl;
        return -1;
    }

#ifdef SMOKE_TESTING
    if (argc > 1 && !run_black_flow_custom_admission_smoke_test(ptr, argv[1])) {
        std::cerr << "-------- BlackFlow Custom admission smoke test failed --------" << std::endl;
        AsstDestroy(ptr);
        return -1;
    }
    if (argc > 1 &&
        (std::string_view(argv[1]) == "Official" || std::string_view(argv[1]) == "Bilibili") &&
        !asst::run_black_flow_store_task_graph_smoke_test()) {
        std::cerr << "-------- BlackFlow TaskData smoke test failed --------" << std::endl;
        AsstDestroy(ptr);
        return -1;
    }
    if (argc > 1 && std::string_view(argv[1]) == "Official" &&
        !asst::run_black_flow_store_fixture_smoke_test(working_path / "black_flow_store_fixtures")) {
        std::cerr << "-------- BlackFlow fixture smoke test failed --------" << std::endl;
        AsstDestroy(ptr);
        return -1;
    }
    std::cout << "Ended early for smoke testing." << std::endl;
    AsstDestroy(ptr);
    return 0;
#endif

#ifndef ASST_DEBUG
    AsstAsyncConnect(ptr, "adb", "127.0.0.1:5555", nullptr, true);
#else
    AsstAsyncConnect(ptr, "adb", "127.0.0.1:5555", "DEBUG", true);
#endif
    if (!AsstConnected(ptr)) {
        std::cerr << "connect failed" << std::endl;
        AsstDestroy(ptr);
        return -1;
    }

#ifdef ASST_DEBUG
    AsstAppendTask(ptr, "Debug", nullptr);
#else
    /* 详细参数可参考 docs / 集成文档.md */
    AsstAppendTask(ptr, "StartUp", nullptr);

    AsstAppendTask(ptr, "Fight", R"(
    {
        "stage": "1-7"
    }
    )");

    AsstAppendTask(ptr, "Recruit", R"(
    {
        "select":[4],
        "confirm":[3,4],
        "times":4
    }
    )");

    AsstAppendTask(ptr, "Infrast", R"(
    {
        "facility": ["Mfg", "Trade", "Power", "Control", "Reception", "Office", "Dorm"],
        "drones": "Money"
    }
    )");

    AsstAppendTask(ptr, "Mall", R"(
    {
        "shopping": true,
        "buy_first": [
            "许可"
        ],
        "black_list": [
            "家具",
            "碳"
        ]
    }
    )");

    AsstAppendTask(ptr, "Award", R"(
    {
        "award": true,
        "mail": true,
        "recruit": true,
        "orundum": true,
        "mining": true,
        "specialaccess": true
    }
    )");

    AsstAppendTask(ptr, "Roguelike", R"(
    {
        "theme": "Sarkaz",
        "mode": 1,
        "squad": "蓝图测绘分队",
        "roles": "稳扎稳打",
        "core_char": "维什戴尔"
    }
    )");
#endif

    AsstStart(ptr);

    while (AsstRunning(ptr)) {
        std::this_thread::yield();
    }

    AsstStop(ptr);
    AsstDestroy(ptr);

    return 0;
}
