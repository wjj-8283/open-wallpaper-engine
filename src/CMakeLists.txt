find_package(OpenGL REQUIRED)
find_package(Threads REQUIRED)
pkg_check_modules(LZ4 REQUIRED liblz4)
pkg_check_modules(AVFORMAT REQUIRED IMPORTED_TARGET libavformat)
pkg_check_modules(AVCODEC REQUIRED IMPORTED_TARGET libavcodec)
pkg_check_modules(AVUTIL REQUIRED IMPORTED_TARGET libavutil)
pkg_check_modules(SWRESAMPLE REQUIRED IMPORTED_TARGET libswresample)

option(ENABLE_RENDERDOC "Build with renderdoc api" OFF)

if(ENABLE_RENDERDOC)
  add_compile_definitions(ENABLE_RENDERDOC_API=1)
endif()

include(TestBigEndian)
test_big_endian(ENDIAN)
if(ENDIAN)
  add_compile_definitions(WP_BIG_ENDIAN)
endif()

if(APPLE)
  find_library(ACCELERATE_FRAMEWORK Accelerate REQUIRED)
  find_library(CORE_GRAPHICS_FRAMEWORK CoreGraphics REQUIRED)
  find_library(CORE_MEDIA_FRAMEWORK CoreMedia REQUIRED)
  find_library(CORE_VIDEO_FRAMEWORK CoreVideo REQUIRED)
  find_library(FOUNDATION_FRAMEWORK Foundation REQUIRED)
  find_library(IOSURFACE_FRAMEWORK IOSurface REQUIRED)
  find_library(METAL_FRAMEWORK Metal REQUIRED)
  find_library(VIDEOTOOLBOX_FRAMEWORK VideoToolbox REQUIRED)
endif()

set(warn_opts
    -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wno-unused-variable
    CACHE INTERNAL "")

include_directories(.)

add_subdirectory(Fs)
add_subdirectory(Utils)
add_subdirectory(Timer)
add_subdirectory(Audio)
add_subdirectory(Scene)
add_subdirectory(Looper)
add_subdirectory(Vulkan)
add_subdirectory(Particle)
add_subdirectory(VulkanRender)
add_subdirectory(RenderGraph)

add_library(
  ${PROJECT_NAME} STATIC
  Presentation/WallpaperScaling.cpp
  WPJson.cpp
  WPPkgFs.cpp
  Type.cpp
  Runtime/DynamicValue.cpp
  Runtime/SceneRuntimeContext.cpp
  Runtime/SceneSettingResolver.cpp
  Runtime/ScriptedDynamicValue.cpp
  Scripting/ScriptEngine.cpp
  wpscene/WPImageObject.cpp
  wpscene/WPParticleObject.cpp
  wpscene/WPMaterial.cpp
  wpscene/WPScene.cpp
  wpscene/WPLightObject.cpp
  WPParticleParser.cpp
  WPShaderParser.cpp
  Shader/ShaderPreprocessor.cpp
  Shader/SceneShaderLegalizer.cpp
  WPSceneParser.cpp
  WPShaderValueUpdater.cpp
  WPTexImageParser.cpp
  WPSoundParser.cpp
  WPMdlParser.cpp
  WPPuppet.cpp
  Project/ProjectManifest.cpp
  Project/ProjectProperties.cpp
  Runtime/VirtualAssetRegistry.cpp
  Video/FfmpegVideoTextureSource.cpp
  Platform/Apple/FfmpegVideoInterop.mm
  SceneSourceResolver.cpp
  SceneWallpaper.cpp)

add_library(wescene::renderer ALIAS ${PROJECT_NAME})

set(InteralLib
    wpAudio
    wpLooper
    wpVulkanRender
    wpFs
    wpScene
    wpVulkan
    wpRGraph
    wpParticle
    wpTimer)

target_link_libraries(
  ${PROJECT_NAME}
  PUBLIC wpUtils
  PRIVATE ${InteralLib} nlohmann_json quickjs::quickjs ${LZ4_LIBRARIES}
          PkgConfig::AVFORMAT
          PkgConfig::AVCODEC
          PkgConfig::AVUTIL
          PkgConfig::SWRESAMPLE
          ${CMAKE_THREAD_LIBS_INIT} ${CMAKE_DL_LIBS})

if(APPLE)
  target_link_libraries(
    ${PROJECT_NAME}
    PUBLIC
      ${CORE_GRAPHICS_FRAMEWORK}
      ${CORE_MEDIA_FRAMEWORK}
      ${CORE_VIDEO_FRAMEWORK}
      ${ACCELERATE_FRAMEWORK}
      ${FOUNDATION_FRAMEWORK}
      ${IOSURFACE_FRAMEWORK}
      ${METAL_FRAMEWORK}
      ${VIDEOTOOLBOX_FRAMEWORK})
endif()

target_include_directories(${PROJECT_NAME} PUBLIC . Swapchain)
set_target_properties(${PROJECT_NAME} PROPERTIES
  OBJCXX_STANDARD 20
  OBJCXX_STANDARD_REQUIRED YES)
target_compile_options(${PROJECT_NAME} PRIVATE
  "$<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>")
set_property(TARGET ${PROJECT_NAME} PROPERTY POSITION_INDEPENDENT_CODE ON)
