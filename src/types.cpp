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

#include "micro_decoder/types.h"

#include "platform/logging.h"

#include <cctype>
#include <cstring>

namespace micro_decoder {

// ============================================================================
// Log level control (host only; no-op on ESP)
// ============================================================================

#ifndef ESP_PLATFORM
static_assert(LOG_LEVEL_ERROR == MD_LOG_ERROR);
static_assert(LOG_LEVEL_WARN == MD_LOG_WARN);
static_assert(LOG_LEVEL_INFO == MD_LOG_INFO);
static_assert(LOG_LEVEL_DEBUG == MD_LOG_DEBUG);
#endif

void set_log_level(int level) {
    platform_set_log_level(level);
}

// ============================================================================
// AudioFileType
// ============================================================================

static AudioFileType type_from_content_type(const char* content_type) {
    if (content_type == nullptr || content_type[0] == '\0') {
        return AudioFileType::NONE;
    }
    // Case-insensitive substring match for MIME type detection
    [[maybe_unused]] auto contains = [&](const char* needle) -> bool {
        return strcasestr(content_type, needle) != nullptr;
    };
    // Case-insensitive whole-token match (bounded by non-alphanumeric characters).
    // Used for bare codec tokens like "opus" so codecs=notopus / opusenc don't match.
    [[maybe_unused]] auto contains_token = [&](const char* needle) -> bool {
        size_t needle_len = strlen(needle);
        const char* pos = content_type;
        while ((pos = strcasestr(pos, needle)) != nullptr) {
            bool left_ok =
                (pos == content_type) || (isalnum(static_cast<unsigned char>(pos[-1])) == 0);
            bool right_ok = isalnum(static_cast<unsigned char>(pos[needle_len])) == 0;
            if (left_ok && right_ok) {
                return true;
            }
            pos += 1;
        }
        return false;
    };

#ifdef MICRO_DECODER_CODEC_FLAC
    if (contains("audio/flac") || contains("audio/x-flac")) {
        return AudioFileType::FLAC;
    }
#endif
#ifdef MICRO_DECODER_CODEC_MP3
    if (contains("audio/mpeg") || contains("audio/mp3") || contains("audio/x-mpeg")) {
        return AudioFileType::MP3;
    }
#endif
    // Ogg can carry Opus or Vorbis. The classification is the same in every build:
    // an explicit opus signal ("audio/opus" or an Ogg type naming opus, e.g.
    // audio/ogg;codecs=opus) is Opus; any other Ogg type is Vorbis. A type whose codec
    // is compiled out returns NONE rather than being reclassified as the other codec.
    if (contains("audio/opus") ||
        ((contains("audio/ogg") || contains("application/ogg")) && contains_token("opus"))) {
#ifdef MICRO_DECODER_CODEC_OPUS
        return AudioFileType::OPUS;
#else
        return AudioFileType::NONE;
#endif
    }
    if (contains("audio/vorbis") || contains("audio/ogg") || contains("application/ogg")) {
#ifdef MICRO_DECODER_CODEC_VORBIS
        return AudioFileType::VORBIS;
#else
        return AudioFileType::NONE;
#endif
    }
#ifdef MICRO_DECODER_CODEC_WAV
    if (contains("audio/wav") || contains("audio/x-wav") || contains("audio/wave")) {
        return AudioFileType::WAV;
    }
#endif
    return AudioFileType::NONE;
}

static AudioFileType type_from_url_extension(const char* url) {
    if (url == nullptr || url[0] == '\0') {
        return AudioFileType::NONE;
    }
    // Find the last '.' in the URL path (before any '?' query string)
    const char* query = strchr(url, '?');
    size_t url_len = query != nullptr ? static_cast<size_t>(query - url) : strlen(url);

    const char* last_dot = nullptr;
    for (size_t i = 0; i < url_len; ++i) {
        if (url[i] == '.') {
            last_dot = url + i;
        }
    }

    if (last_dot == nullptr) {
        return AudioFileType::NONE;
    }

    [[maybe_unused]] const char* ext = last_dot + 1;
    [[maybe_unused]] size_t ext_len = url_len - static_cast<size_t>(ext - url);
#ifdef MICRO_DECODER_CODEC_FLAC
    if (ext_len == 4 && strncasecmp(ext, "flac", 4) == 0) {
        return AudioFileType::FLAC;
    }
#endif
#ifdef MICRO_DECODER_CODEC_MP3
    if (ext_len == 3 && strncasecmp(ext, "mp3", 3) == 0) {
        return AudioFileType::MP3;
    }
#endif
#ifdef MICRO_DECODER_CODEC_OPUS
    if (ext_len == 4 && strncasecmp(ext, "opus", 4) == 0) {
        return AudioFileType::OPUS;
    }
#endif
#ifdef MICRO_DECODER_CODEC_VORBIS
    if (ext_len == 3 && strncasecmp(ext, "ogg", 3) == 0) {
        return AudioFileType::VORBIS;
    }
#endif
#ifdef MICRO_DECODER_CODEC_WAV
    if (ext_len == 3 && strncasecmp(ext, "wav", 3) == 0) {
        return AudioFileType::WAV;
    }
#endif

    return AudioFileType::NONE;
}

const char* audio_file_type_to_string(AudioFileType file_type) {
    switch (file_type) {
#ifdef MICRO_DECODER_CODEC_FLAC
        case AudioFileType::FLAC:
            return "FLAC";
#endif
#ifdef MICRO_DECODER_CODEC_MP3
        case AudioFileType::MP3:
            return "MP3";
#endif
#ifdef MICRO_DECODER_CODEC_OPUS
        case AudioFileType::OPUS:
            return "OPUS";
#endif
#ifdef MICRO_DECODER_CODEC_VORBIS
        case AudioFileType::VORBIS:
            return "VORBIS";
#endif
#ifdef MICRO_DECODER_CODEC_WAV
        case AudioFileType::WAV:
            return "WAV";
#endif
        case AudioFileType::NONE:
        default:
            return "NONE";
    }
}

AudioFileType detect_audio_file_type(const char* content_type, const char* url) {
    AudioFileType type = type_from_content_type(content_type);
    if (type != AudioFileType::NONE) {
        return type;
    }
    return type_from_url_extension(url);
}

// ============================================================================
// AudioStreamInfo
// ============================================================================

AudioStreamInfo::AudioStreamInfo(uint8_t bits_per_sample, uint8_t channels, uint32_t sample_rate)
    : sample_rate_(sample_rate),
      bits_per_sample_(bits_per_sample),
      bytes_per_sample_(static_cast<uint8_t>((bits_per_sample + 7) / 8)),
      channels_(channels) {}

bool AudioStreamInfo::operator==(const AudioStreamInfo& rhs) const {
    return this->bits_per_sample_ == rhs.bits_per_sample_ && this->channels_ == rhs.channels_ &&
           this->sample_rate_ == rhs.sample_rate_;
}

}  // namespace micro_decoder
