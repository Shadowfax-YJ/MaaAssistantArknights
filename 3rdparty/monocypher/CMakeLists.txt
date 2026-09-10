add_library(maa-monocypher STATIC monocypher.c monocypher-ed25519.c)
target_include_directories(maa-monocypher PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
set_target_properties(maa-monocypher PROPERTIES POSITION_INDEPENDENT_CODE ON)
