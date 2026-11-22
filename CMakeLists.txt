cmake_minimum_required(VERSION 3.16)
project(rtw88_native_module NONE)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "FreeBSD")
  message(FATAL_ERROR "The native RTW88 kernel module build requires FreeBSD. Configure this project on FreeBSD.")
endif()

set(FREEBSD_SRCROOT "/usr/src" CACHE PATH "Path to the FreeBSD source tree")
set(KMOD_NAME "if_rtw88_native")
set(KMOD_OUTPUT "${CMAKE_BINARY_DIR}/${KMOD_NAME}.ko")

file(GLOB_RECURSE RTW88_DRIVER_INPUTS CONFIGURE_DEPENDS
  "${CMAKE_SOURCE_DIR}/sys/dev/rtw88_native/*"
  "${CMAKE_SOURCE_DIR}/sys/modules/rtw88_native/*"
)

find_program(FREEBSD_MAKE NAMES make REQUIRED)

add_custom_command(
  OUTPUT "${KMOD_OUTPUT}"
  COMMAND "${CMAKE_COMMAND}" -E env
          "SRCROOT=${FREEBSD_SRCROOT}"
          "OUTDIR=${CMAKE_BINARY_DIR}"
          sh "${CMAKE_SOURCE_DIR}/tools/build-native-module.sh"
  DEPENDS
    "${CMAKE_SOURCE_DIR}/tools/build-native-module.sh"
    "${CMAKE_SOURCE_DIR}/sys/modules/rtw88_native/Makefile"
    ${RTW88_DRIVER_INPUTS}
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
  VERBATIM
  COMMENT "Building native RTW88 kernel module with FreeBSD kmod make system"
)

add_custom_target(rtw88_native ALL DEPENDS "${KMOD_OUTPUT}")
install(FILES "${KMOD_OUTPUT}" DESTINATION lib)
