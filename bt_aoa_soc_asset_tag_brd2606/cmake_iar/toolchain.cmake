set(CMAKE_SYSTEM_NAME                   Generic)
set(CMAKE_SYSTEM_PROCESSOR              arm)
set(CMAKE_TRY_COMPILE_TARGET_TYPE       STATIC_LIBRARY)

if (WIN32)
  set(EXE_SUFFIX ".exe")
  cmake_path(SET USER_DIR "$ENV{USERPROFILE}" NORMALIZE)
else()
  set(EXE_SUFFIX "")
  cmake_path(SET USER_DIR "$ENV{HOME}" NORMALIZE)
endif ()

# Use the SLT tooling to find the install paths so the tools are
# found across multiple machines. This requires placing the 'slt'
# tool on PATH
execute_process(COMMAND slt where commander
                OUTPUT_VARIABLE "COMMANDER_SLT_PATH"
                OUTPUT_STRIP_TRAILING_WHITESPACE)

execute_process(COMMAND slt where ninja
                OUTPUT_VARIABLE "NINJA_SLT_PATH"
                OUTPUT_STRIP_TRAILING_WHITESPACE)

if (DEFINED ENV{IAR_ARM_DIR})
  set(TOOLCHAIN_DIR "$ENV{IAR_ARM_DIR}/arm/bin/")
elseif (WIN32)
  set(TOOLCHAIN_DIR "/arm/bin/")
elseif (APPLE)
  set(TOOLCHAIN_DIR "/arm/bin/")
else()
  set(TOOLCHAIN_DIR "/arm/bin/")
endif ()

if (DEFINED ENV{POST_BUILD_EXE})
  set(POST_BUILD_EXE "$ENV{POST_BUILD_EXE}")
elseif (COMMANDER_SLT_PATH)
  if (WIN32)
    set(POST_BUILD_EXE "${COMMANDER_SLT_PATH}/commander.exe")
  elseif (APPLE)
    set(POST_BUILD_EXE "${COMMANDER_SLT_PATH}/Contents/MacOS/commander")
  else()
    set(POST_BUILD_EXE "${COMMANDER_SLT_PATH}/commander")
  endif ()
elseif (WIN32)
  set(POST_BUILD_EXE "${USER_DIR}/.silabs/slt/installs/archive/Simplicity Commander/commander.exe")
elseif (APPLE)
  set(POST_BUILD_EXE "")
else()
  set(POST_BUILD_EXE "")
endif ()

if (DEFINED ENV{NINJA_EXE_PATH})
  set(NINJA_RUNTIME_PATH "$ENV{NINJA_EXE_PATH}")
elseif (NINJA_SLT_PATH)
  set(NINJA_RUNTIME_PATH "${NINJA_SLT_PATH}/ninja")
elseif (WIN32)
  set(NINJA_RUNTIME_PATH "${USER_DIR}/.silabs/slt/installs/conan/p/ninja1a38fc85adcf7/p/ninja.exe")
elseif (APPLE)
  set(NINJA_RUNTIME_PATH "")
else()
  set(NINJA_RUNTIME_PATH "")
endif ()
# Use default lookup mechanisms if the OS specific values are not set above
if (NINJA_RUNTIME_PATH)
	set(CMAKE_MAKE_PROGRAM ${NINJA_RUNTIME_PATH} CACHE FILEPATH "" FORCE)
endif ()

set(CMAKE_C_COMPILER    ${TOOLCHAIN_DIR}iccarm${EXE_SUFFIX})
set(CMAKE_CXX_COMPILER  ${TOOLCHAIN_DIR}iccarm${EXE_SUFFIX})
set(CMAKE_ASM_COMPILER  ${TOOLCHAIN_DIR}iasmarm${EXE_SUFFIX})
set(CMAKE_LINKER        ${TOOLCHAIN_DIR}ilinkarm${EXE_SUFFIX})
set(CMAKE_OBJCOPY       ${TOOLCHAIN_DIR}ielftool${EXE_SUFFIX})

set(OBJCOPY_SREC_CMD    "--srec")
set(OBJCOPY_IHEX_CMD    "--ihex")
set(OBJCOPY_BIN_CMD     "--bin")

set(CMAKE_C_STANDARD_REQUIRED   OFF)
set(CMAKE_CXX_STANDARD_REQUIRED OFF)
set(CMAKE_C_EXTENSIONS          OFF)

set(CMAKE_C_FLAGS_RELEASE               "" CACHE STRING "")
set(CMAKE_CXX_FLAGS_RELEASE             "" CACHE STRING "")

# Response file support
# IAR cannot consume Ninja Multi-Config generated response files at the moment 
# SET(CMAKE_C_USE_RESPONSE_FILE_FOR_OBJECTS   1)
# SET(CMAKE_CXX_USE_RESPONSE_FILE_FOR_OBJECTS 1)
# SET(CMAKE_NINJA_FORCE_RESPONSE_FILE         1 CACHE INTERNAL "")

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM   NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY   ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE   ONLY)

set(CMAKE_EXECUTABLE_SUFFIX     .out)
set(CMAKE_EXECUTABLE_SUFFIX_C   .out)
set(CMAKE_EXECUTABLE_SUFFIX_CXX .out)
