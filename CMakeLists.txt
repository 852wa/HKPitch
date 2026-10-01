cmake_minimum_required(VERSION 3.22)
project(HKPitch VERSION 1.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# C++ のランタイムを DLL に入れておく（Visual C++ 再頒布パッケージが無い PC でも動くように）
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# 自分の PC だけの設定（OBS の開発用ファイルの場所など）は local.cmake に書く（リポジトリには入れない）
include("${CMAKE_CURRENT_SOURCE_DIR}/local.cmake" OPTIONAL)

set(OBS_SOURCE_DIR "" CACHE PATH "OBS Studio source tree (for the libobs headers)")
set(OBS_LIBRARY "" CACHE FILEPATH "OBS import library (obs.lib)")
set(OBS_USER_PLUGIN_DIR "$ENV{ProgramData}/obs-studio/plugins/HKPitch" CACHE PATH "OBS plugin install directory")
option(HKPITCH_INSTALL_TO_OBS "Copy the plugin into the OBS plugin folder after building" OFF)

if(MSVC)
    add_compile_options(/W4 /utf-8 /permissive- /O2 /fp:fast)
endif()

add_library(voice_shifter STATIC src/voice-shifter.cpp src/voice-shifter.h src/psola.cpp src/psola.h src/three-band-eq.h)
set_target_properties(voice_shifter PROPERTIES POSITION_INDEPENDENT_CODE ON)

# OBS なしで動く確認用ツール
add_executable(hkpitch-offline tests/offline.cpp)
target_link_libraries(hkpitch-offline PRIVATE voice_shifter)

if(EXISTS "${OBS_LIBRARY}" AND EXISTS "${OBS_SOURCE_DIR}/libobs/obs-module.h")
    add_library(obs_import SHARED IMPORTED GLOBAL)
    set_target_properties(obs_import PROPERTIES
        IMPORTED_IMPLIB "${OBS_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${OBS_SOURCE_DIR}/libobs"
        INTERFACE_COMPILE_DEFINITIONS "HAVE_OBSCONFIG_H")

    add_library(${PROJECT_NAME} MODULE src/plugin-main.cpp)
    target_include_directories(${PROJECT_NAME} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/generated")
    target_compile_definitions(${PROJECT_NAME} PRIVATE HKPITCH_VERSION="${PROJECT_VERSION}")
    target_link_libraries(${PROJECT_NAME} PRIVATE obs_import voice_shifter)
    set_target_properties(${PROJECT_NAME} PROPERTIES PREFIX "" OUTPUT_NAME "HKPitch")

    if(HKPITCH_INSTALL_TO_OBS)
        add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory "${OBS_USER_PLUGIN_DIR}/bin/64bit" "${OBS_USER_PLUGIN_DIR}/data"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:${PROJECT_NAME}>" "${OBS_USER_PLUGIN_DIR}/bin/64bit"
            COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/data" "${OBS_USER_PLUGIN_DIR}/data"
            COMMENT "Installing HKPitch into OBS Studio")
    endif()
else()
    message(WARNING "OBS SDK not found (set OBS_SOURCE_DIR and OBS_LIBRARY); building only the offline test tool")
endif()
