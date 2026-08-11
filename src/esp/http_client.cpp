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

#include "platform/http_client.h"

#include "platform/logging.h"
#include <esp_http_client.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
#include <esp_crt_bundle.h>
#endif

#include <cstring>
#include <string>

namespace micro_decoder {

static constexpr const char* TAG = "micro_decoder.http_client";

static constexpr uint8_t MAX_REDIRECTIONS = 5;

/// @brief Returns how much of a tick deadline is left, in milliseconds
/// @note The signed difference keeps the comparison correct across a tick counter wrap.
/// @param deadline_ticks Tick count the budget expires at
/// @return Milliseconds remaining, or 0 once the deadline has passed
static uint32_t remaining_budget_ms(TickType_t deadline_ticks) {
    int32_t remaining_ticks = static_cast<int32_t>(deadline_ticks - xTaskGetTickCount());
    if (remaining_ticks <= 0) {
        return 0;
    }
    return pdTICKS_TO_MS(static_cast<TickType_t>(remaining_ticks));
}

/// @brief Returns true if the URL begins with an "https:" scheme (case-insensitive)
static bool url_has_https_scheme(const std::string& url) {
    static constexpr char SCHEME[] = "https:";
    static constexpr size_t SCHEME_LEN = sizeof(SCHEME) - 1;
    if (url.size() < SCHEME_LEN) {
        return false;
    }
    for (size_t i = 0; i < SCHEME_LEN; ++i) {
        char c = url[i];
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c + ('a' - 'A'));
        }
        if (c != SCHEME[i]) {
            return false;
        }
    }
    return true;
}

// ============================================================================
// EspHttpClient
// ============================================================================

/// @brief ESP-IDF HttpClient implementation using esp_http_client
class EspHttpClient final : public HttpClient {
public:
    EspHttpClient() = default;

    ~EspHttpClient() override {
        this->close();
    }

    /// @brief Opens an HTTP connection and begins streaming via esp_http_client
    /// @param request Connection settings, timeouts, and cancellation hook
    /// @return true on success (2xx status), false on connection error or non-2xx status
    bool open(const HttpRequest& request) override {
        this->close();
        this->complete_ = false;
        this->response_ = HttpResponse{};

        esp_http_client_config_t cfg = {};
        cfg.url = request.url.c_str();
        cfg.disable_auto_redirect = false;
        cfg.max_redirection_count = MAX_REDIRECTIONS;
        cfg.event_handler = http_event_handler;
        cfg.user_data = this;
        cfg.buffer_size = static_cast<int>(request.rx_buffer_size);
        cfg.keep_alive_enable = true;
        cfg.timeout_ms = static_cast<int>(request.connect_timeout_ms);
        if (!request.user_agent.empty()) {
            cfg.user_agent = request.user_agent.c_str();
        }

        if (url_has_https_scheme(request.url)) {
            if (!request.ca_certificate.empty()) {
                cfg.cert_pem = request.ca_certificate.c_str();
            } else {
#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
                cfg.crt_bundle_attach = esp_crt_bundle_attach;
#endif
            }
        }

        this->client_ = esp_http_client_init(&cfg);
        if (this->client_ == nullptr) {
            MD_LOGE(TAG, "esp_http_client_init failed");
            return false;
        }

        // One budget for the whole exchange, redirects included. Every hop draws from what is
        // left of it, so a chain of slow hops cannot stack a fresh timeout each.
        const uint32_t budget_ms = http_connect_budget_ms(request.connect_timeout_ms);
        const TickType_t deadline_ticks = xTaskGetTickCount() + pdMS_TO_TICKS(budget_ms);

        if (this->connect_and_fetch_headers(request, deadline_ticks) < 0) {
            this->cleanup();
            return false;
        }

        int status = esp_http_client_get_status_code(this->client_);

        // Follow redirects manually. Note cfg.disable_auto_redirect only applies to
        // esp_http_client_perform(), not the open/fetch_headers path used here.
        uint8_t redirect_count = 0;
        while (esp_http_client_set_redirection(this->client_) == ESP_OK &&
               redirect_count < MAX_REDIRECTIONS) {
            if (this->connect_and_fetch_headers(request, deadline_ticks) < 0) {
                this->cleanup();
                return false;
            }
            status = esp_http_client_get_status_code(this->client_);
            ++redirect_count;
        }

        static constexpr int HTTP_OK_MIN = 200;
        static constexpr int HTTP_OK_MAX = 300;
        if (status < HTTP_OK_MIN || status >= HTTP_OK_MAX) {
            MD_LOGE(TAG, "HTTP error: %d", status);
            this->cleanup();
            return false;
        }

        this->response_.status_code = status;
        MD_LOGD(TAG, "Connected: status=%d content-type='%s'", status,
                this->response_.content_type.c_str());
        return true;
    }

    /// @brief Returns the HTTP response metadata captured during the event callback
    /// @note Valid after a successful open()
    /// @return HttpResponse with the HTTP status code and Content-Type string
    const HttpResponse& response_info() const override {
        return this->response_;
    }

    /// @brief Reads the next chunk of response body data
    /// @param[out] buffer Destination buffer for received bytes
    /// @param max_length Maximum number of bytes to read
    /// @return Bytes read (>0), 0 when no data is available (check is_complete() for EOF), -1 on
    /// error
    int read(uint8_t* buffer, size_t max_length) override {
        if (this->client_ == nullptr) {
            return -1;
        }
        if (this->complete_) {
            return 0;
        }
        int received = esp_http_client_read(this->client_, reinterpret_cast<char*>(buffer),
                                            static_cast<int>(max_length));
        if (received < 0) {
            if (received == -ESP_ERR_HTTP_EAGAIN) {
                return 0;  // Timeout, retry
            }
            return -1;
        }
        if (received == 0 && esp_http_client_is_complete_data_received(this->client_)) {
            this->complete_ = true;
        }
        return received;
    }

    /// @brief Returns true when the HTTP response body has been fully received
    /// @return true when all body data has been received, false otherwise
    bool is_complete() const override {
        return this->complete_;
    }

    /// @brief Closes the HTTP connection and frees esp_http_client resources
    /// @note Safe to call multiple times; a no-op if already closed
    void close() override {
        this->cleanup();
    }

private:
    /// @brief Connects the socket and collects the response headers
    /// The socket handshake gets whatever is left of the deadline, then the read timeout takes
    /// over so every later blocking call -- the header retries below and every body read --
    /// returns within one short slice.
    /// @note esp_http_client_open() cannot be interrupted once it blocks, so cancellation is
    /// checked before it rather than during. Bounding it by the remaining budget instead of a
    /// fresh timeout is what keeps that uninterruptible stretch short.
    /// @param request Connection settings, timeouts, and cancellation hook
    /// @param deadline_ticks Tick count the whole exchange must finish by
    /// @return Header length on success, or a negative value on failure
    int64_t connect_and_fetch_headers(const HttpRequest& request, TickType_t deadline_ticks) {
        if (request.cancel_check != nullptr && request.cancel_check(request.cancel_context)) {
            MD_LOGD(TAG, "Cancelled before connecting");
            return -1;
        }

        uint32_t remaining_ms = remaining_budget_ms(deadline_ticks);
        if (remaining_ms == 0) {
            MD_LOGE(TAG, "Ran out of time before connecting");
            return -1;
        }

        esp_http_client_set_timeout_ms(this->client_, static_cast<int>(remaining_ms));
        esp_err_t err = esp_http_client_open(this->client_, 0);
        if (err != ESP_OK) {
            MD_LOGE(TAG, "Failed to open URL: %s", esp_err_to_name(err));
            return -1;
        }

        esp_http_client_set_timeout_ms(this->client_, static_cast<int>(request.read_timeout_ms));
        return this->fetch_headers(request, deadline_ticks);
    }

    /// @brief Fetches response headers, retrying while the socket read times out
    /// ESP_ERR_HTTP_EAGAIN means the read timed out with the headers still incomplete. The
    /// connection and the parser state both survive it, so retry on the same client.
    /// Reconnecting instead would spend a socket per attempt, and every socket closed that
    /// way holds one of the few available slots in TIME_WAIT afterwards. Each attempt blocks
    /// for at most read_timeout_ms, which is how often the cancel check gets polled.
    /// @param request Connection settings, timeouts, and cancellation hook
    /// @param deadline_ticks Tick count the whole exchange must finish by
    /// @return Header length on success, or a negative value on failure, cancellation, or
    /// timeout
    int64_t fetch_headers(const HttpRequest& request, TickType_t deadline_ticks) {
        while (true) {
            int64_t header_len = esp_http_client_fetch_headers(this->client_);
            if (header_len != -ESP_ERR_HTTP_EAGAIN) {
                if (header_len < 0) {
                    MD_LOGE(TAG, "Failed to fetch headers");
                }
                return header_len;
            }

            if (request.cancel_check != nullptr && request.cancel_check(request.cancel_context)) {
                MD_LOGD(TAG, "Cancelled while fetching headers");
                return -1;
            }

            if (remaining_budget_ms(deadline_ticks) == 0) {
                MD_LOGE(TAG, "Timed out fetching headers");
                return -1;
            }
        }
    }

    /// @brief Handles HTTP client events from esp_http_client
    static esp_err_t http_event_handler(esp_http_client_event_t* evt) {
        auto* self = static_cast<EspHttpClient*>(evt->user_data);
        if (evt->event_id == HTTP_EVENT_ON_HEADER) {
            if (strcasecmp(evt->header_key, "Content-Type") == 0) {
                self->response_.content_type = evt->header_value;
            }
        }
        return ESP_OK;
    }

    /// @brief Closes and cleans up the HTTP client handle
    void cleanup() {
        if (this->client_ != nullptr) {
            esp_http_client_close(this->client_);
            esp_http_client_cleanup(this->client_);
            this->client_ = nullptr;
        }
    }

    // Struct fields
    HttpResponse response_;

    // Pointer fields
    esp_http_client_handle_t client_{nullptr};

    // bool fields
    bool complete_{false};
};

// ============================================================================
// Factory
// ============================================================================

std::unique_ptr<HttpClient> create_http_client() {
    return std::make_unique<EspHttpClient>();
}

}  // namespace micro_decoder
