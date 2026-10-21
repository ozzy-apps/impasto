cmake_minimum_required(VERSION 3.21)
if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
endif()
project(ImpastoWebPRuntime LANGUAGES C)

# Optional helper for Qt kits whose qwebp plugin imports libwebp-7__.dll,
# libwebpdemux-2__.dll and libwebpmux-3__.dll without shipping these libraries.
# Build separately with the same MinGW toolchain used for Impasto.
include(FetchContent)
set(BUILD_SHARED_LIBS ON CACHE BOOL "" FORCE)
foreach(tool ANIM_UTILS CWEBP DWEBP GIF2WEBP IMG2WEBP VWEBP WEBPINFO WEBPMUX EXTRAS WEBP_JS FUZZTEST)
    set(WEBP_BUILD_${tool} OFF CACHE BOOL "" FORCE)
endforeach()
set(WEBP_BUILD_LIBWEBPMUX ON CACHE BOOL "" FORCE)
FetchContent_Declare(libwebp
    URL https://github.com/webmproject/libwebp/archive/refs/tags/v1.6.0.tar.gz
    URL_HASH SHA256=93a852c2b3efafee3723efd4636de855b46f9fe1efddd607e1f42f60fc8f2136)
FetchContent_MakeAvailable(libwebp)
set_target_properties(webp PROPERTIES OUTPUT_NAME webp-7__)
set_target_properties(webpdemux PROPERTIES OUTPUT_NAME webpdemux-2__)
set_target_properties(libwebpmux PROPERTIES OUTPUT_NAME webpmux-3__)
