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

#include "audio_reader.h"

#include "platform/logging.h"

#include <algorithm>

namespace micro_decoder {

// ============================================================================
// Static constants
// ============================================================================

static constexpr const char* TAG = "micro_decoder.audio_reader";

/// @brief Receive buffer assumed when the caller lets the platform choose one
/// Matches the ESP-IDF HTTP client default. Guessing low is the safe direction: it only
/// costs an extra run() call per receive buffer, while guessing high would let one read
/// block for several read timeouts.
static constexpr size_t DEFAULT_RX_BUFFER_SIZE = 512;

/// @brief Chooses the largest read to request per run() call
/// The HTTP client keeps issuing socket reads until the request is filled, so asking for
/// more than one receive buffer lets a single read block for several read timeouts. A zero
/// receive buffer size means the platform picks it, so assume the platform default rather
/// than the whole transfer buffer, which would drop the bound entirely.
/// @param transfer_buffer_size Size of the reader's staging buffer in bytes
/// @param http_rx_buffer_size Size of the HTTP client's receive buffer in bytes
/// @return Maximum number of bytes to request from a single read
static size_t compute_max_read_size(size_t transfer_buffer_size, size_t http_rx_buffer_size) {
    size_t rx_buffer_size = http_rx_buffer_size == 0 ? DEFAULT_RX_BUFFER_SIZE : http_rx_buffer_size;
    return std::min(rx_buffer_size, transfer_buffer_size);
}

// ============================================================================
// AudioReader
// ============================================================================

AudioReader::AudioReader(size_t transfer_buffer_size, uint32_t http_timeout_ms,
                         uint32_t http_read_timeout_ms, uint32_t write_timeout_ms,
                         size_t http_rx_buffer_size, std::string user_agent,
                         std::string ca_certificate)
    : max_read_size_(compute_max_read_size(transfer_buffer_size, http_rx_buffer_size)),
      write_timeout_ms_(write_timeout_ms),
      allocation_ok_(this->transfer_buffer_.allocate(transfer_buffer_size)) {
    this->request_.user_agent = std::move(user_agent);
    this->request_.ca_certificate = std::move(ca_certificate);
    this->request_.rx_buffer_size = http_rx_buffer_size;
    this->request_.connect_timeout_ms = http_timeout_ms;
    this->request_.read_timeout_ms = http_read_timeout_ms;
}

AudioReader::~AudioReader() {
    if (this->client_) {
        this->client_->close();
    }
}

bool AudioReader::start_url(const std::string& url) {
    if (!this->allocation_ok_) {
        MD_LOGE(TAG, "Transfer buffer allocation failed");
        return false;
    }
    this->file_type_ = AudioFileType::NONE;
    this->client_ = create_http_client();
    this->request_.url = url;

    if (!this->client_->open(this->request_)) {
        MD_LOGE(TAG, "Failed to connect to URL: %s", url.c_str());
        this->client_.reset();
        return false;
    }

    const HttpResponse& resp = this->client_->response_info();

    // Detect file type from Content-Type, then URL extension
    this->file_type_ = detect_audio_file_type(
        resp.content_type.empty() ? nullptr : resp.content_type.c_str(), url.c_str());

    if (this->file_type_ == AudioFileType::NONE) {
        MD_LOGE(TAG, "Could not determine audio file type from URL or Content-Type");
        this->client_->close();
        this->client_.reset();
        return false;
    }

    MD_LOGI(TAG, "Streaming %s (%s)", url.c_str(), audio_file_type_to_string(this->file_type_));
    return true;
}

AudioReaderState AudioReader::run() {
    if (!this->client_) {
        return AudioReaderState::FAILED;
    }

    // Drain the transfer buffer completely before reading more HTTP data. No memmove
    // compaction is performed: the buffer resets its start pointer once fully drained.
    // This avoids expensive memmove calls on ESP32 and is acceptable because the ring
    // buffer is the real data buffer that absorbs minor read/write timing differences.
    while (this->transfer_buffer_.available() > 0) {
        size_t written =
            this->ring_buffer_->write(this->transfer_buffer_.get_buffer_start(),
                                      this->transfer_buffer_.available(), this->write_timeout_ms_);
        if (written > 0) {
            this->transfer_buffer_.decrease_length(written);
        } else {
            // Ring buffer full, try again next call
            return AudioReaderState::READING;
        }
    }

    // Ask for at most one receive buffer's worth. The HTTP client keeps issuing socket reads
    // until the request is filled, so a larger ask lets a single call block for several read
    // timeouts, and the stop request can only be seen between calls.
    size_t space = std::min(this->transfer_buffer_.free(), this->max_read_size_);
    int received = this->client_->read(this->transfer_buffer_.get_buffer_end(), space);

    if (received < 0) {
        MD_LOGE(TAG, "HTTP read error");
        this->client_->close();
        this->client_.reset();
        return AudioReaderState::FAILED;
    }

    if (received > 0) {
        this->transfer_buffer_.increase_length(static_cast<size_t>(received));
        return AudioReaderState::READING;
    }

    // received == 0: no new data this call; check if the transfer is fully done
    if (this->client_->is_complete()) {
        MD_LOGD(TAG, "HTTP read complete");
        this->client_->close();
        this->client_.reset();
        return AudioReaderState::FINISHED;
    }

    return AudioReaderState::IDLE;
}

}  // namespace micro_decoder
