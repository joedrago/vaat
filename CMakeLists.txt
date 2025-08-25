cmake_minimum_required(VERSION 3.15)
project(vaat)

# sudo apt update
# sudo apt install build-essential rsync ninja-build cmake libwayland-dev libegl-dev libgles-dev libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev gstreamer1.0-gl
# mkdir build && cd build && cmake -G Ninja .. && ninja

add_executable(vaat
    app.c
    gfx.c
    player.c
    util.c

    viewporter-protocol.c
    xdg-shell-protocol.c
)

target_include_directories(vaat
    SYSTEM PUBLIC
    /usr/include/gstreamer-1.0
    /usr/include/glib-2.0
    /usr/lib/aarch64-linux-gnu/glib-2.0/include
    /usr/lib/aarch64-linux-gnu/gstreamer-1.0/include
)

target_link_libraries(vaat
    EGL
    GLESv2
    glib-2.0
    gobject-2.0
    gstreamer-1.0

    gstallocators-1.0
    gstapp-1.0
    gstgl-1.0
    gstvideo-1.0

    wayland-client
    wayland-egl
    wayland-server
)
