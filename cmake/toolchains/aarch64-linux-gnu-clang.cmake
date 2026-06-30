set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

# Used by autoconf deps (GMP, MPFR) for ./configure --host=
set(TOOLCHAIN_PREFIX aarch64-linux-gnu)

# Compiler wrapper scripts created by deploy.sh during the cross-compile build.
# They embed --target, --sysroot, --gcc-toolchain, and -isystem so every
# cmake sub-project AND autoconf dep receives the right flags via CC/CXX.
set(CMAKE_C_COMPILER   "$ENV{BIOSLICER_CROSS_CC}"  CACHE FILEPATH "")
set(CMAKE_CXX_COMPILER "$ENV{BIOSLICER_CROSS_CXX}" CACHE FILEPATH "")

# Let cmake know the sysroot so find_package/find_library search inside it.
if(DEFINED ENV{BIOSLICER_AARCH64_SYSROOT})
    set(CMAKE_SYSROOT "$ENV{BIOSLICER_AARCH64_SYSROOT}" CACHE PATH "")
endif()

# Use lld for linking (x86_64 lld produces aarch64 ELF output).
if(DEFINED ENV{BIOSLICER_CROSS_LLD})
    set(_ldflag "-fuse-ld=$ENV{BIOSLICER_CROSS_LLD}")
    set(CMAKE_EXE_LINKER_FLAGS_INIT    "${_ldflag}" CACHE STRING "")
    set(CMAKE_SHARED_LINKER_FLAGS_INIT "${_ldflag}" CACHE STRING "")
    set(CMAKE_MODULE_LINKER_FLAGS_INIT "${_ldflag}" CACHE STRING "")
endif()

# aarch64 is always little-endian — avoids try_run in TestBigEndian.
set(CMAKE_WORDS_BIGENDIAN OFF CACHE BOOL "")

# Pre-set pthreads so FindThreads doesn't need to link a test executable
# before we've confirmed the whole toolchain chain is wired up.
set(CMAKE_HAVE_PTHREAD_CREATE  1     CACHE INTERNAL "")
set(CMAKE_THREAD_LIBS_INIT    "-lpthread" CACHE STRING "")
set(CMAKE_HAVE_THREADS_LIBRARY 1     CACHE INTERNAL "")
set(CMAKE_USE_PTHREADS_INIT    1     CACHE INTERNAL "")

# Never search sysroot for host build tools; search both sysroot and prefix
# paths for libraries/headers/packages so pre-built deps in DESTDIR are found.
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY BOTH)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE BOTH)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE BOTH)
