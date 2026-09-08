set(imgui_dir "${PROJECT_SOURCE_DIR}/third_party/imgui")
file(SHA256 "${PROJECT_SOURCE_DIR}/assets/formulas/formulas.json" formula_source_hash)
file(STRINGS "${PROJECT_SOURCE_DIR}/assets/formulas/formula_cache.hpp" formula_cached_hash REGEX "sourceHash =")
if(NOT formula_cached_hash MATCHES "${formula_source_hash}")
    message(FATAL_ERROR "LaTeX formula cache is stale. Run assets/formulas/generate.py before rebuilding.")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${PROJECT_SOURCE_DIR}/assets/formulas/formulas.json"
    "${PROJECT_SOURCE_DIR}/assets/formulas/formula_cache.hpp")
add_library(physics_imgui STATIC
    "${imgui_dir}/imgui.cpp" "${imgui_dir}/imgui_draw.cpp"
    "${imgui_dir}/imgui_tables.cpp" "${imgui_dir}/imgui_widgets.cpp"
    "${imgui_dir}/backends/imgui_impl_win32.cpp" "${imgui_dir}/backends/imgui_impl_vulkan.cpp")
target_include_directories(physics_imgui PUBLIC "${imgui_dir}" "${imgui_dir}/backends")
target_compile_features(physics_imgui PUBLIC cxx_std_17)
target_link_libraries(physics_imgui PUBLIC Vulkan::Vulkan user32 gdi32 dwmapi)
target_compile_definitions(physics_imgui PRIVATE VK_USE_PLATFORM_WIN32_KHR NOMINMAX WIN32_LEAN_AND_MEAN)
add_library(physics_vulkan_host STATIC vulkan_host.cpp)
target_include_directories(physics_vulkan_host PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}" "${PROJECT_SOURCE_DIR}/src/math" "${PROJECT_SOURCE_DIR}/assets")
target_compile_features(physics_vulkan_host PUBLIC cxx_std_17)
target_compile_definitions(physics_vulkan_host PRIVATE VK_USE_PLATFORM_WIN32_KHR WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
target_link_libraries(physics_vulkan_host PUBLIC physics_imgui)
if(MSVC)
    target_compile_options(physics_vulkan_host PRIVATE /W4 /permissive- /utf-8)
endif()
