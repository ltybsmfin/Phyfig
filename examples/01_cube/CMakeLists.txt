add_executable(vulkan_cube main.cpp)
target_compile_features(vulkan_cube PRIVATE cxx_std_17)
target_compile_definitions(vulkan_cube PRIVATE VK_USE_PLATFORM_WIN32_KHR WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
target_link_libraries(vulkan_cube PRIVATE Vulkan::Vulkan user32)
if(MSVC)
    target_compile_options(vulkan_cube PRIVATE /W4 /permissive- /utf-8)
endif()
set_target_properties(vulkan_cube PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/01_cube"
    VS_DEBUGGER_WORKING_DIRECTORY "$<TARGET_FILE_DIR:vulkan_cube>")

# Compile directly into this example's configuration-specific output directory.
# A dedicated target also recopies shaders when only a shader was modified.
set(shader_outputs)
foreach(shader cube.vert cube.frag)
    set(output "${CMAKE_BINARY_DIR}/bin/01_cube/$<CONFIG>/shaders/${shader}.spv")
    add_custom_command(OUTPUT "${output}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/bin/01_cube/$<CONFIG>/shaders"
        COMMAND Vulkan::glslc --target-env=vulkan1.3 "${CMAKE_CURRENT_SOURCE_DIR}/shaders/${shader}" -o "${output}"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/${shader}"
        VERBATIM)
    list(APPEND shader_outputs "${output}")
endforeach()
add_custom_target(cube_shaders DEPENDS ${shader_outputs})
add_dependencies(vulkan_cube cube_shaders)

if(BUILD_TESTING)
    add_executable(cube_math_test math_test.cpp)
    target_compile_features(cube_math_test PRIVATE cxx_std_17)
    if(MSVC)
        target_compile_options(cube_math_test PRIVATE /W4 /permissive- /utf-8)
    endif()
    add_test(NAME cube_math COMMAND cube_math_test)
    add_test(NAME cube_smoke COMMAND vulkan_cube --smoke-test)
    set_tests_properties(cube_smoke PROPERTIES TIMEOUT 45
        ENVIRONMENT "VK_LAYER_VALIDATE_SYNC=1")
endif()

