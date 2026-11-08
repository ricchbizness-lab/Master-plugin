cmake_minimum_required(VERSION 3.22)
project(RicchBeatzMaster VERSION 1.0.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(MASTER_DEV_TOOLS "Build MasterTest (CLI) and MasterSnapshot (UI -> PNG)" OFF)

# Runtime Visual C++ lié statiquement : l'exe tourne sans installer les redistribuables
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

include(FetchContent)
FetchContent_Declare(JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG 8.0.4
    GIT_SHALLOW ON)
FetchContent_MakeAvailable(JUCE)

juce_add_binary_data(MasterAssets SOURCES
    Resources/fonts/Inter_500Medium.ttf
    Resources/fonts/Inter_600SemiBold.ttf
    Resources/fonts/Sora_600SemiBold.ttf
    Resources/logo.png
    Resources/galaxy.jpg)

# Moteur de mastering (sans interface)
add_library(MasterCore STATIC Source/MasterEngine.cpp)
target_compile_definitions(MasterCore PUBLIC JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_USE_MP3AUDIOFORMAT=1)
target_compile_options(MasterCore PUBLIC $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
target_link_libraries(MasterCore PUBLIC juce::juce_dsp juce::juce_audio_formats
    PRIVATE juce::juce_recommended_config_flags)

set(MASTER_UI_SOURCES Source/MainComponent.cpp)

if(EXISTS ${CMAKE_SOURCE_DIR}/Source/Main.cpp)
juce_add_gui_app(RicchBeatzMaster
    COMPANY_NAME "RicchBeatz"
    PRODUCT_NAME "RicchBeatz Master")
target_sources(RicchBeatzMaster PRIVATE Source/Main.cpp ${MASTER_UI_SOURCES})
target_compile_definitions(RicchBeatzMaster PRIVATE
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_DISPLAY_SPLASH_SCREEN=0)
target_link_libraries(RicchBeatzMaster PRIVATE MasterCore MasterAssets
    juce::juce_audio_utils juce::juce_gui_extra
    juce::juce_recommended_config_flags juce::juce_recommended_lto_flags)
endif()

if(MASTER_DEV_TOOLS)
    juce_add_console_app(MasterTest PRODUCT_NAME "MasterTest")
    target_sources(MasterTest PRIVATE Tools/MasterTest.cpp)
    target_link_libraries(MasterTest PRIVATE MasterCore juce::juce_recommended_config_flags)

    juce_add_console_app(UiTest PRODUCT_NAME "UiTest")
    target_sources(UiTest PRIVATE Tools/UiTest.cpp ${MASTER_UI_SOURCES})
    target_compile_definitions(UiTest PUBLIC JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_MODAL_LOOPS_PERMITTED=1)
    target_link_libraries(UiTest PRIVATE MasterCore MasterAssets
        juce::juce_audio_utils juce::juce_gui_extra juce::juce_recommended_config_flags)

    if(EXISTS ${CMAKE_SOURCE_DIR}/Tools/Snapshot.cpp)
    juce_add_console_app(MasterSnapshot PRODUCT_NAME "MasterSnapshot")
    target_sources(MasterSnapshot PRIVATE Tools/Snapshot.cpp ${MASTER_UI_SOURCES})
    target_compile_definitions(MasterSnapshot PUBLIC JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_MODAL_LOOPS_PERMITTED=1)
    target_link_libraries(MasterSnapshot PRIVATE MasterCore MasterAssets
        juce::juce_audio_utils juce::juce_gui_extra juce::juce_recommended_config_flags)
    endif()
endif()
