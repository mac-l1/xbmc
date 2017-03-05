#.rst:
# FindROCKCHIP
# -----
# Finds the MPP library
#
# This will will define the following variables::
#
# ROCKCHIP_FOUND - system has MPP
# ROCKCHIP_INCLUDE_DIRS - the MPP include directory
# ROCKCHIP_LIBRARIES - the MPP libraries
# ROCKCHIP_DEFINITIONS - the MPP definitions
#
# and the following imported targets::
#
#   ROCKCHIP::ROCKCHIP    - The MPP library
#   ROCKCHIP::VPU - The MPP extension library

if(PKG_CONFIG_FOUND)
  pkg_check_modules(PC_ROCKCHIP rockchip_mpp rockchip_vpu QUIET)
endif()

find_path(ROCKCHIP_INCLUDE_DIR NAMES rockchip/vpu_api.h
                        PATHS ${PC_ROCKCHIP_rockchip_mpp_INCLUDEDIR})
find_library(ROCKCHIP_MPP_LIBRARY NAMES rockchip_mpp
                       PATHS ${PC_ROCKCHIP_rockchip_mpp_LIBDIR})
find_library(ROCKCHIP_VPU_LIBRARY NAMES rockchip_vpu
                           PATHS ${PC_ROCKCHIP_rockchip_vpu_LIBDIR})

set(ROCKCHIP_VERSION ${PC_ROCKCHIP_rockchip_mpp_VERSION})

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(ROCKCHIP
	                          REQUIRED_VARS ROCKCHIP_MPP_LIBRARY ROCKCHIP_VPU_LIBRARY ROCKCHIP_INCLUDE_DIR
				  VERSION_VAR ROCKCHIP_VERSION)

if(ROCKCHIP_FOUND)
  set(ROCKCHIP_LIBRARIES ${ROCKCHIP_MPP_LIBRARY} ${ROCKCHIP_VPU_LIBRARY})
  set(ROCKCHIP_INCLUDE_DIRS ${ROCKCHIP_INCLUDE_DIR})
  set(ROCKCHIP_DEFINITIONS -DHAVE_ROCKCHIP=1)

  if(NOT TARGET ROCKCHIP::MPP)
    add_library(ROCKCHIP::MPP UNKNOWN IMPORTED)
    set_target_properties(ROCKCHIP::MPP PROPERTIES
	                       IMPORTED_LOCATION "${ROCKCHIP_MPP_LIBRARY}"
                               INTERFACE_INCLUDE_DIRECTORIES "${ROCKCHIP_INCLUDE_DIR}"
			       INTERFACE_COMPILE_DEFINITIONS HAVE_ROCKCHIP=1)
  endif()
  if(NOT TARGET ROCKCHIP::VPU)
    add_library(ROCKCHIP::VPU UNKNOWN IMPORTED)
    set_target_properties(ROCKCHIP::VPU PROPERTIES
                                  IMPORTED_LOCATION "${ROCKCHIP_VPU_LIBRARY}"
                                  INTERFACE_INCLUDE_DIRECTORIES "${ROCKCHIP_INCLUDE_DIR}"
				  INTERFACE_LINK_LIBRARIES ROCKCHIP::MPP)
  endif()
endif()

mark_as_advanced(ROCKCHIP_INCLUDE_DIR ROCKCHIP_MPP_LIBRARY ROCKCHIP_VPU_LIBRARY)
