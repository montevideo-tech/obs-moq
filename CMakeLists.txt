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
    add_subdirectory(${MOQ_LOCAL}/rs/hang-c hang)
else()
    include(FetchContent)
    FetchContent_Declare(hang
        URL https://github.com/kixelated/moq/releases/download/v${HANG_VERSION}/hang-${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}.tar.gz
    )
    FetchContent_MakeAvailable(hang)

    add_library(hang SHARED IMPORTED GLOBAL)
    set_target_properties(hang PROPERTIES
        IMPORTED_LOCATION "${hang_SOURCE_DIR}/lib/libhang.dylib"
        INTERFACE_INCLUDE_DIRECTORIES "${hang_SOURCE_DIR}/include"
    )
endif()

target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE hang)

add_custom_command(TARGET ${CMAKE_PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:hang>
            $<TARGET_BUNDLE_CONTENT_DIR:${CMAKE_PROJECT_NAME}>/MacOS/
)

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

target_sources(${CMAKE_PROJECT_NAME} PRIVATE src/obs-moq.cpp src/moq-output.h src/moq-service.h src/moq-output.cpp src/moq-service.cpp)

set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME ${_name})
