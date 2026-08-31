cmake_minimum_required(VERSION 4.0)
project(FIV)

set(CMAKE_CXX_STANDARD 23)


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
    target_link_directories(${EXE_NAME} PRIVATE "${_3RDPARTY_LIB_DIR}")
    file(GLOB _3rdparty_libs
            "${_3RDPARTY_LIB_DIR}/*.a" "${_3RDPARTY_LIB_DIR}/*.so" "${_3RDPARTY_LIB_DIR}/*.dylib" "${_3RDPARTY_LIB_DIR}/*.lib")
    if (_3rdparty_libs)
        target_link_libraries(${EXE_NAME} PRIVATE ${_3rdparty_libs})
        target_link_libraries(${EXE_NAME} PRIVATE ${_3rdparty_libs})
    endif ()
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