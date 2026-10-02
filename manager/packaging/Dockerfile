# The Linux build of atmt_manager (tools\build_manager.ps1 -Linux, and CI).
#
# Debian bookworm: glibc 2.36, older than every SteamOS 3.5+ release, so the binary runs on the
# Deck. SDL2 is built from third_party/SDL2 and linked statically; the X11/Wayland headers are only
# needed to compile it (SDL loads those libraries at run time). appimagetool turns the result into
# the AppImage.
FROM debian:bookworm

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential cmake ninja-build git ca-certificates curl file \
        libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxi-dev libxfixes-dev libxss-dev \
        libxkbcommon-dev libwayland-dev wayland-protocols libegl1-mesa-dev libgles2-mesa-dev \
        libgl1-mesa-dev libdrm-dev libgbm-dev libudev-dev libdbus-1-dev \
    && rm -rf /var/lib/apt/lists/*

# appimagetool (static runtime inside); run with --appimage-extract-and-run, so no FUSE is needed here
ARG APPIMAGETOOL=https://github.com/AppImage/appimagetool/releases/download/continuous/appimagetool-x86_64.AppImage
RUN curl -fsSL -o /usr/local/bin/appimagetool "$APPIMAGETOOL" && chmod +x /usr/local/bin/appimagetool

WORKDIR /src
