cmake_minimum_required (VERSION 3.15)

set(CMAKE_CXX_STANDARD 20)

cmake_policy(SET CMP0091 NEW) # for MSVC_RUNTIME_LIBRARY

set(VCPKG_TARGET_TRIPLET "x64-windows-static" CACHE STRING "VCPKG Target Triplet to use")

project ("ZArchive")

set(SOURCE_FILES_LIB
src/zarchivewriter.cpp
src/zarchivereader.cpp
src/sha_256.c
)

# build static library
add_library (zarchive STATIC ${SOURCE_FILES_LIB})
set_property(TARGET zarchive PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

target_include_directories(zarchive
    PUBLIC
        $<INSTALL_INTERFACE:include>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)


include_directories(PRIVATE
${CMAKE_CURRENT_SOURCE_DIR}/include
)

find_package(zstd REQUIRED)
target_link_libraries(zarchive PRIVATE zstd::libzstd_static)

# standalone executable
add_executable (zarchiveTool src/main.cpp)
set_property(TARGET zarchiveTool PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set_target_properties(zarchiveTool PROPERTIES OUTPUT_NAME "zarchive")
target_link_libraries(zarchiveTool PRIVATE zarchive)

# install
include(GNUInstallDirs)

install(DIRECTORY ${CMAKE_SOURCE_DIR}/include/zarchive/ DESTINATION "include/zarchive" FILES_MATCHING PATTERN "zarchive*.h")
INSTALL(TARGETS zarchive LIBRARY)
INSTALL(TARGETS zarchiveTool)
