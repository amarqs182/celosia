# Cross-compilation toolchain for aarch64-linux-gnu (RK3326 / R36S)
#
# Sysroot: ~/tools/r36s-sysroot
#   - glibc 2.41 runtime + headers from Debian trixie arm64 (matches the
#     device exactly: Debian GNU/Linux 13 "trixie", glibc 2.41-12+deb13u4)
#   - linux-libc-dev 6.1.176 kernel headers
#   - gcc 16.2.0 / binutils 2.47 from the Arch aarch64-linux-gnu packages
#
# The earlier sysroot (~/tools/aarch64-sysroot) shipped glibc 2.44, which
# produced binaries requiring GLIBC_2.43 — newer than the device's 2.41, so
# the loader refused to start them ("version `GLIBC_2.43' not found").
# This sysroot pins the baseline to what the device actually provides.
#
# The GPU libraries must mirror the device: it has
#   libGLESv2.so.2 -> libMali.so, libGLESv3.so.3 -> libMali.so,
#   libEGL.so.1 -> libMali.so, libgbm.so.1 -> libMali.so
# (ARM's libmali, the Bifrost r13p0 build, reporting OpenGL ES 3.2). The
# sysroot needs the matching link-time names:
#   ln -sfn libMali.so libGLESv3.so
#   ln -sfn libMali.so libGLESv3.so.3
#   ln -sfn libMali.so libGLESv3.so.3.0.0
# Without them find_library(NAMES GLESv3 GLESv2 GLES) falls back to the
# sysroot's libGLESv2.so, which is a 27-symbol GLES 2.0 stub: the link
# then fails on glGenVertexArrays, glBindFramebuffer, glCopyBufferSubData
# and glClearDepthf. (The runtime also needs libGLESv3.so to resolve at
# run time, so the same names must exist on the device itself.)
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

set(R36S_SYSROOT "$ENV{HOME}/tools/r36s-sysroot" CACHE PATH
    "AArch64 sysroot for the R36S (Debian trixie arm64)")
set(_r36s_sysroot "${R36S_SYSROOT}")
set(_r36s_triplet "aarch64-linux-gnu")
set(_r36s_libdir "${_r36s_sysroot}/usr/lib/${_r36s_triplet}")

if(NOT IS_DIRECTORY "${_r36s_sysroot}/usr/bin")
    message(FATAL_ERROR
        "R36S sysroot not found at '${_r36s_sysroot}'. Set -DR36S_SYSROOT=/path/to/r36s-sysroot.")
endif()

set(CMAKE_C_COMPILER "${_r36s_sysroot}/usr/bin/${_r36s_triplet}-gcc")
set(CMAKE_CXX_COMPILER "${_r36s_sysroot}/usr/bin/${_r36s_triplet}-g++")
set(CMAKE_ASM_COMPILER "${_r36s_sysroot}/usr/bin/${_r36s_triplet}-as")

# The Arch gcc build looks for the assembler and linker next to *itself* on
# PATH, not next to its own prefix, so it picks up the host's x86-64 `as`
# and fails with "opção não reconhecida "-EL" (-EL is the AArch64
# little-endian marker). `-B<dir>` tells gcc where to find its own binutils,
# which makes the cross build independent of the caller's PATH.
# Celosia is a C++ project, so the flag is needed for the CXX driver too.
set(CMAKE_C_FLAGS_INIT "-B${_r36s_sysroot}/usr/bin/")
set(CMAKE_CXX_FLAGS_INIT "-B${_r36s_sysroot}/usr/bin/")

# The Arch gcc build hardcodes its own include/lib search relative to the
# toolchain prefix; point it at the Debian-layout sysroot instead.
set(CMAKE_SYSROOT "${_r36s_sysroot}/usr")

# crt1.o/crti.o/crtn.o and libc_nonshared.a live in the multiarch libdir.
# These must survive every cmake reconfigure, so set them as CACHE entries
# rather than the _INIT variables (which the toolchain file's cache scope
# can drop on a second configure run).
#
# -rpath-link lets the linker resolve the transitive dependencies of the
# shared libraries it links against (SDL2 pulls X11, wayland, libdrm, ALSA,
# PulseAudio and libdecor). Those transitive symbols are resolved by the
# dynamic loader on the device, so --allow-shlib-undefined keeps the link
# from failing on the ones the sysroot stubs do not provide.
set(_r36s_link_flags
    "-B${_r36s_libdir}/ -L${_r36s_libdir}/ -Wl,-rpath-link,${_r36s_libdir}/ -Wl,--allow-shlib-undefined")

set(CMAKE_EXE_LINKER_FLAGS "${_r36s_link_flags}" CACHE STRING "" FORCE)
set(CMAKE_SHARED_LINKER_FLAGS "${_r36s_link_flags}" CACHE STRING "" FORCE)
set(CMAKE_MODULE_LINKER_FLAGS "${_r36s_link_flags}" CACHE STRING "" FORCE)

set(CMAKE_FIND_ROOT_PATH "${_r36s_sysroot}/usr")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# The target sysroot includes SDL2/EGL/GLES headers, libraries, and pkg-config
# metadata. Point the host pkg-config at those target packages so the cross
# build includes the same display backends the PortMaster launcher requests.
set(ENV{PKG_CONFIG_SYSROOT_DIR} "${_r36s_sysroot}")
set(ENV{PKG_CONFIG_LIBDIR} "${_r36s_sysroot}/usr/lib/${_r36s_triplet}/pkgconfig")
