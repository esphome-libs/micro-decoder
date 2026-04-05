# ESP-IDF specific configuration for micro-decoder

function(micro_decoder_configure_esp_idf TARGET_LIB COMPONENT_DIR)
    target_compile_features(${TARGET_LIB} PUBLIC cxx_std_17)
    target_compile_options(${TARGET_LIB} PRIVATE
        -Wall
        -Wextra
        -Wshadow
        -Wnon-virtual-dtor
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
    if(CONFIG_MICRO_DECODER_CODEC_WAV)
        target_compile_definitions(${TARGET_LIB} PUBLIC MICRO_DECODER_CODEC_WAV=1)
    endif()
endfunction()
