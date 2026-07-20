if(BUILD_DEBUG_DEMO)
    add_executable(debug_demo main.cpp)

    if(MSVC)
        target_compile_options(debug_demo PRIVATE "/WX-")
    else()
        target_compile_options(debug_demo PRIVATE "-Wno-error")
    endif()

    target_link_libraries(debug_demo MaaCore)

    add_dependencies(debug_demo MaaCore)

    # 为 debug_demo 创建资源目录链接
    create_resource_link(debug_demo "$<TARGET_FILE_DIR:debug_demo>")

    # install(TARGETS debug_demo ${MaaCore_install_flatten_args})
endif(BUILD_DEBUG_DEMO)

if(BUILD_SMOKE_TEST)
    add_executable(smoke_test main.cpp)

    target_compile_definitions(MaaCore PRIVATE ASST_BUILD_SMOKE_TEST)

    if(MSVC)
        target_compile_options(smoke_test PRIVATE "/WX-")
    else()
        target_compile_options(smoke_test PRIVATE "-Wno-error")
    endif()

    target_compile_definitions(smoke_test PRIVATE SMOKE_TESTING ASST_BUILD_SMOKE_TEST)
    target_include_directories(smoke_test PRIVATE ${PROJECT_SOURCE_DIR}/src/MaaCore)
    target_link_libraries(smoke_test MaaCore)

    add_dependencies(smoke_test MaaCore)
    install(TARGETS smoke_test ${MaaCore_install_flatten_args})
    install(
        DIRECTORY ${PROJECT_SOURCE_DIR}/unit_test/fixtures/black_flow_store/
        DESTINATION black_flow_store_fixtures
    )
endif(BUILD_SMOKE_TEST)
