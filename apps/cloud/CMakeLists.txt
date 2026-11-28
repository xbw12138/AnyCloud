# cloud: cloud drive client built on top of nxkit_core.
set(CLOUD_LIBS "")
set(CLOUD_INCLUDES "")

# Quark serves its cover art as WebP, which the bundled stb_image cannot
# decode; libwebp is used as a fallback when the texture loader rejects bytes.
find_library(WEBP_LIBRARY webp)
find_path(WEBP_INCLUDE_DIR webp/decode.h)
if(PLATFORM_SWITCH)
    # devkitPro portlibs are not always part of CMake's search roots.
    if(EXISTS ${DEVKITPRO}/portlibs/switch/lib/libwebp.a)
        set(WEBP_LIBRARY ${DEVKITPRO}/portlibs/switch/lib/libwebp.a)
    endif()
    if(EXISTS ${DEVKITPRO}/portlibs/switch/include/webp/decode.h)
        set(WEBP_INCLUDE_DIR ${DEVKITPRO}/portlibs/switch/include)
    endif()
endif()
if(WEBP_LIBRARY AND WEBP_INCLUDE_DIR)
    list(APPEND CLOUD_LIBS ${WEBP_LIBRARY})
    list(APPEND CLOUD_INCLUDES ${WEBP_INCLUDE_DIR})
    set(CLOUD_HAS_WEBP ON)
endif()

# zip extraction (store + deflate) is built on zlib.
find_library(ZLIB_LIBRARY z)
find_path(ZLIB_INCLUDE_DIR zlib.h)
if(PLATFORM_SWITCH)
    if(EXISTS ${DEVKITPRO}/portlibs/switch/lib/libz.a)
        set(ZLIB_LIBRARY ${DEVKITPRO}/portlibs/switch/lib/libz.a)
    endif()
    if(EXISTS ${DEVKITPRO}/portlibs/switch/include/zlib.h)
        set(ZLIB_INCLUDE_DIR ${DEVKITPRO}/portlibs/switch/include)
    endif()
endif()
if(ZLIB_LIBRARY AND ZLIB_INCLUDE_DIR)
    list(APPEND CLOUD_LIBS ${ZLIB_LIBRARY})
    list(APPEND CLOUD_INCLUDES ${ZLIB_INCLUDE_DIR})
endif()

# PDF rendering uses MuPDF when a port is available; without it the download
# page simply reports that PDF viewing is unsupported.
set(CLOUD_PDF_SOURCES "")
set(MUPDF_LIB "")
set(MUPDF_THIRD_LIB "")
set(MUPDF_INCLUDE_DIR "")
if(PLATFORM_SWITCH)
    if(EXISTS ${DEVKITPRO}/portlibs/switch/lib/libmupdf.a AND
       EXISTS ${DEVKITPRO}/portlibs/switch/lib/libmupdf-third.a AND
       EXISTS ${DEVKITPRO}/portlibs/switch/include/mupdf/fitz.h)
        set(MUPDF_LIB ${DEVKITPRO}/portlibs/switch/lib/libmupdf.a)
        set(MUPDF_THIRD_LIB ${DEVKITPRO}/portlibs/switch/lib/libmupdf-third.a)
        set(MUPDF_INCLUDE_DIR ${DEVKITPRO}/portlibs/switch/include)
    endif()
else()
    find_library(MUPDF_LIB mupdf)
    find_library(MUPDF_THIRD_LIB mupdf-third)
    find_path(MUPDF_INCLUDE_DIR mupdf/fitz.h)
endif()
if(MUPDF_LIB AND MUPDF_THIRD_LIB AND MUPDF_INCLUDE_DIR)
    # MuPDF is static and pulls in these codecs, so keep them after the
    # archive on the command line.
    set(MUPDF_DEPS "")
    foreach(dep freetype harfbuzz jpeg png16 bz2 z)
        find_library(MUPDF_DEP_${dep} ${dep})
        if(PLATFORM_SWITCH AND EXISTS
           ${DEVKITPRO}/portlibs/switch/lib/lib${dep}.a)
            set(MUPDF_DEP_${dep}
                ${DEVKITPRO}/portlibs/switch/lib/lib${dep}.a)
        endif()
        if(MUPDF_DEP_${dep})
            list(APPEND MUPDF_DEPS ${MUPDF_DEP_${dep}})
        endif()
    endforeach()
    # Link order matters: libmupdf-third.a bundles a HarfBuzz patched to
    # allocate through MuPDF's fz_* hooks. Listing the portlibs HarfBuzz first
    # keeps libass (mpv's subtitle renderer) on the normal one; otherwise
    # ass_renderer_init crashes in fz_calloc_no_throw with a null context.
    list(APPEND CLOUD_LIBS ${MUPDF_LIB} ${MUPDF_DEPS} ${MUPDF_THIRD_LIB})
    set(CLOUD_PDF_SOURCES source/pages/pdf_viewer.cpp)
    set(CLOUD_HAS_MUPDF ON)
endif()

if(PLATFORM_SWITCH AND BUILTIN_NSP)
    # nspmini: install the embedded NSP forwarder to the SD card at runtime.
    # Vendored under external/nspmini (lib + headers); install hacbrewpack to
    # build the forwarder itself (scripts/switch-forwarder).
    set(NSPMINI_DIR ${CMAKE_SOURCE_DIR}/external/nspmini)
    list(APPEND CLOUD_LIBS ${NSPMINI_DIR}/lib/libnsp.a)
    list(APPEND CLOUD_INCLUDES ${NSPMINI_DIR}/include)
    add_subdirectory(${CMAKE_SOURCE_DIR}/scripts/switch-forwarder
                     ${CMAKE_BINARY_DIR}/forwarder)
endif()

# SMB drives: libsmb2 speaks the protocol, the app's own socket server serves
# the localhost bridge that hands files to mpv / curl.
list(APPEND CLOUD_LIBS smb2)

nxkit_add_app(cloud
        TITLEID "0x010FF000FFFF0102"
        AUTHOR "xbw12138"
        DISPLAY_NAME "AnyCloud"
        ICON "${NXKIT_RESOURCES_DIR}/img/cloud-icon.jpg"
        SOURCES
        source/main.cpp
        source/main_activity.cpp
        source/update_check.cpp
        source/drive/aliyun_drive.cpp
        source/drive/baidu_drive.cpp
        source/drive/drive115.cpp
        source/drive/download_manager.cpp
        source/drive/drive_manager.cpp
        source/drive/drive_provider.cpp
        source/drive/quark_drive.cpp
        source/drive/recent_store.cpp
        source/drive/playback_store.cpp
        source/drive/smb_bridge.cpp
        source/drive/smb_drive.cpp
        source/drive/smb_session.cpp
        source/pages/login_page.cpp
        source/pages/smb_login_view.cpp
        source/pages/file_list_page.cpp
        source/pages/downloads_page.cpp
        source/pages/local_dir_page.cpp
        source/pages/local_file_picker.cpp
        source/pages/profile_page.cpp
        source/pages/http_log_page.cpp
        source/pages/info_page.cpp
        source/pages/player_page.cpp
        source/pages/image_viewer.cpp
        source/pages/text_reader.cpp
        source/ui/cloud_empty_view.cpp
        source/ui/popup.cpp
        source/util/book_text.cpp
        source/util/crypto.cpp
        source/util/file_upload.cpp
        source/util/text_encoding.cpp
        source/util/gbk_table.cpp
        source/util/zip_extract.cpp
        ${CLOUD_PDF_SOURCES}
        LIBS
        ${CLOUD_LIBS}
        )

if(CLOUD_HAS_WEBP)
    target_include_directories(cloud PRIVATE ${WEBP_INCLUDE_DIR})
    target_compile_definitions(cloud PRIVATE CLOUD_HAS_WEBP)
endif()

if(ZLIB_LIBRARY AND ZLIB_INCLUDE_DIR)
    target_include_directories(cloud PRIVATE ${ZLIB_INCLUDE_DIR})
endif()

if(CLOUD_HAS_MUPDF)
    target_include_directories(cloud PRIVATE ${MUPDF_INCLUDE_DIR})
    target_compile_definitions(cloud PRIVATE CLOUD_HAS_MUPDF)
endif()

if(PLATFORM_SWITCH AND BUILTIN_NSP)
    target_include_directories(cloud PRIVATE ${CLOUD_INCLUDES})
    target_compile_definitions(cloud PRIVATE BUILTIN_NSP)
    # Build (and embed) the forwarder NSP before packaging the NRO romfs.
    add_dependencies(cloud.nro nsp_forwarder.nsp)
endif()


