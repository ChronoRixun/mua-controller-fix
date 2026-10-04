cmake_minimum_required(VERSION 3.21)
project(mua_controller_fix VERSION 1.2.0 LANGUAGES CXX RC)

if(NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
	message(FATAL_ERROR "Build with MSVC for x64 - the 2016 Marvel: Ultimate Alliance ports are 64-bit.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>") # no VC++ redistributable needed
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin") # test runs next to the DLL
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

add_compile_definitions(WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
add_compile_options(/W4 /permissive- /utf-8)

# The fix: a drop-in dinput8.dll for the game folder.
add_library(controller_fix SHARED
	src/dllmain.cpp
	src/exports.cpp
	src/gamepad_fix.cpp
	src/xinput_pad.cpp
	src/log.cpp
	src/iat_hook.cpp
	src/mod_loader.cpp
	src/loose_first.cpp
	src/steam_presence.cpp
	src/discord_presence.cpp
	src/discord_ipc.cpp
	src/dinput8.def
	src/version.rc)
target_link_libraries(controller_fix PRIVATE dxguid)
# Output is dinput8.dll; its import library gets a different name so it can't shadow the Windows SDK's dinput8.lib.
set_target_properties(controller_fix PROPERTIES OUTPUT_NAME dinput8 ARCHIVE_OUTPUT_NAME controller_fix)

# Checks the built DLL the way the game loads it (next to the executable).
add_executable(fix_test test/fix_test.cpp)
target_link_libraries(fix_test PRIVATE dinput8.lib dxguid xinput)
add_dependencies(fix_test controller_fix)
