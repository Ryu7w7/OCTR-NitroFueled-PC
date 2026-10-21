cmake_minimum_required(VERSION 3.5)

# This has to be initialized before the project() command appears
# Set the default of CMAKE_BUILD_TYPE to be release, unless user specifies with -D.  MSVC_IDE does not use CMAKE_BUILD_TYPE
if( NOT MSVC_IDE AND NOT CMAKE_BUILD_TYPE )
    set( CMAKE_BUILD_TYPE Release CACHE STRING "Choose the type of build, options are: None Debug Release RelWithDebInfo MinSizeRel." )
endif()

# Default installation path
if(WIN32)
    set(CMAKE_INSTALL_PREFIX "/opt/rocm/x86_64-w64-mingw32" CACHE PATH "")
else()
    set(CMAKE_INSTALL_PREFIX "/opt/rocm" CACHE PATH "")
endif()

project(half)
find_package(ROCM REQUIRED)

include(ROCMSetupVersion)
include(ROCMCreatePackage)

# FOR HANDLING ENABLE/DISABLE OPTIONAL BACKWARD COMPATIBILITY for FILE/FOLDER REORG
option(BUILD_FILE_REORG_BACKWARD_COMPATIBILITY "Build with file/folder reorg with backward compatibility enabled" OFF)

rocm_setup_version(VERSION 1.12.0 NO_GIT_TAG_VERSION)

install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/include/half.hpp DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/half)

if(BUILD_FILE_REORG_BACKWARD_COMPATIBILITY)
  include(ROCMHeaderWrapper)
  rocm_wrap_header_file(
    half.hpp
    GUARDS WRAPPER
    WRAPPER_LOCATIONS ${CMAKE_INSTALL_INCLUDEDIR}
  )

  #Install Wrapper for backward compatibility
  install(FILES ${CMAKE_CURRENT_BINARY_DIR}/${CMAKE_INSTALL_INCLUDEDIR}/half.hpp DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
endif()

rocm_create_package(
    NAME half
    DESCRIPTION "HALF-PRECISION FLOATING POINT LIBRARY"
    MAINTAINER "Paul Fultz II <paul.fultz@amd.com>"
)
