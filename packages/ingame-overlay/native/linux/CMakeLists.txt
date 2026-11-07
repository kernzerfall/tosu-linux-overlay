cmake_minimum_required(VERSION 3.20)
project(tosu_overlay_linux C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

option(TOSU_OVERLAY_TESTS "Build test programs" OFF)

# ---- Vulkan headers: system copy, or fetch the version matching current loaders ----
find_path(VULKAN_LAYER_INCLUDE_DIR vulkan/vk_layer.h)
if(VULKAN_LAYER_INCLUDE_DIR)
    set(TOSU_VULKAN_INCLUDE ${VULKAN_LAYER_INCLUDE_DIR})
else()
    include(FetchContent)
    FetchContent_Declare(
        vulkan_headers
        GIT_REPOSITORY https://github.com/KhronosGroup/Vulkan-Headers.git
        GIT_TAG v1.4.363
        GIT_SHALLOW TRUE
    )
    FetchContent_GetProperties(vulkan_headers)
    if(NOT vulkan_headers_POPULATED)
        FetchContent_Populate(vulkan_headers)
    endif()
    set(TOSU_VULKAN_INCLUDE ${vulkan_headers_SOURCE_DIR}/include)
endif()

# ---- shaders -> SPIR-V as C array initializers ----
find_program(GLSLC glslc REQUIRED)
set(SHADER_OUT ${CMAKE_CURRENT_BINARY_DIR}/shaders)
file(MAKE_DIRECTORY ${SHADER_OUT})
set(SHADER_INCS)
foreach(stage vert frag)
    set(src ${CMAKE_CURRENT_SOURCE_DIR}/shaders/overlay.${stage})
    set(out ${SHADER_OUT}/overlay.${stage}.spv.inc)
    add_custom_command(
        OUTPUT ${out}
        COMMAND ${GLSLC} -O --target-env=vulkan1.0 -mfmt=num -o ${out} ${src}
        DEPENDS ${src}
        COMMENT "Compiling overlay.${stage}"
    )
    list(APPEND SHADER_INCS ${out})
endforeach()

add_library(tosu-overlay SHARED
    src/hook.c
    src/channel.c
    src/frame.c
    src/input.c
    src/render_gl.c
    src/vulkan.c
    ${SHADER_INCS}
)
target_include_directories(tosu-overlay PRIVATE ${TOSU_VULKAN_INCLUDE} ${SHADER_OUT})
target_compile_options(tosu-overlay PRIVATE -Wall -Wextra -Wno-unused-parameter -fvisibility=hidden)
# only dlsym and the layer negotiation entry point are exported
target_link_options(tosu-overlay PRIVATE
    -Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/exports.map
    -Wl,-z,defs
)
target_link_libraries(tosu-overlay PRIVATE dl pthread)
set_target_properties(tosu-overlay PROPERTIES PREFIX "lib" OUTPUT_NAME "tosu-overlay")

configure_file(${CMAKE_CURRENT_SOURCE_DIR}/tosu_overlay.json
               ${CMAKE_CURRENT_BINARY_DIR}/tosu_overlay.json COPYONLY)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/tosu-overlay-run
               ${CMAKE_CURRENT_BINARY_DIR}/tosu-overlay-run COPYONLY)

if(TOSU_OVERLAY_TESTS)
    add_subdirectory(tests)
endif()
