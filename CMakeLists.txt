cmake_minimum_required(VERSION 3.20)
project(burnout3 C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Default options and warnings
if(MSVC)
    add_compile_options(/W3 /O2)
else()
    add_compile_options(-Wall -Wextra -O2)
endif()

# Threading
find_package(Threads REQUIRED)

# Include directories
set(INC_DIRS
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/tools/cextract
    ${CMAKE_CURRENT_SOURCE_DIR}/tools/cextract/wma
)

# On Windows / MinGW, add our compat headers (which provides sys/mman.h, win_posix_compat.h, etc.)
if(WIN32)
    list(APPEND INC_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/src/compat)
endif()

# Locate SDL2, SDL2_image, and ZLIB
# Check Scoop default paths on Windows if not provided
if(WIN32)
    set(SCOOP_DIR "$ENV{USERPROFILE}/scoop/apps")
    if(EXISTS "${SCOOP_DIR}/sdl2/current")
        list(APPEND INC_DIRS "${SCOOP_DIR}/sdl2/current/include" "${SCOOP_DIR}/sdl2/current/include/SDL2")
        link_directories("${SCOOP_DIR}/sdl2/current/lib")
    endif()
    if(EXISTS "${SCOOP_DIR}/sdl2-image/current")
        # Provide both include and include/SDL2_image so <SDL2/SDL_image.h> and <SDL_image.h> resolve
        list(APPEND INC_DIRS "${SCOOP_DIR}/sdl2-image/current/include" "${SCOOP_DIR}/sdl2-image/current/include/SDL2_image")
        link_directories("${SCOOP_DIR}/sdl2-image/current/lib")
    endif()
    if(EXISTS "${SCOOP_DIR}/zlib/current")
        list(APPEND INC_DIRS "${SCOOP_DIR}/zlib/current/include")
        link_directories("${SCOOP_DIR}/zlib/current/lib")
    endif()

    # Create forwarding header SDL2/SDL_image.h in binary dir so #include <SDL2/SDL_image.h> resolves cleanly
    file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/gen_include/SDL2")
    file(WRITE "${CMAKE_BINARY_DIR}/gen_include/SDL2/SDL_image.h" "#pragma once\n#include <SDL_image.h>\n")
    list(APPEND INC_DIRS "${CMAKE_BINARY_DIR}/gen_include")
endif()

# WMA sources: rockbox if vendored, otherwise stub
set(RB_ENTRY "${CMAKE_CURRENT_SOURCE_DIR}/third_party/rockbox/codecs/libwma/wmadeci.c")
if(EXISTS "${RB_ENTRY}")
    set(WMA_SRCS
        tools/cextract/wma/b3_wma.c
        third_party/rockbox/codecs/libwma/wmadeci.c
        third_party/rockbox/codecs/libwma/wmafixed.c
        third_party/rockbox/codecs/lib/mdct.c
        third_party/rockbox/codecs/lib/mdct_lookup.c
        third_party/rockbox/codecs/lib/fft-ffmpeg.c
        third_party/rockbox/codecs/lib/ffmpeg_bitstream.c
    )
    add_compile_definitions(ROCKBOX)
    list(APPEND INC_DIRS
        ${CMAKE_CURRENT_SOURCE_DIR}/tools/cextract/wma/codecs/lib
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/rockbox
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/rockbox/codecs
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/rockbox/codecs/lib
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/rockbox/codecs/libwma
    )
else()
    set(WMA_SRCS tools/cextract/wma/b3_wma_stub.c)
endif()

# cextract sources (all tools/cextract/*.c EXCEPT cx_main.c)
file(GLOB ALL_CX_SRCS "tools/cextract/*.c")
list(FILTER ALL_CX_SRCS EXCLUDE REGEX ".*/cx_main\\.c$")
set(CX_SRCS ${ALL_CX_SRCS} ${WMA_SRCS})

# Target 1: cextract_obj (compiled WITHOUT isoshim forced include)
add_library(cextract_obj OBJECT ${CX_SRCS})
target_include_directories(cextract_obj PRIVATE ${INC_DIRS})
if(WIN32)
    target_compile_definitions(cextract_obj PRIVATE _WIN32 WIN32_LEAN_AND_MEAN)
    if(MSVC)
        target_compile_options(cextract_obj PRIVATE /FI "${CMAKE_CURRENT_SOURCE_DIR}/src/compat/win_posix_compat.h")
    else()
        target_compile_options(cextract_obj PRIVATE -include "${CMAKE_CURRENT_SOURCE_DIR}/src/compat/win_posix_compat.h")
    endif()
endif()

# Target 2: isodata_obj (src/burnout3_isodata.c, compiled WITHOUT isoshim forced include)
add_library(isodata_obj OBJECT src/burnout3_isodata.c)
target_include_directories(isodata_obj PRIVATE ${INC_DIRS})
if(WIN32)
    target_compile_definitions(isodata_obj PRIVATE _WIN32 WIN32_LEAN_AND_MEAN)
    if(MSVC)
        target_compile_options(isodata_obj PRIVATE /FI "${CMAKE_CURRENT_SOURCE_DIR}/src/compat/win_posix_compat.h")
    else()
        target_compile_options(isodata_obj PRIVATE -include "${CMAKE_CURRENT_SOURCE_DIR}/src/compat/win_posix_compat.h")
    endif()
endif()

# Target 3: burnout3 executable (all src/*.c EXCEPT burnout3_isodata.c, compiled WITH isoshim forced include)
file(GLOB ALL_SRC_SRCS "src/*.c")
list(FILTER ALL_SRC_SRCS EXCLUDE REGEX ".*/burnout3_isodata\\.c$")

add_executable(burnout3 ${ALL_SRC_SRCS} $<TARGET_OBJECTS:cextract_obj> $<TARGET_OBJECTS:isodata_obj>)
target_include_directories(burnout3 PRIVATE ${INC_DIRS})

# Force-include burnout3_isoshim.h for game sources
if(MSVC)
    target_compile_options(burnout3 PRIVATE /FI "${CMAKE_CURRENT_SOURCE_DIR}/src/burnout3_isoshim.h")
else()
    target_compile_options(burnout3 PRIVATE -include "${CMAKE_CURRENT_SOURCE_DIR}/src/burnout3_isoshim.h")
endif()

# Definitions
if(WIN32)
    # Define SDL_MAIN_HANDLED so MinGW's CRT main() is used without MSVC SDL2main.lib conflicts
    target_compile_definitions(burnout3 PRIVATE _WIN32 WIN32_LEAN_AND_MEAN SDL_MAIN_HANDLED)
endif()

# Link libraries
if(WIN32)
    target_link_libraries(burnout3 PRIVATE
        SDL2
        SDL2_image
        zlib
        opengl32
        ws2_32
        Threads::Threads
    )
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(SDL2 REQUIRED sdl2 SDL2_image)
    target_link_libraries(burnout3 PRIVATE
        ${SDL2_LIBRARIES}
        GL
        m
        z
        Threads::Threads
    )
endif()

# Standalone cxtract extraction tool
add_executable(cxtract tools/cextract/cx_main.c $<TARGET_OBJECTS:cextract_obj>)
target_include_directories(cxtract PRIVATE ${INC_DIRS})
if(WIN32)
    target_compile_definitions(cxtract PRIVATE _WIN32 WIN32_LEAN_AND_MEAN)
    if(MSVC)
        target_compile_options(cxtract PRIVATE /FI "${CMAKE_CURRENT_SOURCE_DIR}/src/compat/win_posix_compat.h")
    else()
        target_compile_options(cxtract PRIVATE -include "${CMAKE_CURRENT_SOURCE_DIR}/src/compat/win_posix_compat.h")
    endif()
    target_link_libraries(cxtract PRIVATE zlib ws2_32 Threads::Threads)

    # Automatically copy required runtime DLLs to build directory on Windows
    add_custom_command(TARGET burnout3 POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${SCOOP_DIR}/sdl2/current/lib/SDL2.dll" $<TARGET_FILE_DIR:burnout3>
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${SCOOP_DIR}/sdl2-image/current/lib/SDL2_image.dll" $<TARGET_FILE_DIR:burnout3>
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${SCOOP_DIR}/zlib/current/bin/zlib.dll" $<TARGET_FILE_DIR:burnout3>
        COMMENT "Copying runtime DLLs to output directory..."
    )
else()
    target_link_libraries(cxtract PRIVATE m z Threads::Threads)
endif()

# ==============================================================================
# C Unit Test Suites & CTest Integration
# ==============================================================================
enable_testing()

# 1. test_panels_soup
add_executable(test_panels_soup tools/test_panels_soup.c src/burnout3_panels.c src/burnout3_vehicle_sim.c)
target_include_directories(test_panels_soup PRIVATE ${INC_DIRS})
if(WIN32)
    target_compile_definitions(test_panels_soup PRIVATE _WIN32 WIN32_LEAN_AND_MEAN)
    if(MSVC)
        target_compile_options(test_panels_soup PRIVATE /FI "${CMAKE_CURRENT_SOURCE_DIR}/src/compat/win_posix_compat.h")
    else()
        target_compile_options(test_panels_soup PRIVATE -include "${CMAKE_CURRENT_SOURCE_DIR}/src/compat/win_posix_compat.h")
    endif()
endif()
add_test(NAME test_panels_soup COMMAND test_panels_soup)

# 2. test_crash_director_zones
add_executable(test_crash_director_zones tools/test_crash_director_zones.c src/burnout3_vehicle_sim.c)
target_include_directories(test_crash_director_zones PRIVATE ${INC_DIRS})
if(WIN32)
    target_compile_definitions(test_crash_director_zones PRIVATE _WIN32 WIN32_LEAN_AND_MEAN)
    if(MSVC)
        target_compile_options(test_crash_director_zones PRIVATE /FI "${CMAKE_CURRENT_SOURCE_DIR}/src/compat/win_posix_compat.h")
    else()
        target_compile_options(test_crash_director_zones PRIVATE -include "${CMAKE_CURRENT_SOURCE_DIR}/src/compat/win_posix_compat.h")
    endif()
endif()
add_test(NAME test_crash_director_zones COMMAND test_crash_director_zones)

# 3. test_psyche_out
add_executable(test_psyche_out tools/test_psyche_out.c src/burnout3_td_rules.c)
target_include_directories(test_psyche_out PRIVATE ${INC_DIRS})
if(WIN32)
    target_compile_definitions(test_psyche_out PRIVATE _WIN32 WIN32_LEAN_AND_MEAN)
    if(MSVC)
        target_compile_options(test_psyche_out PRIVATE /FI "${CMAKE_CURRENT_SOURCE_DIR}/src/compat/win_posix_compat.h")
    else()
        target_compile_options(test_psyche_out PRIVATE -include "${CMAKE_CURRENT_SOURCE_DIR}/src/compat/win_posix_compat.h")
    endif()
endif()
add_test(NAME test_psyche_out COMMAND test_psyche_out)

# 4. test_sap_broadphase
add_executable(test_sap_broadphase tools/test_sap_broadphase.c src/burnout3_carcol.c src/burnout3_vehicle_sim.c)
target_include_directories(test_sap_broadphase PRIVATE ${INC_DIRS})
if(WIN32)
    target_compile_definitions(test_sap_broadphase PRIVATE _WIN32 WIN32_LEAN_AND_MEAN)
    if(MSVC)
        target_compile_options(test_sap_broadphase PRIVATE /FI "${CMAKE_CURRENT_SOURCE_DIR}/src/compat/win_posix_compat.h")
    else()
        target_compile_options(test_sap_broadphase PRIVATE -include "${CMAKE_CURRENT_SOURCE_DIR}/src/compat/win_posix_compat.h")
    endif()
endif()
add_test(NAME test_sap_broadphase COMMAND test_sap_broadphase)

# 5. validate_traffic_pool
add_executable(validate_traffic_pool tools/validate_traffic_pool.c src/burnout3_traffic_pool.c)
target_include_directories(validate_traffic_pool PRIVATE ${INC_DIRS})
if(WIN32)
    target_compile_definitions(validate_traffic_pool PRIVATE _WIN32 WIN32_LEAN_AND_MEAN)
    if(MSVC)
        target_compile_options(validate_traffic_pool PRIVATE /FI "${CMAKE_CURRENT_SOURCE_DIR}/src/compat/win_posix_compat.h")
    else()
        target_compile_options(validate_traffic_pool PRIVATE -include "${CMAKE_CURRENT_SOURCE_DIR}/src/compat/win_posix_compat.h")
    endif()
endif()
add_test(NAME validate_traffic_pool COMMAND validate_traffic_pool)

# 6. validate_traffic_reservations
add_executable(validate_traffic_reservations tools/validate_traffic_reservations.c src/burnout3_traffic_reservations.c)
target_include_directories(validate_traffic_reservations PRIVATE ${INC_DIRS})
if(WIN32)
    target_compile_definitions(validate_traffic_reservations PRIVATE _WIN32 WIN32_LEAN_AND_MEAN)
    if(MSVC)
        target_compile_options(validate_traffic_reservations PRIVATE /FI "${CMAKE_CURRENT_SOURCE_DIR}/src/compat/win_posix_compat.h")
    else()
        target_compile_options(validate_traffic_reservations PRIVATE -include "${CMAKE_CURRENT_SOURCE_DIR}/src/compat/win_posix_compat.h")
    endif()
endif()
add_test(NAME validate_traffic_reservations COMMAND validate_traffic_reservations)

# 7. validate_frozen_soup
add_executable(validate_frozen_soup tools/validate_frozen_soup.c src/burnout3_collision.c src/burnout3_vehicle_sim.c)
target_include_directories(validate_frozen_soup PRIVATE ${INC_DIRS})
if(WIN32)
    target_compile_definitions(validate_frozen_soup PRIVATE _WIN32 WIN32_LEAN_AND_MEAN)
    if(MSVC)
        target_compile_options(validate_frozen_soup PRIVATE /FI "${CMAKE_CURRENT_SOURCE_DIR}/src/compat/win_posix_compat.h")
    else()
        target_compile_options(validate_frozen_soup PRIVATE -include "${CMAKE_CURRENT_SOURCE_DIR}/src/compat/win_posix_compat.h")
    endif()
endif()
add_test(NAME validate_frozen_soup COMMAND validate_frozen_soup)

