# cloud: cloud drive client built on top of nxkit_core.
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
        )
