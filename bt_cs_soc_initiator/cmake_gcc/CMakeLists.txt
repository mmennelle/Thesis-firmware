# Define minimal required version of CMake.
cmake_minimum_required(VERSION "3.25")

# Project definition
project(
	bt_cs_soc_initiator
	VERSION 1.0
	LANGUAGES C CXX ASM
)

# Include the definition of the slc_bt_cs_soc_initiator target,
# which contains the content of the SLC project
include(bt_cs_soc_initiator.cmake)

add_executable(bt_cs_soc_initiator
    # Add additional sources here
)

target_include_directories(bt_cs_soc_initiator PUBLIC
    # Add additional include paths here
)

target_compile_definitions(bt_cs_soc_initiator PUBLIC
    # Add additional macros here
)

target_compile_options(bt_cs_soc_initiator PUBLIC
    # Set additional compiler flags here
)

target_link_options(bt_cs_soc_initiator PUBLIC
    # Set additional linker flags here
)

# Link with the content defined in the SLC project
target_link_libraries(bt_cs_soc_initiator PRIVATE
    slc
)

# Include managed project content if available
include(bt_cs_soc_initiator_project.cmake OPTIONAL RESULT_VARIABLE managed_project)
if(managed_project)
    message(STATUS "Using managed project content from ${managed_project}")
endif()

# Force the gcc linker command
set_target_properties(bt_cs_soc_initiator PROPERTIES LINKER_LANGUAGE C)

# Create .bin, .hex and .s37 artifacts after building the project
add_custom_command(TARGET bt_cs_soc_initiator
    POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} ${OBJCOPY_SREC_CMD} "$<TARGET_FILE:bt_cs_soc_initiator>" "$<TARGET_FILE_DIR:bt_cs_soc_initiator>/$<TARGET_FILE_BASE_NAME:bt_cs_soc_initiator>.s37"
    COMMAND ${CMAKE_OBJCOPY} ${OBJCOPY_IHEX_CMD} "$<TARGET_FILE:bt_cs_soc_initiator>" "$<TARGET_FILE_DIR:bt_cs_soc_initiator>/$<TARGET_FILE_BASE_NAME:bt_cs_soc_initiator>.hex"
    COMMAND ${CMAKE_OBJCOPY} ${OBJCOPY_BIN_CMD}  "$<TARGET_FILE:bt_cs_soc_initiator>" "$<TARGET_FILE_DIR:bt_cs_soc_initiator>/$<TARGET_FILE_BASE_NAME:bt_cs_soc_initiator>.bin" 
)

# Run post-build pipeline to perform additional post-processing
if(post_build_command)
add_custom_command(TARGET bt_cs_soc_initiator
    POST_BUILD
    WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}/..
    COMMAND ${post_build_command}
)
endif()