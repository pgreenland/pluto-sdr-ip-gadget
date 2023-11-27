cmake_minimum_required(VERSION 3.9.4)

project(sdr_ip_gadget LANGUAGES C)

# Options
option(GENERATE_STATS "Generate and output runtime stats" OFF)

# Check if link time optimisation is supported
include(CheckIPOSupported)
check_ipo_supported(RESULT lto_supported OUTPUT lto_error)

# From: https://www.mattkeeter.com/blog/2018-01-06-versioning/
execute_process(COMMAND git log --pretty=format:'%h' -n 1
                OUTPUT_VARIABLE GIT_REV
                ERROR_QUIET)
if ("${GIT_REV}" STREQUAL "")
    set(GIT_REV "N/A")
    set(GIT_DIFF "")
else()
    execute_process(
        COMMAND bash -c "git diff --quiet --exit-code || echo +"
        OUTPUT_VARIABLE GIT_DIFF)

    string(STRIP "${GIT_REV}" GIT_REV)
    #string(SUBSTRING "${GIT_REV}" 1 7 GIT_REV)
    string(STRIP "${GIT_DIFF}" GIT_DIFF)
endif()
set(GIT_VERSION "${GIT_REV}${GIT_DIFF}")

if (NOT "${GIT_VERSION_OVERRIDE}" STREQUAL "")
    # Use provided override if package isn't being built within git repo
    set(GIT_VERSION "${GIT_VERSION_OVERRIDE}")
endif()

add_executable(sdr_ip_gadget
    main.c
    epoll_loop.c
    thread_read.c
    thread_write.c
    utils.c
)
target_link_libraries(sdr_ip_gadget
    pthread
    iio
)
target_compile_definitions(sdr_ip_gadget PRIVATE
    PROGRAM_VERSION="${GIT_VERSION}"
    STATS_PERIOD_SECS=2)
if (GENERATE_STATS)
target_compile_definitions(sdr_ip_gadget PRIVATE GENERATE_STATS=1)
endif(GENERATE_STATS)

if(lto_supported)
    message(STATUS "LTO enabled")
    set_property(TARGET sdr_ip_gadget PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
else()
    message(STATUS "LTO not supported: <${lto_error}>")
endif()

install(TARGETS sdr_ip_gadget RUNTIME DESTINATION sbin)
