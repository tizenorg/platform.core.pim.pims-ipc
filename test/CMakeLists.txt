CMAKE_MINIMUM_REQUIRED(VERSION 2.6)
PROJECT(pims_ipc_test C)

####################################################################
# Basic configuration                                              #
####################################################################
# Set ready to build
SET(EXT_LIBS_DIRS "")
SET(EXT_LIBS_DEFS "")
SET(EXT_LIBS_LDFLAGS "")

# Set external libraries
SET(EXT_LIBS
    E_GLIB
)

FOREACH(flag ${EXT_LIBS})
    SET(EXT_LIBS_DIRS       ${EXT_LIBS_DIRS}    ${${flag}_INCLUDE_DIRS})
    SET(EXT_LIBS_DEFS       ${EXT_LIBS_DEFS}    ${${flag}_CFLAGS_OTHER})
    SET(EXT_LIBS_LDFLAGS    ${EXT_LIBS_LDFLAGS} ${${flag}_LDFLAGS})
ENDFOREACH(flag)

####################################################################
# Build this project                                               #
####################################################################

# Set source
SET(SRCS
    ${CMAKE_CURRENT_SOURCE_DIR}/test.c
)

INCLUDE_DIRECTORIES(
    ${CMAKE_SOURCE_DIR}/include
    ${EXT_LIBS_DIRS}
)

ADD_DEFINITIONS(
    ${EXT_LIBS_DEFS}
)

ADD_EXECUTABLE(${PROJECT_NAME} ${SRCS})
TARGET_LINK_LIBRARIES(${PROJECT_NAME} pims-ipc ${EXT_LIBS_LDFLAGS})
INSTALL(TARGETS ${PROJECT_NAME} DESTINATION lib)

