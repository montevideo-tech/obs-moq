cmake_minimum_required(VERSION 3.28)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

project(${_name} VERSION ${_version})

option(ENABLE_FRONTEND_API "Use obs-frontend-api for UI functionality" OFF)
option(ENABLE_QT "Use Qt functionality" OFF)

include(compilerconfig)
include(defaults)
include(helpers)

add_library(${CMAKE_PROJECT_NAME} MODULE)

find_package(libobs REQUIRED)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::libobs)

option(MOQ_LOCAL "Path to moq repo for local development" "")

if(MOQ_LOCAL)
  add_subdirectory(${MOQ_LOCAL}/rs/libmoq moq)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE moq)
else()
  include(FetchContent)
  FetchContent_Declare(
    moq
    URL https://github.com/moq-dev/moq/releases/download/libmoq-v${MOQ_VERSION}/moq-${MOQ_VERSION}-${MOQ_TARGET}.${MOQ_ARCHIVE}
  )
  FetchContent_MakeAvailable(moq)

  find_package(moq REQUIRED PATHS ${moq_SOURCE_DIR} NO_DEFAULT_PATH)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE moq::moq)
endif()

if(ENABLE_FRONTEND_API)
  find_package(obs-frontend-api REQUIRED)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::obs-frontend-api)
endif()

if(ENABLE_QT)
  find_package(Qt6 COMPONENTS Widgets Core)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE Qt6::Core Qt6::Widgets)
  target_compile_options(
    ${CMAKE_PROJECT_NAME}
    PRIVATE $<$<C_COMPILER_ID:Clang,AppleClang>:-Wno-quoted-include-in-framework-header -Wno-comma>
  )
  set_target_properties(
    ${CMAKE_PROJECT_NAME}
    PROPERTIES AUTOMOC ON AUTOUIC ON AUTORCC ON
  )
endif()

# FFmpeg dependency
include(FindPkgConfig)
pkg_check_modules(FFMPEG REQUIRED libavcodec libavutil libswscale libswresample)
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE ${FFMPEG_INCLUDE_DIRS})
target_link_directories(${CMAKE_PROJECT_NAME} PRIVATE ${FFMPEG_LIBRARY_DIRS})
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE ${FFMPEG_LIBRARIES})

target_sources(
  ${CMAKE_PROJECT_NAME}
  PRIVATE src/obs-moq.cpp src/moq-output.h src/moq-service.h src/moq-output.cpp src/moq-service.cpp
          src/moq-source.cpp src/moq-source.h
)

set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME ${_name})
