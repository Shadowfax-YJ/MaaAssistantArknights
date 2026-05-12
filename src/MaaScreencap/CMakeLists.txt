set(MaaScreencap_src
    PlatformShim.cpp
    ${PROJECT_SOURCE_DIR}/src/MaaCore/ScreencapCaller.cpp
    ${PROJECT_SOURCE_DIR}/src/MaaCore/Controller/MumuExtras.cpp
)

if(WIN32)
    list(APPEND MaaScreencap_src
        ${PROJECT_SOURCE_DIR}/src/MaaCore/Controller/Win32ControlUnitLoader.cpp
    )
endif()

add_library(MaaScreencap SHARED ${MaaScreencap_src})

if(WITH_EMULATOR_EXTRAS)
    if(EXISTS ${PROJECT_SOURCE_DIR}/3rdparty/EmulatorExtras/Mumu)
        target_include_directories(MaaScreencap PUBLIC ${PROJECT_SOURCE_DIR}/3rdparty/EmulatorExtras)
    else()
        message(WARNING "EmulatorExtras not found, please run `git submodule update --init 3rdparty/EmulatorExtras`")
        set(WITH_EMULATOR_EXTRAS OFF)
    endif()
endif()

target_compile_definitions(MaaScreencap PRIVATE ASST_WITH_EMULATOR_EXTRAS=$<BOOL:${WITH_EMULATOR_EXTRAS}>)
target_compile_definitions(MaaScreencap PRIVATE ASST_DLL_EXPORTS _UNICODE UNICODE)

target_include_directories(MaaScreencap
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include
    PRIVATE
        ${PROJECT_SOURCE_DIR}/src/MaaCore
        ${PROJECT_SOURCE_DIR}/src/MaaUtils/include
)

target_link_libraries(MaaScreencap HeaderOnlyLibraries ${OpenCV_LIBS})

if(WIN32)
    target_link_libraries(MaaScreencap ws2_32)
endif()

if(MSVC)
    target_compile_options(MaaScreencap PRIVATE /utf-8)
endif()

install(TARGETS MaaScreencap ${MaaCore_install_flatten_args})
