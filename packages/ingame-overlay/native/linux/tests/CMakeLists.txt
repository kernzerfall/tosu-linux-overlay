find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL3 REQUIRED IMPORTED_TARGET sdl3)

add_executable(overlay_test overlay_test.c)
target_include_directories(overlay_test PRIVATE ${TOSU_VULKAN_INCLUDE})
target_link_libraries(overlay_test PRIVATE PkgConfig::SDL3)

# verifies the SDL_Event offsets hard-coded in src/input.c
add_executable(sdl_abi_check sdl_abi_check.c)
target_link_libraries(sdl_abi_check PRIVATE PkgConfig::SDL3)
