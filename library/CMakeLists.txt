cmake_minimum_required(VERSION 3.15)

if (_USE_SHARED_LIB)
    find_package(cpr REQUIRED GLOBAL)

    find_package(qrcodegencpp REQUIRED GLOBAL)
    add_library(qrcode ALIAS qrcodegencpp::qrcodegencpp)

    find_path(PYSTRING_INCLUDE_DIR pystring.h PATH_SUFFIXES pystring REQUIRED)
    find_library(PYSTRING_LIBRARY NAMES pystring REQUIRED)
    list(APPEND PLATFORM_INCLUDES ${PYSTRING_INCLUDE_DIR})
    list(APPEND PLATFORM_LIBS ${PYSTRING_LIBRARY})

    find_package(PkgConfig REQUIRED)
    pkg_check_modules(opencc REQUIRED IMPORTED_TARGET opencc)
    add_library(libopencc ALIAS PkgConfig::opencc)

    set(PLATFORM_LIBS
        ${PLATFORM_LIBS}
        PARENT_SCOPE
    )
    set(PLATFORM_INCLUDES
        ${PLATFORM_INCLUDES}
        PARENT_SCOPE
    )
else ()
    # add cpr
    if (PLATFORM_DESKTOP)
        if (APPLE)
            set(CPR_USE_SYSTEM_CURL ON)
            set(CPR_FORCE_DARWINSSL_BACKEND ON)
        endif ()
        if (WIN32)
            set(CPR_USE_SYSTEM_CURL OFF)
            set(CPR_FORCE_WINSSL_BACKEND ON)
        endif ()
    else ()
        set(CPR_USE_SYSTEM_CURL ON)
        set(CPR_FORCE_MBEDTLS_BACKEND ON)
    endif ()
    set(BUILD_SHARED_LIBS OFF)
    add_subdirectory(cpr EXCLUDE_FROM_ALL)

    # add libopencc
    set(BUILD_SHARED_LIBS OFF)
    set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)
    add_subdirectory(OpenCC EXCLUDE_FROM_ALL)
    target_compile_options(libopencc PRIVATE -w)

    # add QR-Code-generator
    add_library(
        qrcode STATIC
        ${CMAKE_CURRENT_SOURCE_DIR}/QR-Code-generator/cpp/qrcodegen.cpp
    )
    target_include_directories(
        qrcode PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/QR-Code-generator/cpp
    )
    set_property(TARGET qrcode PROPERTY CXX_STANDARD 17)

    # add pystring
    add_library(
        pystring STATIC ${CMAKE_CURRENT_SOURCE_DIR}/pystring/pystring.cpp
    )
    target_include_directories(
        pystring PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/pystring
    )
    set_property(TARGET pystring PROPERTY CXX_STANDARD 17)
endif ()

# add quickjs (mini program script runtime)
add_subdirectory(quickjs EXCLUDE_FROM_ALL)

# add yoga (flex layout for the mini program renderer)
add_subdirectory(yoga EXCLUDE_FROM_ALL)

# add lunasvg
set(BUILD_SHARED_LIBS OFF)
set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)
add_subdirectory(lunasvg EXCLUDE_FROM_ALL)

# add mongoose
add_subdirectory(mongoose EXCLUDE_FROM_ALL)

# add libsmb2 (SMB2/CIFS client, used by the cloud app's SMB drive).
# Kerberos/GSSAPI/DCE-RPC are not needed for plain NTLM or guest shares.
set(ENABLE_EXAMPLES OFF CACHE BOOL "" FORCE)
set(ENABLE_UTILS OFF CACHE BOOL "" FORCE)
set(ENABLE_LIBKRB5 OFF CACHE BOOL "" FORCE)
set(ENABLE_GSSAPI OFF CACHE BOOL "" FORCE)
set(ENABLE_LIBDCERPC OFF CACHE BOOL "" FORCE)
set(ENABLE_OPENSSL OFF CACHE BOOL "" FORCE)
set(ENABLE_LIBQUIC OFF CACHE BOOL "" FORCE)
set(ENABLE_RDMA OFF CACHE BOOL "" FORCE)
add_subdirectory(libsmb2 EXCLUDE_FROM_ALL)

# libsmb2's Switch branch is guarded by __SWITCH__, which the rest of the tree
# only receives through borealis' public compile options.
if (PLATFORM_SWITCH)
    target_compile_definitions(smb2 PRIVATE __SWITCH__)
endif ()

# add borealis
set(BRLS_USE_OPENCC ON)
if (_USE_SHARED_LIB)
    set(BRLS_OPENCC_INCLUDE ${opencc_INCLUDE_DIRS})
else ()
    set(BRLS_OPENCC_INCLUDE ${CMAKE_CURRENT_SOURCE_DIR}/OpenCC/src)
endif ()
add_subdirectory(borealis/library)
