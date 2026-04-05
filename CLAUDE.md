# microDecoder

## Build

```bash
mkdir build && cmake -S . -B build && cmake --build build
```

## Architecture

```api
DecoderSource          -- public entry point (pImpl)
├── AudioReader        -- HTTP streaming -> RingBuffer  [reader thread]
└── AudioDecoder       -- RingBuffer / const buf -> PCM [decoder thread]
    └── DecoderListener::on_audio_write()  -- caller-supplied callback
```

**Data flow (HTTP):** `HttpClient -> TransferBuffer -> RingBuffer -> AudioDecoder -> DecoderListener`

**Data flow (buffer):** `const uint8_t* -> AudioDecoder -> DecoderListener`

Threads are short-lived: spawned by `play_url()` / `play_buffer()`, joined by `stop()` or the destructor. `EventFlags` coordinate the reader and decoder threads.

**Callback threading:** `on_stream_info()` and `on_audio_write()` fire on the decoder thread. `on_state_change()` fires exclusively on the caller's thread via `loop()`. Worker threads set event flags; `loop()` reads them and dispatches state callbacks. State changes from `stop()` and error paths are deferred to the next `loop()` call.

## File layout

```api
include/micro_decoder/   - public headers
  decoder_source.h       - DecoderSource class
  types.h                - AudioFileType, AudioStreamInfo, DecoderConfig, DecoderListener, DecoderState

src/
  decoder_source.cpp     - thread lifecycle and orchestration
  audio_decoder.{h,cpp}  - FLAC/MP3/Opus/WAV decode loop
  audio_reader.{h,cpp}   - HTTP → RingBuffer
  ring_buffer.{h,cpp}    - owns storage; wraps SpscRingBuffer
  md_transfer_buffer.{h,cpp} - flat staging buffer with start-pointer + length cursors
  types.cpp              - AudioStreamInfo, detect_audio_file_type, audio_file_type_to_string
  platform/
    logging.h            - MD_LOG{E,W,I,D} → ESP_LOG* or fprintf
    memory.h             - platform_malloc/free + PlatformBuffer RAII
    thread.h             - platform_configure_thread (ESP: esp_pthread_set_cfg / host: no-op)
    event_flags.h        - EventFlags (ESP: xEventGroup / host: mutex+cv)
    spsc_ring_buffer.h   - SpscRingBuffer (ESP: FreeRTOS BYTEBUF / host: mutex+cv)
    http_client.h        - HttpClient abstract interface
  esp/
    http_client.cpp      - esp_http_client implementation
  host/
    http_client.cpp      - libcurl implementation

cmake/
  sources.cmake          - micro_decoder_get_sources() function
  host.cmake             - micro_decoder_configure_host(); FetchContent deps
  esp-idf.cmake          - micro_decoder_configure_esp_idf(); compiler flags

host_examples/
  common/portaudio_sink.{h,cpp}   - DecoderListener implementation using PortAudio
  basic_player/main.cpp            - CLI: play a file or URL
```

## Namespace

All symbols live in `namespace micro_decoder`.

## Coding conventions

- Member variables use `this->` prefix
- Struct/class member ordering: structs → pointers → size_t → smaller integer types → bool
- Whenever all codecs appear together (enums, switch cases, includes, cmake options, docs), list them in alphabetical order: FLAC, MP3, Opus, WAV
- Platform-specific code guarded by `#ifdef ESP_PLATFORM` / `#else`
- Internal headers live in `src/`; only `include/micro_decoder/` is public
