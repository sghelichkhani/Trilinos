# Copyright(C) 1999-2021 National Technology & Engineering Solutions
# of Sandia, LLC (NTESS).  Under the terms of Contract DE-NA0003525 with
# NTESS, the U.S. Government retains certain rights in this software.
#
# See packages/seacas/LICENSE for details

CMAKE_MINIMUM_REQUIRED(VERSION 3.8.0)

PROJECT(CatalystIossAdapter)

SET(CMAKE_CXX_STANDARD 14)

SET(PVCSA_VERSION CACHE STRING "Version string to append to catalyst adapter library.")

IF(PVCSA_VERSION)
 SET(PVCSA_LIBRARY_NAME catalystioss_${PVCSA_VERSION})
ELSE()
 SET(PVCSA_LIBRARY_NAME catalystioss)
ENDIF()

FIND_PACKAGE(ParaView REQUIRED)

SET(PARAVIEW_PYTHON_LIBRARY_RPATH_DIRECTORY CACHE
    FILEPATH "Path to the directory containing the Python library used by ParaView.")

IF(PARAVIEW_PYTHON_LIBRARY_RPATH_DIRECTORY)
  CMAKE_POLICY(SET CMP0015 OLD)
  LINK_DIRECTORIES(${PARAVIEW_PYTHON_LIBRARY_RPATH_DIRECTORY})
ENDIF()

SET(CMAKE_SKIP_BUILD_RPATH FALSE)
SET(CMAKE_BUILD_WITH_INSTALL_RPATH FALSE)
SET(CMAKE_INSTALL_RPATH "${CMAKE_INSTALL_PREFIX}/lib" ${PARAVIEW_PYTHON_LIBRARY_RPATH_DIRECTORY})
SET(CMAKE_INSTALL_RPATH_USE_LINK_PATH TRUE)
LIST(FIND CMAKE_PLATFORM_IMPLICIT_LINK_DIRECTORIES "${CMAKE_INSTALL_PREFIX}/lib" isSystemDir)
IF("${isSystemDir}" STREQUAL "-1")
   SET(CMAKE_INSTALL_RPATH "${CMAKE_INSTALL_PREFIX}/lib")
ENDIF()

INCLUDE_DIRECTORIES("manager")
INCLUDE_DIRECTORIES("parser")
INCLUDE_DIRECTORIES("../../")

ADD_SUBDIRECTORY(manager)
ADD_SUBDIRECTORY(parser)
ADD_LIBRARY(${PVCSA_LIBRARY_NAME} SHARED manager/CatalystManager.cxx
                                         manager/CatalystMeshWriter.cxx
                                         exodus/CatalystExodusMesh.cxx
                                         cgns/CatalystCGNSMesh.cxx)

TARGET_INCLUDE_DIRECTORIES(${PVCSA_LIBRARY_NAME}
    PUBLIC
        ${CMAKE_CURRENT_BINARY_DIR}/manager)

TARGET_LINK_LIBRARIES(${PVCSA_LIBRARY_NAME}
    INTERFACE
      VTK::PythonUsed
    PRIVATE
      phactori_parser
      ParaView::PythonCatalyst)

OPTION(BUILD_IOSS_2_CATALYST_APPLICATION
    "Build ioss2catalyst application" OFF)

IF(BUILD_IOSS_2_CATALYST_APPLICATION)
    FILE(COPY phactori DESTINATION ${CMAKE_CURRENT_BINARY_DIR})
    SET(SEACAS_INSTALL_DIR CACHE PATH
        "Path to SEACAS build installation directory")

    SET(CMAKE_PREFIX_PATH ${SEACAS_INSTALL_DIR} ${CMAKE_PREFIX_PATH})
    FIND_PACKAGE(SEACAS REQUIRED)
    FIND_PACKAGE(HDF5 REQUIRED)

    IF(SEACAS_FOUND)
        IF("CGNS" IN_LIST SEACAS_TPL_LIST)
            ADD_SUBDIRECTORY(ioss2catalyst)
            ENABLE_TESTING()
            ADD_SUBDIRECTORY(test)
        ELSE()
            MESSAGE(FATAL_ERROR "Must build SEACAS with CGNS TPL enabled.")
        ENDIF()
    ENDIF()
ENDIF()

INSTALL(TARGETS ${PVCSA_LIBRARY_NAME} LIBRARY DESTINATION lib)
INSTALL(DIRECTORY phactori DESTINATION .)
