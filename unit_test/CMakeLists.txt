cmake_minimum_required(VERSION 3.24)

project(MAAUnitTests LANGUAGES CXX)

include(CTest)
include(FetchContent)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CATCH_INSTALL_DOCS OFF CACHE BOOL "" FORCE)
set(CATCH_INSTALL_EXTRAS ON CACHE BOOL "" FORCE)
set(CATCH_DEVELOPMENT_BUILD OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
    Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG v3.8.1
    GIT_SHALLOW TRUE
)

FetchContent_MakeAvailable(Catch2)

include(Catch)

add_executable(maa-algorithm-test
    MaaCore/AlgorithmTest.cpp
)

add_executable(maa-black-flow-store-screenshot-trigger-test
    MaaCore/BlackFlowStoreScreenshotTriggerTest.cpp
)

add_executable(maa-black-flow-store-contract-test
    MaaCore/BlackFlowStoreContractTest.cpp
)

target_compile_features(maa-algorithm-test PRIVATE cxx_std_20)
target_link_libraries(maa-algorithm-test PRIVATE Catch2::Catch2WithMain)
target_include_directories(maa-algorithm-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src/MaaCore)

target_compile_features(maa-black-flow-store-screenshot-trigger-test PRIVATE cxx_std_20)
target_link_libraries(maa-black-flow-store-screenshot-trigger-test PRIVATE Catch2::Catch2WithMain)
target_include_directories(maa-black-flow-store-screenshot-trigger-test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/MaaCore
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/MaaUtils/include
)
if(MSVC)
    target_compile_options(maa-black-flow-store-screenshot-trigger-test PRIVATE /utf-8)
endif()

target_compile_features(maa-black-flow-store-contract-test PRIVATE cxx_std_20)
target_link_libraries(maa-black-flow-store-contract-test PRIVATE Catch2::Catch2WithMain)
target_include_directories(maa-black-flow-store-contract-test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/MaaCore
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/MaaUtils/include
)
target_compile_definitions(maa-black-flow-store-contract-test PRIVATE
    MAA_TEST_RESOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../resource"
)
if(MSVC)
    target_compile_options(maa-black-flow-store-contract-test PRIVATE /utf-8)
endif()

catch_discover_tests(maa-algorithm-test TEST_PREFIX "algorithm::")
catch_discover_tests(maa-black-flow-store-screenshot-trigger-test TEST_PREFIX "black-flow-store-screenshot-trigger::")
catch_discover_tests(maa-black-flow-store-contract-test TEST_PREFIX "black-flow-store-contract::")
