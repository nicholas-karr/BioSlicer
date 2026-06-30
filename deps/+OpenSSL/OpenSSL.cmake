
include(ProcessorCount)
ProcessorCount(NPROC)

set(_conf_cmd "./config")
set(_cross_arch "")
if (CMAKE_CROSSCOMPILING)
    set(_conf_cmd "./Configure")
    if (${CMAKE_SYSTEM_PROCESSOR} STREQUAL "aarch64" OR ${CMAKE_SYSTEM_PROCESSOR} STREQUAL "arm64")
        set(_cross_arch "linux-aarch64")
    elseif (${CMAKE_SYSTEM_PROCESSOR} STREQUAL "armhf") # For raspbian
        set(_cross_arch "linux-armv4")
    endif ()
    set(_configure_cmd env
        "CC=${CMAKE_C_COMPILER}"
        "AR=${TOOLCHAIN_PREFIX}-ar"
        "RANLIB=${TOOLCHAIN_PREFIX}-ranlib"
        ${_conf_cmd} ${_cross_arch}
        "--prefix=${${PROJECT_NAME}_DEP_INSTALL_PREFIX}"
        no-shared no-ssl3-method no-dynamic-engine -Wa,--noexecstack)
else ()
    set(_configure_cmd ${_conf_cmd}
        "--prefix=${${PROJECT_NAME}_DEP_INSTALL_PREFIX}"
        no-shared no-ssl3-method no-dynamic-engine -Wa,--noexecstack)
endif ()

ExternalProject_Add(dep_OpenSSL
    EXCLUDE_FROM_ALL ON
    URL "https://github.com/openssl/openssl/archive/OpenSSL_1_1_0l.tar.gz"
    URL_HASH SHA256=e2acf0cf58d9bff2b42f2dc0aee79340c8ffe2c5e45d3ca4533dd5d4f5775b1d
    DOWNLOAD_DIR ${${PROJECT_NAME}_DEP_DOWNLOAD_DIR}/OpenSSL
    BUILD_IN_SOURCE ON
    CONFIGURE_COMMAND ${_configure_cmd}
    BUILD_COMMAND make depend && make "-j${NPROC}"
    INSTALL_COMMAND make install_sw
)