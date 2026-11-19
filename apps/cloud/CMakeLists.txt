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

nxkit_add_app(cloud
        TITLEID "0x010FF000FFFF0102"
        AUTHOR "xbw12138"
        DISPLAY_NAME "AnyCloud"
        ICON "${NXKIT_RESOURCES_DIR}/img/cloud-icon.jpg"
        SOURCES
        source/main.cpp
        source/main_activity.cpp
        source/drive/aliyun_drive.cpp
        source/drive/baidu_drive.cpp
        source/drive/drive115.cpp
        source/drive/download_manager.cpp
        source/drive/drive_manager.cpp
        source/drive/drive_provider.cpp
        source/drive/quark_drive.cpp
        source/drive/recent_store.cpp
        source/drive/playback_store.cpp
        source/pages/login_page.cpp
        source/pages/file_list_page.cpp
        source/pages/downloads_page.cpp
        source/pages/local_dir_page.cpp
        source/pages/profile_page.cpp
        source/pages/http_log_page.cpp
        source/pages/info_page.cpp
        source/pages/player_page.cpp
        source/pages/image_viewer.cpp
        source/ui/cloud_empty_view.cpp
        source/ui/popup.cpp
        source/util/crypto.cpp
        source/util/text_encoding.cpp
        source/util/gbk_table.cpp
        LIBS
        ${CLOUD_LIBS}
        )

if(CLOUD_HAS_WEBP)
    target_include_directories(cloud PRIVATE ${WEBP_INCLUDE_DIR})
    target_compile_definitions(cloud PRIVATE CLOUD_HAS_WEBP)
endif()

if(PLATFORM_SWITCH AND BUILTIN_NSP)
    target_include_directories(cloud PRIVATE ${CLOUD_INCLUDES})
    target_compile_definitions(cloud PRIVATE BUILTIN_NSP)
    # Build (and embed) the forwarder NSP before packaging the NRO romfs.
    add_dependencies(cloud.nro nsp_forwarder.nsp)
endif()


