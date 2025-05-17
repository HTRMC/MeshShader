cmake_minimum_required(VERSION 3.10)
project(MeshShaderNetworkGame)

set(CMAKE_CXX_STANDARD 17)

# Find packages
find_package(Vulkan REQUIRED)
find_package(glfw3 REQUIRED)
find_package(glm REQUIRED)

# Find glslangValidator for shader compilation
find_program(GLSLANG_VALIDATOR glslangValidator HINTS $ENV{VULKAN_SDK}/bin)

# Shader compilation function
function(compile_shader TARGET SHADER_FILE SHADER_TYPE OUTPUT_FILE)
    add_custom_command(
            OUTPUT ${OUTPUT_FILE}
            COMMAND ${GLSLANG_VALIDATOR} -V ${SHADER_FILE} -S ${SHADER_TYPE} -o ${OUTPUT_FILE} --target-env vulkan1.3
            DEPENDS ${SHADER_FILE}
            COMMENT "Compiling ${SHADER_TYPE} shader ${SHADER_FILE}"
    )
    add_custom_target(${TARGET} DEPENDS ${OUTPUT_FILE})
endfunction()

# Check for thread support
find_package(Threads REQUIRED)

# Create a directory for shader output
file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/shaders)

# Compile shaders
compile_shader(
        task_shader
        ${CMAKE_CURRENT_SOURCE_DIR}/shaders/task_shader.glsl
        "task"
        ${CMAKE_CURRENT_BINARY_DIR}/shaders/task_shader.spv
)

compile_shader(
        mesh_shader
        ${CMAKE_CURRENT_SOURCE_DIR}/shaders/mesh_shader.glsl
        "mesh"
        ${CMAKE_CURRENT_BINARY_DIR}/shaders/mesh_shader.spv
)

compile_shader(
        fragment_shader
        ${CMAKE_CURRENT_SOURCE_DIR}/shaders/fragment_shader.glsl
        "frag"
        ${CMAKE_CURRENT_BINARY_DIR}/shaders/fragment_shader.spv
)

# Include directories
include_directories(${Vulkan_INCLUDE_DIRS})

# Common source files
set(COMMON_SOURCES
        game_protocol.cpp
)

# Game server executable
add_executable(game_server ${COMMON_SOURCES} game_server.cpp)
target_link_libraries(game_server PRIVATE Threads::Threads)

# Game client executable
add_executable(game_client ${COMMON_SOURCES} game_client.cpp)
target_link_libraries(game_client PRIVATE
        ${Vulkan_LIBRARIES}
        glfw
        Threads::Threads
)

# Add shader dependencies to client
add_dependencies(game_client task_shader mesh_shader fragment_shader)

# For Windows, we need to link against the Winsock library
if(WIN32)
    target_link_libraries(game_server PRIVATE wsock32 ws2_32)
    target_link_libraries(game_client PRIVATE wsock32 ws2_32)
endif()