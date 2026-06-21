# Define minimal required version of CMake.
cmake_minimum_required(VERSION "3.25")

# Project definition
project(
	bt_aoa_soc_asset_tag
	VERSION 1.0
	LANGUAGES C CXX ASM
)

# Include the definition of the slc_bt_aoa_soc_asset_tag target,
# which contains the content of the SLC project
include(bt_aoa_soc_asset_tag.cmake)

add_executable(bt_aoa_soc_asset_tag
    # Add additional sources here
    ../mpu6050_glue.c
    ../third_party/mpu6050/src/driver_mpu6050.c
    ../third_party/mpu6050/example/driver_mpu6050_dmp.c
)

target_include_directories(bt_aoa_soc_asset_tag PUBLIC
    # Add additional include paths here
    ..
    ../third_party/mpu6050/src
    ../third_party/mpu6050/interface
    ../third_party/mpu6050/example
)

# app.c and mpu6050_glue.c are compiled as part of the generated `slc`
# library, so the libdriver headers must also be visible there.
target_include_directories(slc PUBLIC
    ../third_party/mpu6050/src
    ../third_party/mpu6050/interface
    ../third_party/mpu6050/example
)

target_compile_definitions(bt_aoa_soc_asset_tag PUBLIC
    # Add additional macros here
)

target_compile_options(bt_aoa_soc_asset_tag PUBLIC
    # Set additional compiler flags here
)

target_link_options(bt_aoa_soc_asset_tag PUBLIC
    # Set additional linker flags here
)

# Link with the content defined in the SLC project
target_link_libraries(bt_aoa_soc_asset_tag PRIVATE
    slc
)

# Include managed project content if available
include(bt_aoa_soc_asset_tag_project.cmake OPTIONAL RESULT_VARIABLE managed_project)
if(managed_project)
    message(STATUS "Using managed project content from ${managed_project}")
endif()

# Force the gcc linker command
set_target_properties(bt_aoa_soc_asset_tag PROPERTIES LINKER_LANGUAGE C)

# Create .bin, .hex and .s37 artifacts after building the project
add_custom_command(TARGET bt_aoa_soc_asset_tag
    POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} ${OBJCOPY_SREC_CMD} "$<TARGET_FILE:bt_aoa_soc_asset_tag>" "$<TARGET_FILE_DIR:bt_aoa_soc_asset_tag>/$<TARGET_FILE_BASE_NAME:bt_aoa_soc_asset_tag>.s37"
    COMMAND ${CMAKE_OBJCOPY} ${OBJCOPY_IHEX_CMD} "$<TARGET_FILE:bt_aoa_soc_asset_tag>" "$<TARGET_FILE_DIR:bt_aoa_soc_asset_tag>/$<TARGET_FILE_BASE_NAME:bt_aoa_soc_asset_tag>.hex"
    COMMAND ${CMAKE_OBJCOPY} ${OBJCOPY_BIN_CMD}  "$<TARGET_FILE:bt_aoa_soc_asset_tag>" "$<TARGET_FILE_DIR:bt_aoa_soc_asset_tag>/$<TARGET_FILE_BASE_NAME:bt_aoa_soc_asset_tag>.bin" 
)

# Run post-build pipeline to perform additional post-processing
if(post_build_command)
add_custom_command(TARGET bt_aoa_soc_asset_tag
    POST_BUILD
    WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}/..
    COMMAND ${post_build_command}
)
endif()