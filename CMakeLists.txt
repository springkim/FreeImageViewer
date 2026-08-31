cmake_minimum_required(VERSION 3.24)
project(FIV LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)


set(EXE_NAME FIV)

# === === === === Source === === === ===
set(SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src")
file(GLOB_RECURSE SRC
        CONFIGURE_DEPENDS
        "${SRC_DIR}/*.cpp"
)
add_executable(${EXE_NAME}
        main.cpp
        ${SRC}
)
target_include_directories(${EXE_NAME} PRIVATE
        "${SRC_DIR}"
)
# === === === === 3rdparty === === === ===
set(_3RDPARTY_DIR "${CMAKE_CURRENT_SOURCE_DIR}/3rdparty")
set(_3RDPARTY_INCLUDE_DIR "${_3RDPARTY_DIR}/include")
set(_3RDPARTY_LIB_DIR "${_3RDPARTY_DIR}/lib")
set(_3RDPARTY_BIN_DIR "${_3RDPARTY_DIR}/bin")
set(_3RDPARTY_RES_DIR "${_3RDPARTY_DIR}/res")

if (EXISTS "${_3RDPARTY_INCLUDE_DIR}")
    target_include_directories(${EXE_NAME} PRIVATE "${_3RDPARTY_INCLUDE_DIR}")
    # OpenEXR/Imath 헤더는 서로를 <ImathBox.h> 처럼 최상위 경로로 include 하므로
    # 해당 디렉터리들도 검색 경로에 넣어 준다.
    foreach (_nested_inc "OpenEXR" "Imath")
        if (EXISTS "${_3RDPARTY_INCLUDE_DIR}/${_nested_inc}")
            target_include_directories(${EXE_NAME} PRIVATE "${_3RDPARTY_INCLUDE_DIR}/${_nested_inc}")
        endif ()
    endforeach ()
endif ()

if (EXISTS "${_3RDPARTY_LIB_DIR}")
    # Only select library formats understood by the active toolchain.  Mixing
    # Linux .so, macOS .dylib and Windows .lib files makes a shared checkout
    # impossible to configure reliably on more than one platform.
    if (MSVC)
        file(GLOB _3rdparty_static_libs CONFIGURE_DEPENDS
                "${_3RDPARTY_LIB_DIR}/*.lib")
        set(_3rdparty_shared_libs "")
    elseif (WIN32)
        file(GLOB _3rdparty_static_libs CONFIGURE_DEPENDS
                "${_3RDPARTY_LIB_DIR}/*.a")
        # Some MinGW builds emit only .lib archives.  Use them as a fallback,
        # but never mix the two sets because they can contain the same symbols.
        if (NOT _3rdparty_static_libs)
            file(GLOB _3rdparty_static_libs CONFIGURE_DEPENDS
                    "${_3RDPARTY_LIB_DIR}/*.lib")
        endif ()
        set(_3rdparty_shared_libs "")
    elseif (APPLE)
        file(GLOB _3rdparty_static_libs CONFIGURE_DEPENDS
                "${_3RDPARTY_LIB_DIR}/*.a")
        file(GLOB _3rdparty_shared_libs CONFIGURE_DEPENDS
                "${_3RDPARTY_LIB_DIR}/*.dylib")
    elseif (UNIX)
        file(GLOB _3rdparty_static_libs CONFIGURE_DEPENDS
                "${_3RDPARTY_LIB_DIR}/*.a")
        file(GLOB _3rdparty_shared_libs CONFIGURE_DEPENDS
                "${_3RDPARTY_LIB_DIR}/*.so")
    else ()
        message(FATAL_ERROR "Unsupported platform: ${CMAKE_SYSTEM_NAME}")
    endif ()

    # Static archives may have circular or order-dependent dependencies (for
    # example brotlidec -> brotlicommon).  Ask the linker to rescan the group
    # instead of relying on the alphabetical order returned by GLOB.
    if (_3rdparty_static_libs)
        # The bundled Windows libraries are static archives.  Their public
        # headers otherwise default to dllimport, which makes MinGW emit
        # references such as __imp_heif_init that cannot exist in a .a file.
        if (WIN32)
            target_compile_definitions(${EXE_NAME} PRIVATE
                    LIBHEIF_STATIC_BUILD
                    OPJ_STATIC
                    JXL_STATIC_DEFINE
                    JXL_CMS_STATIC_DEFINE
                    JXL_THREADS_STATIC_DEFINE
                    SPNG_STATIC)
        endif ()

        # GNU/MinGW linkers need an archive group to resolve circular and
        # order-dependent dependencies such as brotlidec -> brotlicommon.
        if (CMAKE_LINK_GROUP_USING_RESCAN_SUPPORTED OR
                CMAKE_CXX_LINK_GROUP_USING_RESCAN_SUPPORTED)
            target_link_libraries(${EXE_NAME} PRIVATE
                    "$<LINK_GROUP:RESCAN,${_3rdparty_static_libs}>")
        else ()
            # Apple ld and toolchains without RESCAN get a second pass.  This
            # preserves normal archive extraction (unlike force-loading every
            # object, which can create duplicate symbols).
            target_link_libraries(${EXE_NAME} PRIVATE
                    ${_3rdparty_static_libs} ${_3rdparty_static_libs})
        endif ()
    endif ()
    if (_3rdparty_shared_libs)
        target_link_libraries(${EXE_NAME} PRIVATE ${_3rdparty_shared_libs})
    endif ()

    # The bundled x265 archive was built with NUMA support enabled.
    if (UNIX AND NOT APPLE AND EXISTS "${_3RDPARTY_LIB_DIR}/libx265.a")
        find_library(NUMA_LIBRARY NAMES numa)
        if (NOT NUMA_LIBRARY)
            message(FATAL_ERROR
                    "The bundled libx265.a requires libnuma (install libnuma-dev).")
        endif ()
        target_link_libraries(${EXE_NAME} PRIVATE "${NUMA_LIBRARY}")
    endif ()
endif ()

# Use CMake's portable thread target rather than spelling pthread/winpthread
# directly.  It supplies the correct compiler and linker flags per toolchain.
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)
target_link_libraries(${EXE_NAME} PRIVATE Threads::Threads)

if (CMAKE_DL_LIBS)
    target_link_libraries(${EXE_NAME} PRIVATE "${CMAKE_DL_LIBS}")
endif ()

if (WIN32)
    # Common system dependencies of the bundled image/video codecs.
    target_link_libraries(${EXE_NAME} PRIVATE bcrypt ws2_32)
endif ()

if (EXISTS "${_3RDPARTY_BIN_DIR}")
    file(GLOB _3rdparty_bins "${_3RDPARTY_BIN_DIR}/*")
    if (_3rdparty_bins)
        add_custom_command(TARGET ${EXE_NAME} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${_3rdparty_bins}
                $<TARGET_FILE_DIR:${EXE_NAME}>
        )
    endif ()
endif ()

if (EXISTS "${_3RDPARTY_RES_DIR}")
    file(GLOB _3rdparty_res "${_3RDPARTY_RES_DIR}/*")
    if (_3rdparty_res)
        add_custom_command(TARGET ${EXE_NAME} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${_3rdparty_res}
                $<TARGET_FILE_DIR:${EXE_NAME}>
        )
    endif ()
endif ()
