cmake_minimum_required(VERSION 3.24)
project(
    RhineMusicDatabase
    VERSION 0.4.0
    LANGUAGES CXX)
set(RHINE_APPLICATION_ID "rhine.music.database")
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
find_package(
    Qt6 6.8 REQUIRED
    COMPONENTS Core
               Gui
               Qml
               Quick
               Network
               Concurrent
               ShaderTools
               Multimedia)
if(NOT TARGET Qt6::GuiPrivate)
    find_package(Qt6GuiPrivate REQUIRED)
endif()
if(NOT TARGET Qt6::MultimediaPrivate)
    find_package(Qt6MultimediaPrivate REQUIRED)
endif()
qt_policy(SET QTP0001 NEW)
qt_policy(SET QTP0004 NEW)
set_source_files_properties(Qt/qml/Main.qml PROPERTIES QT_RESOURCE_ALIAS Main.qml)
# 自维护源码和锁定的二维码库构成独立应用目标。
qt_add_executable(
    RhineMusicDatabase
    WIN32
    MACOSX_BUNDLE
    Qt/src/AudioPlayer.cpp
    Qt/src/AudioPlayer.h
    Qt/src/AppStorage.cpp
    Qt/src/AppStorage.h
    Qt/src/DiscGeometry.cpp
    Qt/src/DiscGeometry.h
    Qt/src/FrameProfiler.cpp
    Qt/src/FrameProfiler.h
    Qt/src/NativeCards.cpp
    Qt/src/NativeCards.h
    Qt/src/ArchiveField.cpp
    Qt/src/ArchiveField.h
    Qt/src/NativePicking.cpp
    Qt/src/NativePicking.h
    Qt/tests/UiChecks.cpp
    Qt/src/Icon.cpp
    Qt/src/SceneRuntime.cpp
    Qt/src/SceneRuntime.h
    Qt/src/main.cpp
    Qt/src/Host.cpp
    Qt/src/Host.h
    Qt/src/Platform.cpp
    Qt/src/Engine.cpp
    Qt/src/Engine.h
    Qt/src/Hud.cpp
    Qt/src/Hud.h
    Qt/src/RhiScene.cpp
    Qt/src/RhiScene.h
    Qt/src/Glyphs.cpp
    Qt/src/Glyphs.h
    vendor/qrcodegen/qrcodegen.cpp)
target_include_directories(RhineMusicDatabase PRIVATE vendor/qrcodegen Qt/src)
qt_add_resources(
    RhineMusicDatabase
    "rhine_icons"
    PREFIX
    "/icons"
    BASE
    "Assets/icons"
    FILES
    Assets/icons/icon-16.png
    Assets/icons/icon-32.png
    Assets/icons/icon-64.png
    Assets/icons/icon-128.png
    Assets/icons/icon-256.png
    Assets/icons/icon-512.png
    Assets/icons/icon-1024.png)
qt_add_qml_module(
    RhineMusicDatabase
    URI
    RhineMusic
    VERSION
    1.0
    QML_FILES
    Qt/qml/Main.qml)
# 着色器按原通道顺序打包，所有后端使用同一份公式。
qt_add_shaders(
    RhineMusicDatabase
    "rhine_shaders"
    PREFIX
    "/shaders"
    BASE
    "Qt/shaders"
    GLSL
    "150,300es"
    HLSL
    50
    MSL
    12
    FILES
    Qt/shaders/geometry.vert
    Qt/shaders/shadow.vert
    Qt/shaders/shadow.frag
    Qt/shaders/surface.frag
    Qt/shaders/normal.frag
    Qt/shaders/quad.vert
    Qt/shaders/aoFragment.frag
    Qt/shaders/blurAO.frag
    Qt/shaders/combineAO.frag
    Qt/shaders/bokehFragment.frag
    Qt/shaders/finalFragment.frag)
target_compile_definitions(RhineMusicDatabase PRIVATE RHINE_APPLICATION_ID="${RHINE_APPLICATION_ID}"
                                                      $<$<CONFIG:Debug>:QT_QML_DEBUG>)
target_link_libraries(
    RhineMusicDatabase
    PRIVATE Qt6::Core
            Qt6::Gui
            Qt6::GuiPrivate
            Qt6::Qml
            Qt6::Quick
            Qt6::Network
            Qt6::Concurrent
            Qt6::Multimedia)
# 平台分支只处理原生能力、部署身份及链接依赖。
if(APPLE)
    enable_language(OBJCXX)
    set(CMAKE_OBJCXX_STANDARD 20)
    target_sources(RhineMusicDatabase PRIVATE Qt/src/Platform.mm)
    target_link_libraries(RhineMusicDatabase PRIVATE "-framework Security" "-framework AppKit"
                                                     "-framework MediaPlayer")
    target_compile_options(RhineMusicDatabase PRIVATE $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>)
    set_target_properties(
        RhineMusicDatabase
        PROPERTIES MACOSX_BUNDLE_GUI_IDENTIFIER ${RHINE_APPLICATION_ID}
                   MACOSX_BUNDLE_BUNDLE_NAME "RHINE MUSIC DATABASE"
                   MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/Qt/Info.plist.in")
elseif(WIN32)
    configure_file(Qt/WindowsIcon.rc.in "${CMAKE_CURRENT_BINARY_DIR}/WindowsIcon.rc" @ONLY)
    target_sources(RhineMusicDatabase PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/WindowsIcon.rc")
    target_sources(RhineMusicDatabase PRIVATE Qt/src/PlatformWindows.cpp)
    target_link_libraries(RhineMusicDatabase PRIVATE crypt32 shell32 user32 windowsapp runtimeobject
                                                     Qt6::MultimediaPrivate)
    target_compile_options(RhineMusicDatabase PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
else()
    message(FATAL_ERROR "Supported desktop platforms are macOS and Windows")
endif()
# Qt Creator 的普通构建也交付完整应用，不能依赖运行目录里的 output/resources。
option(RHINE_PACKAGE_APP "Package resources and Qt libraries after a macOS CMake build" ON)
if(APPLE AND RHINE_PACKAGE_APP)
    find_program(RHINE_NPM_EXECUTABLE NAMES npm REQUIRED)
    get_filename_component(RHINE_QT_KIT "${Qt6_DIR}/../../.." ABSOLUTE)
    # 脚本、字体和场景数据改变时重新链接，确保 POST_BUILD 重新生成部署资源。
    file(
        GLOB_RECURSE
        RHINE_RESOURCE_INPUTS
        CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/Scene/*"
        "${CMAKE_CURRENT_SOURCE_DIR}/Resources/*"
        "${CMAKE_CURRENT_SOURCE_DIR}/Assets/*"
        "${CMAKE_CURRENT_SOURCE_DIR}/Qt/logic/*"
        "${CMAKE_CURRENT_SOURCE_DIR}/scripts/*")
    list(
        APPEND RHINE_RESOURCE_INPUTS "${CMAKE_CURRENT_SOURCE_DIR}/package.json"
        "${CMAKE_CURRENT_SOURCE_DIR}/MusicBridge/worker.cjs" "${CMAKE_CURRENT_SOURCE_DIR}/MusicBridge/qq.cjs"
        "${CMAKE_CURRENT_SOURCE_DIR}/MusicBridge/package-lock.json")
    set_property(
        TARGET RhineMusicDatabase
        APPEND
        PROPERTY LINK_DEPENDS ${RHINE_RESOURCE_INPUTS})
    add_custom_command(
        TARGET RhineMusicDatabase
        POST_BUILD
        COMMAND "${RHINE_NPM_EXECUTABLE}" run package:cmake -- "$<TARGET_BUNDLE_DIR:RhineMusicDatabase>"
                "${RHINE_QT_KIT}" "$<CONFIG>"
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        COMMENT "Packaging standalone music panel resources and Qt runtime"
        VERBATIM)
endif()
enable_testing()
qt_add_executable(RhinePlatformRegression Qt/tests/PlatformRegression.cpp Qt/src/AppStorage.cpp
                  Qt/src/AudioPlayer.cpp)
target_include_directories(RhinePlatformRegression PRIVATE Qt/src)
target_link_libraries(RhinePlatformRegression PRIVATE Qt6::Core Qt6::Gui Qt6::Multimedia
                                                      Qt6::MultimediaPrivate)
if(WIN32)
    target_sources(RhinePlatformRegression PRIVATE Qt/src/PlatformWindows.cpp)
    target_link_libraries(
        RhinePlatformRegression
        PRIVATE Qt6::Qml
                Qt6::Network
                crypt32
                shell32
                user32
                windowsapp
                runtimeobject)
    target_compile_definitions(RhinePlatformRegression PRIVATE RHINE_APPLICATION_ID="${RHINE_APPLICATION_ID}")
    target_compile_options(RhinePlatformRegression PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
endif()
add_test(NAME qt_platform_regression COMMAND RhinePlatformRegression
                                             "${CMAKE_CURRENT_BINARY_DIR}/platform-regression")
set_tests_properties(qt_platform_regression PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
add_test(NAME qt_offline COMMAND RhineMusicDatabase --self-test)
set_tests_properties(
    qt_offline
    PROPERTIES ENVIRONMENT
               "QT_QPA_PLATFORM=offscreen;RHINE_RESOURCES=${CMAKE_CURRENT_SOURCE_DIR}/output/resources")
