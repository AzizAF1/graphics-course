cmake_minimum_required(VERSION 3.16)
project(local_shadertoy1 CXX)

add_executable(local_shadertoy1
  main.cpp
  App.cpp
  App.hpp
)

target_link_libraries(local_shadertoy1
  PRIVATE glfw etna glm::glm wsi gui
)

# ---- Shader build: toy.comp -> toy.comp.spv ----
find_program(GLSLC glslc REQUIRED)

set(SHADER_SRC ${CMAKE_CURRENT_SOURCE_DIR}/shaders/toy.comp)
set(SHADER_OUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/shaders)
set(SHADER_SPV ${SHADER_OUT_DIR}/toy.comp.spv)

add_custom_command(
  OUTPUT ${SHADER_SPV}
  COMMAND ${CMAKE_COMMAND} -E make_directory ${SHADER_OUT_DIR}
  COMMAND ${GLSLC} --target-env=vulkan1.2 -fshader-stage=compute
          ${SHADER_SRC}
          -o ${SHADER_SPV}
  DEPENDS ${SHADER_SRC}
  VERBATIM
)

add_custom_target(local_shadertoy1_shaders DEPENDS ${SHADER_SPV})
add_dependencies(local_shadertoy1 local_shadertoy1_shaders)

# Copy shader next to executable: <exe_dir>/shaders/toy.comp.spv
add_custom_command(TARGET local_shadertoy1 POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:local_shadertoy1>/shaders
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          ${SHADER_SPV}
          $<TARGET_FILE_DIR:local_shadertoy1>/shaders/toy.comp.spv
)

target_include_directories(local_shadertoy1 PRIVATE
  ${CMAKE_SOURCE_DIR}/common
)

target_compile_definitions(local_shadertoy1 PRIVATE
  LOCAL_SHADERTOY1_SHADERS_ROOT="${CMAKE_CURRENT_BINARY_DIR}/shaders/"
)

