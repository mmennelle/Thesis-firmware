# Define minimal required version of CMake.
cmake_minimum_required(VERSION "3.25")

# Project definition
project(
	bt_aoa_ncp_locator
	VERSION 1.0
	LANGUAGES C CXX ASM
)

# Include the definition of the slc_bt_aoa_ncp_locator target,
# which contains the content of the SLC project
include(bt_aoa_ncp_locator.cmake)

add_executable(bt_aoa_ncp_locator
    # Add additional sources here
)

target_include_directories(bt_aoa_ncp_locator PUBLIC
    # Add additional include paths here
)

target_compile_definitions(bt_aoa_ncp_locator PUBLIC
    # Add additional macros here
)

target_compile_options(bt_aoa_ncp_locator PUBLIC
    # Set additional compiler flags here
)

target_link_options(bt_aoa_ncp_locator PUBLIC
    # Set additional linker flags here
)

# Link with the content defined in the SLC project
target_link_libraries(bt_aoa_ncp_locator PRIVATE
    slc
)

# Include managed project content if available
include(bt_aoa_ncp_locator_project.cmake OPTIONAL RESULT_VARIABLE managed_project)
if(managed_project)
    message(STATUS "Using managed project content from ${managed_project}")
endif()

# Force the gcc linker command
set_target_properties(bt_aoa_ncp_locator PROPERTIES LINKER_LANGUAGE C)

# Create .bin, .hex and .s37 artifacts after building the project
add_custom_command(TARGET bt_aoa_ncp_locator
    POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} ${OBJCOPY_SREC_CMD} "$<TARGET_FILE:bt_aoa_ncp_locator>" "$<TARGET_FILE_DIR:bt_aoa_ncp_locator>/$<TARGET_FILE_BASE_NAME:bt_aoa_ncp_locator>.s37"
    COMMAND ${CMAKE_OBJCOPY} ${OBJCOPY_IHEX_CMD} "$<TARGET_FILE:bt_aoa_ncp_locator>" "$<TARGET_FILE_DIR:bt_aoa_ncp_locator>/$<TARGET_FILE_BASE_NAME:bt_aoa_ncp_locator>.hex"
    COMMAND ${CMAKE_OBJCOPY} ${OBJCOPY_BIN_CMD}  "$<TARGET_FILE:bt_aoa_ncp_locator>" "$<TARGET_FILE_DIR:bt_aoa_ncp_locator>/$<TARGET_FILE_BASE_NAME:bt_aoa_ncp_locator>.bin" 
)

# Run post-build pipeline to perform additional post-processing
if(post_build_command)
add_custom_command(TARGET bt_aoa_ncp_locator
    POST_BUILD
    WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}/..
    COMMAND ${post_build_command}
)
endif()