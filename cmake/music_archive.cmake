# File-format dependencies belong only to the desktop persistence adapter.
function(barelief_music_archive_dependencies)
    include(FetchContent)
    set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)
    set(BUILD_TESTS OFF)
    set(BUILD_FUZZERS OFF)
    set(BUILD_BENCHMARKS OFF)
    set(BUILD_EXAMPLES OFF)
    set(BUILD_PLUGINS OFF)
    set(BUILD_SHARED OFF)
    set(BUILD_STATIC ON)
    set(DEACTIVATE_ZLIB ON)
    set(INSTALL_PROJECT OFF)
    FetchContent_Declare(lz4
        URL https://codeload.github.com/lz4/lz4/tar.gz/refs/tags/v1.10.0
        URL_HASH SHA256=537512904744b35e232912055ccf8ec66d768639ff3abe5788d90d792ec5f48b
        SOURCE_SUBDIR build/cmake EXCLUDE_FROM_ALL)
    FetchContent_Declare(zstd
        URL https://codeload.github.com/facebook/zstd/tar.gz/refs/tags/v1.5.7
        URL_HASH SHA256=37d7284556b20954e56e1ca85b80226768902e2edabd3b649e9e72c0c9012ee3
        SOURCE_SUBDIR build/cmake EXCLUDE_FROM_ALL)
    FetchContent_Declare(blosc2
        GIT_REPOSITORY https://github.com/Blosc/c-blosc2.git
        GIT_TAG ef9267838d70fe7ab9b8863f12cfe36d7ca6228f
        GIT_CONFIG core.symlinks=false
        EXCLUDE_FROM_ALL)
    FetchContent_Declare(miniz
        URL https://codeload.github.com/richgel999/miniz/tar.gz/refs/tags/3.1.0
        URL_HASH SHA256=09569fc19d060ac9f5999ba9356728c2494ebe6a24ac0eb0a6b6ae3d396cfea6
        EXCLUDE_FROM_ALL)
    FetchContent_MakeAvailable(blosc2 miniz)
    FetchContent_GetProperties(lz4)
    FetchContent_GetProperties(zstd)
    target_compile_definitions(miniz PUBLIC MINIZ_NO_ZLIB_COMPATIBLE_NAMES)
    set_target_properties(miniz blosc2_static PROPERTIES AUTOMOC OFF SYSTEM TRUE)
    install(FILES ${blosc2_SOURCE_DIR}/LICENSE.txt
        DESTINATION share/licenses/Barelief RENAME Blosc2-LICENSE.txt)
    install(FILES ${miniz_SOURCE_DIR}/LICENSE
        DESTINATION share/licenses/Barelief RENAME miniz-LICENSE.txt)
    install(FILES ${lz4_SOURCE_DIR}/lib/LICENSE
        DESTINATION share/licenses/Barelief RENAME LZ4-LICENSE.txt)
    install(FILES ${zstd_SOURCE_DIR}/LICENSE
        DESTINATION share/licenses/Barelief RENAME Zstd-LICENSE.txt)
endfunction()
barelief_music_archive_dependencies()
