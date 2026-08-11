# ESP-IDF specific configuration for micro-decoder

function(micro_decoder_configure_esp_idf TARGET_LIB COMPONENT_DIR)
    target_compile_features(${TARGET_LIB} PUBLIC cxx_std_17)

    # Strict warnings on the ESP-IDF build too, not just on host: this component is what ships to
    # hardware and is the only place src/esp/ and the #ifdef ESP_PLATFORM branches ever compile,
    # so the host build cannot catch their warnings. -Werror stays gated behind ENABLE_WERROR
    # (default off), exactly as on host: a consumer building from the registry gets warnings only,
    # while CI's pinned ESP build (examples/decode_benchmark) passes -DENABLE_WERROR=ON and turns
    # them into errors.
    target_compile_options(${TARGET_LIB} PRIVATE
        -Wall
        -Wextra
        -Wshadow
        -Wnon-virtual-dtor
        -Wconversion
        -Wsign-conversion
        -Wdouble-promotion
        -Wformat=2
        -Wimplicit-fallthrough
        # Any function not declared in a header must be static; keeps
        # -Wunused-function able to see dead internal functions. Clang and GCC
        # spell the C++ variant of this check differently.
        $<$<CXX_COMPILER_ID:Clang,AppleClang>:-Wmissing-prototypes>
        $<$<CXX_COMPILER_ID:GNU>:-Wmissing-declarations>
        # Two flags from the host block are deliberately absent, each because ESP-IDF's own
        # headers trip it inside our translation units and a diagnostic raised in a system
        # header expansion cannot be silenced from here:
        #   -Wold-style-cast: pdMS_TO_TICKS and pdFALSE in freertos/projdefs.h cast C-style.
        #   -Wpedantic: newlib's sys/cdefs.h uses #include_next, a GCC extension.
        # Both still apply on the host build, which compiles every source shared with it.
        $<$<BOOL:${ENABLE_WERROR}>:-Werror>
    )

    # Enable debug-level logging for this library regardless of ESP-IDF's global default.
    # ESP-IDF defaults to ERROR in ESPHome, which compiles out all INFO/DEBUG/WARN logs.
    # LOG_LOCAL_LEVEL overrides the compile-time maximum for this component only.
    target_compile_definitions(${TARGET_LIB} PRIVATE
        LOG_LOCAL_LEVEL=ESP_LOG_DEBUG
    )

    # =========================================================================
    # Codec options: translate Kconfig to compiler defines
    # =========================================================================
    if(CONFIG_MICRO_DECODER_CODEC_FLAC)
        target_compile_definitions(${TARGET_LIB} PUBLIC MICRO_DECODER_CODEC_FLAC=1)
    endif()
    if(CONFIG_MICRO_DECODER_CODEC_MP3)
        target_compile_definitions(${TARGET_LIB} PUBLIC MICRO_DECODER_CODEC_MP3=1)
    endif()
    if(CONFIG_MICRO_DECODER_CODEC_OPUS)
        target_compile_definitions(${TARGET_LIB} PUBLIC MICRO_DECODER_CODEC_OPUS=1)
    endif()
    if(CONFIG_MICRO_DECODER_CODEC_VORBIS)
        target_compile_definitions(${TARGET_LIB} PUBLIC MICRO_DECODER_CODEC_VORBIS=1)
    endif()
    if(CONFIG_MICRO_DECODER_CODEC_WAV)
        target_compile_definitions(${TARGET_LIB} PUBLIC MICRO_DECODER_CODEC_WAV=1)
    endif()
endfunction()
