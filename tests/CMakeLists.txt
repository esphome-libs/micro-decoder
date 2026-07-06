cmake_minimum_required(VERSION 3.16)
project(micro_decoder_tests CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# script/clang-tidy.sh can lint the tests against this build's compile DB
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Option to enable sanitizers for debugging
option(ENABLE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)

# Treat warnings as errors (also picked up by the micro_decoder library build below)
option(ENABLE_WERROR "Treat warnings as errors" OFF)

if(ENABLE_SANITIZERS)
    message(STATUS "Building with AddressSanitizer and UndefinedBehaviorSanitizer")
    set(SANITIZER_FLAGS "-fsanitize=address,undefined -fno-omit-frame-pointer -g")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${SANITIZER_FLAGS}")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${SANITIZER_FLAGS}")
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} ${SANITIZER_FLAGS}")
endif()

# The tests drive the library directly; skip the host examples so a PortAudio
# install isn't required to run the test suite.
set(MICRO_DECODER_BUILD_EXAMPLES OFF CACHE BOOL "Tests do not need the host examples")

# Build micro-decoder as a standard host library
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/.. micro-decoder-build)

add_executable(test_micro_decoder test_micro_decoder.cpp)
target_link_libraries(test_micro_decoder PRIVATE micro_decoder)
# The tests exercise internal pieces (TransferBuffer, RingBuffer, EventFlags)
# alongside the public API
target_include_directories(test_micro_decoder PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
target_compile_options(test_micro_decoder PRIVATE
    -Wall
    -Wextra
    -Wshadow
    -Wimplicit-fallthrough
    $<$<BOOL:${ENABLE_WERROR}>:-Werror>
)

find_package(Threads REQUIRED)
target_link_libraries(test_micro_decoder PRIVATE Threads::Threads)

enable_testing()

set(DECODER_TESTS
    detect_content_type
    detect_url_extension
    stream_info
    transfer_buffer
    ring_buffer_basic
    ring_buffer_threaded
    event_flags
    play_buffer_validation
    play_buffer_passthrough
    play_buffer_backpressure
    play_buffer_stop_deferred
    stop_from_callback
    play_buffer_failure_and_reuse
    destructor_while_playing
    play_url_stream
    play_url_extension_fallback
    play_url_error_states
    play_url_stop_mid_stream
    play_url_reader_stops_on_decoder_exit
)

foreach(test_name IN LISTS DECODER_TESTS)
    add_test(NAME ${test_name} COMMAND test_micro_decoder ${test_name})
    # Every wait in the tests is individually bounded; this is a backstop so a
    # regression that hangs a thread join can never wedge CI.
    set_tests_properties(${test_name} PROPERTIES TIMEOUT 120)
endforeach()
