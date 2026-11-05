# cloud: cloud drive client built on top of nxkit_core.
set(CLOUD_LIBS "")
set(CLOUD_INCLUDES "")

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
        DISPLAY_NAME "夸克网盘"
        ICON "${NXKIT_RESOURCES_DIR}/img/cloud-icon.jpg"
        SOURCES
        source/main.cpp
        source/main_activity.cpp
        source/drive/drive_manager.cpp
        source/drive/drive_provider.cpp
        source/drive/quark_drive.cpp
        source/drive/recent_store.cpp
        source/drive/playback_store.cpp
        source/pages/login_page.cpp
        source/pages/file_list_page.cpp
        source/pages/profile_page.cpp
        source/pages/player_page.cpp
        source/pages/image_viewer.cpp
        source/ui/cloud_empty_view.cpp
        LIBS
        ${CLOUD_LIBS}
        )

if(PLATFORM_SWITCH AND BUILTIN_NSP)
    target_include_directories(cloud PRIVATE ${CLOUD_INCLUDES})
    target_compile_definitions(cloud PRIVATE BUILTIN_NSP)
    # Build (and embed) the forwarder NSP before packaging the NRO romfs.
    add_dependencies(cloud.nro nsp_forwarder.nsp)
endif()


