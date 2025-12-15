cmake_minimum_required(VERSION 3.16)
project(local_shadertoy2 CXX)

add_executable(local_shadertoy2
  main.cpp
  App.cpp
  App.hpp
)

target_link_libraries(local_shadertoy2
  PRIVATE glfw etna glm::glm wsi gui
  PRIVATE render_utils scene
)

target_include_directories(local_shadertoy2 PRIVATE
  ${CMAKE_SOURCE_DIR}/common
)

find_program(GLSLC glslc REQUIRED)

set(SHADER_OUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/shaders)
file(MAKE_DIRECTORY ${SHADER_OUT_DIR})

set(SHADERS
  ${CMAKE_CURRENT_SOURCE_DIR}/shaders/fullscreen.vert
  ${CMAKE_CURRENT_SOURCE_DIR}/shaders/proc.frag
  ${CMAKE_CURRENT_SOURCE_DIR}/shaders/toy.frag
)

set(SPVS "")
foreach(S IN LISTS SHADERS)
  get_filename_component(NAME ${S} NAME)
  set(OUT ${SHADER_OUT_DIR}/${NAME}.spv)

  if(NAME MATCHES ".*\\.vert")
    set(STAGE vert)
  elseif(NAME MATCHES ".*\\.frag")
    set(STAGE frag)
  else()
    message(FATAL_ERROR "Unknown shader stage for ${NAME}")
  endif()

  add_custom_command(
    OUTPUT ${OUT}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${SHADER_OUT_DIR}
    COMMAND ${GLSLC} --target-env=vulkan1.2 -fshader-stage=${STAGE} ${S} -o ${OUT}
    DEPENDS ${S}
    VERBATIM
  )
  list(APPEND SPVS ${OUT})
endforeach()

add_custom_target(local_shadertoy2_shaders DEPENDS ${SPVS})
add_dependencies(local_shadertoy2 local_shadertoy2_shaders)

add_custom_command(TARGET local_shadertoy2 POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:local_shadertoy2>/shaders
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          ${SPVS}
          $<TARGET_FILE_DIR:local_shadertoy2>/shaders
)

target_compile_definitions(local_shadertoy2 PRIVATE
  LOCAL_SHADERTOY2_SHADERS_ROOT="${CMAKE_CURRENT_BINARY_DIR}/shaders/"
  LOCAL_SHADERTOY2_ASSETS_ROOT="${CMAKE_SOURCE_DIR}/resources/"
)
