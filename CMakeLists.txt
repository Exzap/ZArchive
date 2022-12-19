cmake_minimum_required (VERSION 3.15)

if (WIN32)
    set(VCPKG_TARGET_TRIPLET "x64-windows-static" CACHE STRING "VCPKG Target Triplet to use")
endif()

project("ZArchive"
    VERSION "0.1.2"
    DESCRIPTION "Library for creating and reading zstd-compressed file archives"
    HOMEPAGE_URL "https://github.com/Exzap/ZArchive"
)

list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")

include(GNUInstallDirs)

set(SOURCE_FILES_LIB
    src/zarchivewriter.cpp
    src/zarchivereader.cpp
    src/sha_256.c
)

# build static library
add_library (zarchive ${SOURCE_FILES_LIB})
add_library (ZArchive::zarchive ALIAS zarchive)
target_compile_features(zarchive PUBLIC cxx_std_20)
set_target_properties(zarchive PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
    VERSION "${PROJECT_VERSION}"
    SOVERSION "${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}"
    CXX_VISIBILITY_PRESET "hidden"
)

target_include_directories(zarchive
    PUBLIC
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
)

find_package(zstd MODULE REQUIRED) # MODULE because zstd::zstd is not defined upstream
target_link_libraries(zarchive PRIVATE zstd::zstd)

# standalone executable
add_executable (zarchiveTool src/main.cpp)
set_property(TARGET zarchiveTool PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set_target_properties(zarchiveTool PROPERTIES OUTPUT_NAME "zarchive")
target_link_libraries(zarchiveTool PRIVATE zarchive)

# install
install(DIRECTORY include/zarchive/ DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/zarchive" FILES_MATCHING PATTERN "zarchive*.h")
install(TARGETS zarchive)
install(TARGETS zarchiveTool)

# pkg-config
include(JoinPaths) # can be replaced by cmake_path(APPEND) in CMake 3.20
join_paths(PKGCONFIG_INCLUDEDIR "\${prefix}" "${CMAKE_INSTALL_INCLUDEDIR}")
join_paths(PKGCONFIG_LIBDIR "\${prefix}" "${CMAKE_INSTALL_LIBDIR}")

configure_file("zarchive.pc.in" "zarchive.pc" @ONLY)
install(
    FILES "${CMAKE_CURRENT_BINARY_DIR}/zarchive.pc"
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/pkgconfig"
)
