// Copyright 2026 Kevin Ahrendt
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "micro_decoder/decoder_source.h"

#include "audio_decoder.h"
#include "audio_reader.h"
#include "platform/event_flags.h"
#include "platform/http_client.h"
#include "platform/logging.h"
#include "platform/thread.h"
#include "ring_buffer.h"

#include <atomic>
#include <string>

namespace micro_decoder {

static constexpr const char* TAG = "micro_decoder.decoder_source";

// ============================================================================
// Event flag bits
// ============================================================================

static constexpr uint32_t FLAG_READER_READY = (1U << 0);
static constexpr uint32_t FLAG_READER_FINISHED = (1U << 1);
static constexpr uint32_t FLAG_READER_ERROR = (1U << 2);
static constexpr uint32_t FLAG_COMMAND_STOP = (1U << 3);
static constexpr uint32_t FLAG_DECODER_STARTED = (1U << 4);
static constexpr uint32_t FLAG_DECODER_FINISHED = (1U << 5);
static constexpr uint32_t FLAG_DECODER_FAILED = (1U << 6);

static constexpr uint32_t ALL_FLAGS = FLAG_READER_READY | FLAG_READER_FINISHED | FLAG_READER_ERROR |
                                      FLAG_COMMAND_STOP | FLAG_DECODER_STARTED |
                                      FLAG_DECODER_FINISHED | FLAG_DECODER_FAILED;

// ============================================================================
// DecoderSource::Impl
// ============================================================================

/// @brief Internal implementation state for DecoderSource (pImpl)
struct DecoderSource::Impl {
    // Struct fields
    DecoderConfig config;
    EventFlags event_flags;
    RingBuffer ring_buffer;
    /// @brief URL for the active URL-sourced playback
    /// Owned here rather than captured per thread so that starting a thread needs no
    /// allocation. Only written by play_url(), which runs after both threads are joined.
    std::string url;

    /// @brief Worker threads, declared last so their destructors run first
    /// Members are destroyed in reverse declaration order, so the backstop join in
    /// ~PlatformThread has to happen before the event flags, ring buffer, and URL the threads
    /// touch are gone. Everything declared after these is trivially destructible.
    /// @note stop() still joins both explicitly; this only decides what an unjoined thread
    /// would find still alive.
    PlatformThread decoder_thread;
    PlatformThread reader_thread;

    // Pointer fields
    /// @brief Source buffer for the active buffer-sourced playback; owned by the caller
    const uint8_t* buffer_data{nullptr};
    std::atomic<DecoderListener*> listener{nullptr};

    // size_t fields
    size_t buffer_length{0};

    // 8-bit fields
    std::atomic<DecoderState> decoder_state{DecoderState::IDLE};
    std::atomic<AudioFileType> detected_file_type{AudioFileType::NONE};

    // bool fields
    bool initialized_{false};
    std::atomic<bool> pending_state_notification_{false};

    explicit Impl(const DecoderConfig& cfg)
        : config(cfg), initialized_(this->event_flags.create()) {
        // Claim the ring buffer up front so playback cannot fail later on a fragmented
        // heap. A failure here is not fatal: ensure_ring_buffer() retries at play time.
        if (this->config.persistent_ring_buffer &&
            !this->ring_buffer.create(this->config.ring_buffer_size)) {
            MD_LOGW(TAG, "Failed to preallocate the ring buffer; will retry on playback");
        }
    }

    /// @brief Makes an empty ring buffer available, allocating one if none is held
    /// @note A retained buffer still holds whatever the previous playback left unread, so it
    /// is reset rather than reused as-is.
    /// @return true if the ring buffer is ready for use
    bool ensure_ring_buffer() {
        if (this->ring_buffer.allocated()) {
            return this->ring_buffer.reset();
        }
        return this->ring_buffer.create(this->config.ring_buffer_size);
    }

    /// @brief Frees the ring buffer unless it is configured to persist
    /// @note All threads must be joined first; they read and write the ring buffer.
    void release_ring_buffer() {
        if (!this->config.persistent_ring_buffer) {
            this->ring_buffer.release();
        }
    }

    /// @brief Updates the decoder state and notifies the listener immediately
    /// Only called from pump_events(), which runs on the user's thread via loop().
    void set_state(DecoderState s) {
        this->decoder_state.store(s, std::memory_order_release);
        DecoderListener* l = this->listener.load(std::memory_order_acquire);
        if (l != nullptr) {
            l->on_state_change(s);
        }
    }

    /// @brief Updates the decoder state without notifying the listener.
    /// The notification is deferred to the next pump_events() call via loop().
    void store_state(DecoderState s) {
        this->decoder_state.store(s, std::memory_order_release);
        this->pending_state_notification_.store(true, std::memory_order_release);
    }

    /// @brief Fires a state change deferred by stop() or a play_*() error path
    /// @note Touches no event flags, so it is safe even when they were never created.
    void drain_pending_notification() {
        if (!this->pending_state_notification_.exchange(false, std::memory_order_acq_rel)) {
            return;
        }
        DecoderState s = this->decoder_state.load(std::memory_order_acquire);
        DecoderListener* l = this->listener.load(std::memory_order_acquire);
        if (l != nullptr) {
            l->on_state_change(s);
        }
    }

    /// @brief Reads pending event flags and fires listener callbacks
    /// Called from DecoderSource::loop() on the user's thread.
    void pump_events() {
        uint32_t flags = this->event_flags.get();

        // Check FAILED first -- if the decoder failed, skip STARTED/FINISHED to avoid
        // spurious PLAYING or IDLE callbacks before the FAILED transition.
        bool failed = (flags & FLAG_DECODER_FAILED) != 0;
        bool started = !failed && (flags & FLAG_DECODER_STARTED) != 0;
        bool finished = !failed && (flags & FLAG_DECODER_FINISHED) != 0;

        // Clear everything this call consumes before running any callback. A listener may
        // call stop() or play_url() from on_state_change(), and clearing afterwards would
        // discard flags belonging to the playback it just started.
        this->event_flags.clear(
            flags & (FLAG_DECODER_FAILED | FLAG_DECODER_STARTED | FLAG_DECODER_FINISHED));

        if (failed) {
            this->set_state(DecoderState::FAILED);
            return;
        }

        if (started &&
            this->decoder_state.load(std::memory_order_acquire) != DecoderState::FAILED) {
            this->set_state(DecoderState::PLAYING);
        }

        if (finished &&
            this->decoder_state.load(std::memory_order_acquire) == DecoderState::PLAYING) {
            this->set_state(DecoderState::IDLE);
        }
    }

    // ========================================
    // HTTP reader thread body
    // ========================================

    /// @brief pthread trampoline for the reader thread
    /// @param arg Pointer to the owning Impl
    /// @return Always nullptr; the result is unused
    static void* reader_entry(void* arg) {
        static_cast<Impl*>(arg)->reader_thread_func();
        return nullptr;
    }

    /// @brief Polled by the HTTP client while it blocks connecting
    /// @param arg Pointer to the owning Impl
    /// @return true once a stop has been requested
    static bool reader_cancelled(void* arg) {
        auto* impl = static_cast<Impl*>(arg);
        return (impl->event_flags.get() & FLAG_COMMAND_STOP) != 0;
    }

    /// @brief Reader thread entry point that streams from the URL in this->url
    void reader_thread_func() {
        AudioReader reader(this->config.transfer_buffer_size, this->config.http_timeout_ms,
                           this->config.http_read_timeout_ms, this->config.reader_write_timeout_ms,
                           this->config.http_rx_buffer_size, this->config.http_user_agent,
                           this->config.http_ca_certificate);
        reader.set_sink(&this->ring_buffer);
        // Connecting blocks for far longer than a body read, so let it be abandoned too
        reader.set_cancel_check(&Impl::reader_cancelled, this);

        if (!reader.start_url(this->url)) {
            MD_LOGE(TAG, "Reader failed to open URL");
            this->event_flags.set(FLAG_READER_ERROR);
            return;
        }

        // Signal the file type to the decoder
        this->detected_file_type.store(reader.file_type(), std::memory_order_release);
        this->event_flags.set(FLAG_READER_READY);

        while (true) {
            // Check for stop command
            if (this->event_flags.get() & FLAG_COMMAND_STOP) {
                break;
            }

            AudioReaderState rs = reader.run();
            if (rs == AudioReaderState::FINISHED) {
                MD_LOGD(TAG, "Reader finished");
                this->event_flags.set(FLAG_READER_FINISHED);
                break;
            }
            if (rs == AudioReaderState::FAILED) {
                MD_LOGE(TAG, "Reader error");
                this->event_flags.set(FLAG_READER_ERROR);
                break;
            }

            if (rs == AudioReaderState::IDLE) {
                // No data available from HTTP; yield to avoid a tight spin.
                // Wakes immediately on a stop command.
                this->event_flags.wait(FLAG_COMMAND_STOP, false, false,
                                       this->config.reader_write_timeout_ms);
            }
        }
    }

    // ========================================
    // Decoder thread body (HTTP path)
    // ========================================

    /// @brief pthread trampoline for the URL-sourced decoder thread
    /// @param arg Pointer to the owning Impl
    /// @return Always nullptr; the result is unused
    static void* decoder_url_entry(void* arg) {
        static_cast<Impl*>(arg)->decoder_thread_func_url();
        return nullptr;
    }

    /// @brief pthread trampoline for the buffer-sourced decoder thread
    /// @param arg Pointer to the owning Impl
    /// @return Always nullptr; the result is unused
    static void* decoder_buffer_entry(void* arg) {
        auto* impl = static_cast<Impl*>(arg);
        impl->run_decoder_buffer(impl->buffer_data, impl->buffer_length,
                                 impl->detected_file_type.load(std::memory_order_acquire));
        return nullptr;
    }

    /// @brief Decoder thread entry point for URL-based playback
    void decoder_thread_func_url() {
        // Wait for the reader to signal file type or error. http_timeout_ms bounds one connect
        // and header-fetch cycle, and the reader spends up to HTTP_MAX_CONNECT_ATTEMPTS of
        // them, so waiting for a single cycle would give up partway through a legitimate
        // retry sequence and fail a stream the reader was still fetching.
        static constexpr uint32_t WAIT_MARGIN_MS = 2000;
        uint32_t connect_budget_ms = this->config.http_timeout_ms * HTTP_MAX_CONNECT_ATTEMPTS;
        uint32_t bits =
            this->event_flags.wait(FLAG_READER_READY | FLAG_READER_ERROR | FLAG_COMMAND_STOP, false,
                                   false, connect_budget_ms + WAIT_MARGIN_MS);

        if (bits & FLAG_COMMAND_STOP) {
            return;
        }
        if (bits & FLAG_READER_ERROR) {
            this->event_flags.set(FLAG_DECODER_FAILED);
            return;
        }
        if ((bits & FLAG_READER_READY) == 0) {
            // The wait timed out. Falling through would decode with whatever file type was
            // left over from an earlier playback.
            MD_LOGE(TAG, "Timed out waiting for the reader to report a file type");
            this->event_flags.set(FLAG_DECODER_FAILED);
            return;
        }

        AudioFileType file_type = this->detected_file_type.load(std::memory_order_acquire);

        this->run_decoder(file_type);
    }

    // ========================================
    // Common decoder logic - HTTP streaming source
    // ========================================

    /// @brief Runs the decode loop for URL-sourced audio
    void run_decoder(AudioFileType file_type) {
        AudioDecoder decoder(this->config.transfer_buffer_size);
        decoder.set_source(&this->ring_buffer);

        if (!decoder.start(file_type)) {
            MD_LOGE(TAG, "Decoder start failed for %s", audio_file_type_to_string(file_type));
            this->event_flags.set(FLAG_DECODER_FAILED);
            return;
        }

        this->event_flags.set(FLAG_DECODER_STARTED);

        while (true) {
            uint32_t flags = this->event_flags.get();

            if (flags & FLAG_COMMAND_STOP) {
                break;
            }

            bool reader_done = (flags & (FLAG_READER_FINISHED | FLAG_READER_ERROR)) != 0;

            AudioDecoderState ds =
                decoder.decode(reader_done, this->listener.load(std::memory_order_acquire),
                               this->config.audio_write_timeout_ms);

            if (ds == AudioDecoderState::FINISHED) {
                if (flags & FLAG_READER_ERROR) {
                    MD_LOGE(TAG, "Decode drained after reader error");
                    this->event_flags.set(FLAG_DECODER_FAILED);
                } else {
                    MD_LOGD(TAG, "Decode finished");
                    this->event_flags.set(FLAG_DECODER_FINISHED);
                }
                return;
            }
            if (ds == AudioDecoderState::FAILED) {
                MD_LOGE(TAG, "Decode failed");
                this->event_flags.set(FLAG_DECODER_FAILED);
                return;
            }
        }
        // Stopped by command; stop() handles the IDLE transition
    }

    // ========================================
    // Common decoder logic - in-memory source
    // ========================================

    /// @brief Runs the decode loop for buffer-sourced audio
    void run_decoder_buffer(const uint8_t* data, size_t length, AudioFileType file_type) {
        AudioDecoder decoder(this->config.transfer_buffer_size);
        decoder.set_source(data, length);

        if (!decoder.start(file_type)) {
            MD_LOGE(TAG, "Decoder start failed for %s", audio_file_type_to_string(file_type));
            this->event_flags.set(FLAG_DECODER_FAILED);
            return;
        }

        this->event_flags.set(FLAG_DECODER_STARTED);

        while (true) {
            if (this->event_flags.get() & FLAG_COMMAND_STOP) {
                break;
            }

            AudioDecoderState ds =
                decoder.decode(true, this->listener.load(std::memory_order_acquire),
                               this->config.audio_write_timeout_ms);

            if (ds == AudioDecoderState::FINISHED) {
                MD_LOGD(TAG, "Decode finished");
                this->event_flags.set(FLAG_DECODER_FINISHED);
                return;
            }
            if (ds == AudioDecoderState::FAILED) {
                MD_LOGE(TAG, "Decode failed");
                this->event_flags.set(FLAG_DECODER_FAILED);
                return;
            }
        }
        // Stopped by command; stop() handles the IDLE transition
    }
};

// ============================================================================
// DecoderSource
// ============================================================================

DecoderSource::DecoderSource(const DecoderConfig& config)
    : impl_(std::make_unique<DecoderSource::Impl>(config)) {}

DecoderSource::~DecoderSource() {
    this->stop();
}

void DecoderSource::set_listener(DecoderListener* listener) {
    this->impl_->listener.store(listener, std::memory_order_release);
}

bool DecoderSource::play_url(const std::string& url) {
    if (!this->impl_->initialized_) {
        MD_LOGE(TAG, "Not initialized (event flags allocation failed)");
        this->impl_->store_state(DecoderState::FAILED);
        return false;
    }

    this->stop();

    if (!this->impl_->ensure_ring_buffer()) {
        MD_LOGE(TAG, "Failed to allocate ring buffer");
        this->impl_->store_state(DecoderState::FAILED);
        return false;
    }

    // Clear event flags and pending notifications from any previous run
    this->impl_->event_flags.clear(ALL_FLAGS);
    this->impl_->pending_state_notification_.store(false, std::memory_order_release);

    // Drop any type left over from a previous playback; the reader publishes the real one
    this->impl_->detected_file_type.store(AudioFileType::NONE, std::memory_order_release);

    // Copy the URL before starting the reader; the thread reads it from the Impl
    this->impl_->url = url;

    // Spawn reader thread. Always in internal RAM for lwip settings compatibility
    ThreadConfig reader_config{"md_reader", this->impl_->config.reader_stack_size,
                               this->impl_->config.reader_priority, false};
    if (!this->impl_->reader_thread.start(reader_config, &Impl::reader_entry, this->impl_.get())) {
        MD_LOGE(TAG, "Failed to start the reader thread");
        this->impl_->release_ring_buffer();
        this->impl_->store_state(DecoderState::FAILED);
        return false;
    }

    // Spawn decoder thread
    ThreadConfig decoder_config{"md_decoder", this->impl_->config.decoder_stack_size,
                                this->impl_->config.decoder_priority,
                                this->impl_->config.decoder_stack_in_psram};
    if (!this->impl_->decoder_thread.start(decoder_config, &Impl::decoder_url_entry,
                                           this->impl_.get())) {
        MD_LOGE(TAG, "Failed to start the decoder thread");
        // Unwind the reader thread that is already running
        this->impl_->event_flags.set(FLAG_COMMAND_STOP);
        this->impl_->reader_thread.join();
        this->impl_->event_flags.clear(ALL_FLAGS);
        this->impl_->release_ring_buffer();
        this->impl_->store_state(DecoderState::FAILED);
        return false;
    }

    return true;
}

bool DecoderSource::play_buffer(const uint8_t* data, size_t length, AudioFileType type) {
    if (data == nullptr || length == 0) {
        MD_LOGE(TAG, "Null or empty buffer");
        return false;
    }

    if (type == AudioFileType::NONE) {
        MD_LOGE(TAG, "Unsupported audio file type");
        return false;
    }

    if (!this->impl_->initialized_) {
        MD_LOGE(TAG, "Not initialized (event flags allocation failed)");
        this->impl_->store_state(DecoderState::FAILED);
        return false;
    }

    this->stop();

    // Clear event flags and pending notifications from any previous run
    this->impl_->event_flags.clear(ALL_FLAGS);
    this->impl_->pending_state_notification_.store(false, std::memory_order_release);

    // Publish the source before starting the decoder; the thread reads it from the Impl
    this->impl_->buffer_data = data;
    this->impl_->buffer_length = length;
    this->impl_->detected_file_type.store(type, std::memory_order_release);

    // Spawn decoder thread only
    ThreadConfig decoder_config{"md_decoder", this->impl_->config.decoder_stack_size,
                                this->impl_->config.decoder_priority,
                                this->impl_->config.decoder_stack_in_psram};
    if (!this->impl_->decoder_thread.start(decoder_config, &Impl::decoder_buffer_entry,
                                           this->impl_.get())) {
        MD_LOGE(TAG, "Failed to start the decoder thread");
        this->impl_->store_state(DecoderState::FAILED);
        return false;
    }

    return true;
}

void DecoderSource::stop() {
    // Nothing was ever started without event flags, and touching them would dereference a
    // null handle
    if (!this->impl_->initialized_) {
        return;
    }

    DecoderState current = this->impl_->decoder_state.load(std::memory_order_acquire);

    // Signal all threads to stop
    this->impl_->event_flags.set(FLAG_COMMAND_STOP);

    if (this->impl_->reader_thread.joinable()) {
        this->impl_->reader_thread.join();
    }
    if (this->impl_->decoder_thread.joinable()) {
        this->impl_->decoder_thread.join();
    }

    // Clear all pending events; threads are done, no more will arrive
    this->impl_->event_flags.clear(ALL_FLAGS);

    // Safe now that both threads are joined; nothing else touches the ring buffer
    this->impl_->release_ring_buffer();

    if (current == DecoderState::PLAYING || current == DecoderState::FAILED) {
        this->impl_->store_state(DecoderState::IDLE);
    }
}

void DecoderSource::loop() {
    // Deferred notifications do not depend on the event flags, so they still have to be
    // delivered when initialization failed -- that is exactly when a FAILED state is waiting
    this->impl_->drain_pending_notification();

    // pump_events() reads the event flags, whose handle was never created
    if (!this->impl_->initialized_) {
        return;
    }
    this->impl_->pump_events();
}

DecoderState DecoderSource::state() const {
    return this->impl_->decoder_state.load(std::memory_order_acquire);
}

}  // namespace micro_decoder
