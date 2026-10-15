#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Adam DePrince
set -eu

repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
dependency_prefix=${GOBLIN_DEPENDENCY_PREFIX:-"$repo_dir/build/deps"}
dependency_work=${GOBLIN_DEPENDENCY_WORK:-"$repo_dir/build/deps-work"}
build_jobs=${BUILD_JOBS:-$(getconf _NPROCESSORS_ONLN)}
mkdir -p "$dependency_prefix" "$dependency_work"

if [ "${GOBLIN_BUNDLE_OPENSSL:-0}" = 1 ]; then
    openssl_source="$repo_dir/vendor/openssl"
    if [ ! -d "$openssl_source" ]; then
        openssl_source="$dependency_work/openssl"
        if [ ! -d "$openssl_source/.git" ]; then
            git clone --depth 1 --branch openssl-3.6.5 https://github.com/openssl/openssl.git "$openssl_source"
        fi
        test "$(git -C "$openssl_source" rev-parse HEAD)" = c8bd5a57108599ac650bbae77fcabe3109dab2e8
    fi
    mkdir -p "$dependency_work/openssl-build"
    (
        cd "$dependency_work/openssl-build"
        # Generic C supports Kylin's old-world LoongArch ABI and GCC 8 assembler.
        # Built-in providers avoid any runtime module path outside the package.
        perl "$openssl_source/Configure" linux-generic64 shared no-module no-asm \
            --prefix="$dependency_prefix" --libdir=lib --openssldir=/etc/ssl
        make -j "$build_jobs"
        make test TESTS='test_evp test_sslapi test_verify'
        make install_sw
    )
    PKG_CONFIG_PATH="$dependency_prefix/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
    CMAKE_PREFIX_PATH="$dependency_prefix${CMAKE_PREFIX_PATH:+;$CMAKE_PREFIX_PATH}"
    export PKG_CONFIG_PATH CMAKE_PREFIX_PATH
fi
pkg-config --atleast-version=3.0 openssl || {
    echo 'Release builds require OpenSSL 3 or newer; use GOBLIN_BUNDLE_OPENSSL=1 on older Linux systems.' >&2
    exit 1
}

if [ -d "$repo_dir/vendor/freerdp" ] && [ -d "$repo_dir/vendor/libvncserver" ]; then
    freerdp_source="$repo_dir/vendor/freerdp"
    vnc_source="$repo_dir/vendor/libvncserver"
else
    if [ ! -d "$dependency_work/freerdp/.git" ]; then
        git clone --depth 1 --branch 3.17.2 https://github.com/FreeRDP/FreeRDP.git "$dependency_work/freerdp"
    fi
    if [ ! -d "$dependency_work/libvncserver/.git" ]; then
        git clone --depth 1 --branch LibVNCServer-0.9.15 https://github.com/LibVNC/libvncserver.git "$dependency_work/libvncserver"
    fi
    freerdp_source="$dependency_work/freerdp"
    vnc_source="$dependency_work/libvncserver"
    test "$(git -C "$freerdp_source" rev-parse HEAD)" = 955ad49545919c35ed731bc7716b26ddf888833a
    test "$(git -C "$vnc_source" rev-parse HEAD)" = 9b54b1ec32731bd23158ca014dc18014db4194c3
fi

cmake -S "$freerdp_source" -B "$dependency_work/freerdp-build" \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$dependency_prefix" \
    -DCMAKE_INSTALL_RPATH="$dependency_prefix/lib" \
    -DCMAKE_PREFIX_PATH="${CMAKE_PREFIX_PATH:-}" -DCMAKE_EXPORT_NO_PACKAGE_REGISTRY=ON \
    -DWITH_CLIENT=OFF -DWITH_SERVER=OFF -DWITH_CLIENT_COMMON=OFF \
    -DWITH_CHANNELS=OFF -DWITH_X11=OFF -DWITH_WAYLAND=OFF \
    -DWITH_FFMPEG=OFF -DWITH_DSP_FFMPEG=OFF -DWITH_VIDEO_FFMPEG=OFF \
    -DWITH_SWSCALE=OFF -DWITH_CUPS=OFF -DWITH_PCSC=OFF -DWITH_KRB5=OFF \
    -DWITH_WINPR_TOOLS=OFF -DWITH_SAMPLE=OFF -DWITH_CLIENT_SDL=OFF \
    -DWITH_OPENH264=OFF -DWITH_AAD=OFF -DWITH_JSON_DISABLED=ON -DWITH_OPUS=OFF \
    -DWITH_PKCS11=OFF -DWITH_SOXR=OFF -DWITH_JPEG=OFF -DBUILD_TESTING=OFF
cmake --build "$dependency_work/freerdp-build" -j "$build_jobs"
cmake --install "$dependency_work/freerdp-build"

cmake -S "$vnc_source" -B "$dependency_work/vnc-build" \
    -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$dependency_prefix" -DCMAKE_PREFIX_PATH="${CMAKE_PREFIX_PATH:-}" \
    -DCMAKE_INSTALL_RPATH="$dependency_prefix/lib" \
    -DWITH_EXAMPLES=OFF -DWITH_TESTS=OFF -DWITH_SDL=OFF -DWITH_GTK=OFF \
    -DWITH_QT=OFF -DWITH_XCB=OFF -DWITH_GNUTLS=OFF -DWITH_GCRYPT=OFF \
    -DWITH_OPENSSL=ON -DWITH_FFMPEG=OFF -DWITH_LIBSSHTUNNEL=OFF \
    -DWITH_LZO=OFF -DWITH_SASL=OFF -DWITH_PNG=OFF -DWITH_SYSTEMD=OFF
cmake --build "$dependency_work/vnc-build" -j "$build_jobs"
cmake --install "$dependency_work/vnc-build"

printf '\nDependencies installed in %s\n' "$dependency_prefix"
printf 'Configure with: PKG_CONFIG_PATH="%s/lib/pkgconfig" cmake -S . -B build\n' "$dependency_prefix"
