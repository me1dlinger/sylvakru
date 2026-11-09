#include "flac_decoder.h"

#include <FLAC/stream_decoder.h>

#include <algorithm>
#include <cstring>
#include <vector>

namespace sylvakru {

struct FlacDecoder::State {
    FLAC__StreamDecoder* decoder = nullptr;
    FlacStreamInfo info;
    std::vector<int32_t> pending_interleaved;
    size_t pending_offset = 0;
    uint64_t current_frame = 0;
    bool metadata_ready = false;
    bool end_of_stream = false;
    FlacResult callback_error;

    static FLAC__StreamDecoderWriteStatus writeCallback(
        const FLAC__StreamDecoder*,
        const FLAC__Frame* frame,
        const FLAC__int32* const buffer[],
        void* client_data) {
        auto* state = static_cast<State*>(client_data);
        if (!state->metadata_ready || frame == nullptr || buffer == nullptr) {
            state->callback_error = {
                FlacError::kInvalidStream,
                "FLAC audio frame arrived before STREAMINFO.",
            };
            return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
        }
        const uint32_t blocksize = frame->header.blocksize;
        state->pending_interleaved.reserve(
            state->pending_interleaved.size() +
            static_cast<size_t>(blocksize) * state->info.channels);
        for (uint32_t frame_index = 0; frame_index < blocksize; ++frame_index) {
            for (uint32_t channel = 0; channel < state->info.channels; ++channel) {
                state->pending_interleaved.push_back(buffer[channel][frame_index]);
            }
        }
        return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
    }

    static void metadataCallback(
        const FLAC__StreamDecoder*,
        const FLAC__StreamMetadata* metadata,
        void* client_data) {
        auto* state = static_cast<State*>(client_data);
        if (metadata == nullptr || metadata->type != FLAC__METADATA_TYPE_STREAMINFO) {
            return;
        }
        const auto& stream_info = metadata->data.stream_info;
        state->info = {
            stream_info.sample_rate,
            stream_info.channels,
            stream_info.bits_per_sample,
            stream_info.total_samples,
        };
        if (state->info.sample_rate == 0) {
            state->callback_error = {
                FlacError::kUnsupportedFormat,
                "Unsupported FLAC sample rate.",
            };
            return;
        }
        if (state->info.channels < 1 || state->info.channels > 2) {
            state->callback_error = {
                FlacError::kUnsupportedFormat,
                "Unsupported FLAC channel count.",
            };
            return;
        }
        if (state->info.valid_bits_per_sample < 8 ||
            state->info.valid_bits_per_sample > 32) {
            state->callback_error = {
                FlacError::kUnsupportedFormat,
                "Unsupported FLAC bit depth.",
            };
            return;
        }
        state->metadata_ready = true;
    }

    static void errorCallback(
        const FLAC__StreamDecoder*,
        FLAC__StreamDecoderErrorStatus status,
        void* client_data) {
        auto* state = static_cast<State*>(client_data);
        state->callback_error = {
            FlacError::kDecodeFailed,
            std::string("FLAC decode failed: ") +
                FLAC__StreamDecoderErrorStatusString[status] + ".",
        };
    }
};

FlacDecoder::FlacDecoder() : state_(new State()) {}

FlacDecoder::~FlacDecoder() {
    close();
    delete state_;
}

FlacResult FlacDecoder::open(const std::string& path) {
    close();
    state_->decoder = FLAC__stream_decoder_new();
    if (state_->decoder == nullptr) {
        return {FlacError::kOpenFailed, "Failed to allocate FLAC decoder."};
    }
    const auto init_status = FLAC__stream_decoder_init_file(
        state_->decoder,
        path.c_str(),
        State::writeCallback,
        State::metadataCallback,
        State::errorCallback,
        state_);
    if (init_status != FLAC__STREAM_DECODER_INIT_STATUS_OK) {
        const std::string message = std::string("Failed to open FLAC file: ") +
            FLAC__StreamDecoderInitStatusString[init_status] + ".";
        close();
        return {FlacError::kOpenFailed, message};
    }
    if (!FLAC__stream_decoder_process_until_end_of_metadata(state_->decoder)) {
        const FlacResult error = state_->callback_error.ok()
            ? FlacResult{FlacError::kInvalidStream, "Failed to read FLAC metadata."}
            : state_->callback_error;
        close();
        return error;
    }
    if (!state_->callback_error.ok()) {
        const FlacResult error = state_->callback_error;
        close();
        return error;
    }
    if (!state_->metadata_ready) {
        close();
        return {FlacError::kInvalidStream, "FLAC STREAMINFO is missing."};
    }
    return {};
}

const FlacStreamInfo& FlacDecoder::streamInfo() const {
    return state_->info;
}

FlacReadResult FlacDecoder::readFrames(int32_t* output, uint32_t capacity_frames) {
    if (state_->decoder == nullptr || !state_->metadata_ready) {
        return {FlacError::kInvalidStream, 0, false, "FLAC decoder is not open."};
    }
    if (output == nullptr || capacity_frames == 0) {
        return {FlacError::kInvalidBuffer, 0, false, "FLAC target buffer is invalid."};
    }

    uint32_t frames_written = 0;
    while (frames_written < capacity_frames) {
        const size_t pending_samples =
            state_->pending_interleaved.size() - state_->pending_offset;
        const uint32_t pending_frames = static_cast<uint32_t>(
            pending_samples / state_->info.channels);
        if (pending_frames > 0) {
            const uint32_t frames_to_copy = std::min(
                pending_frames,
                capacity_frames - frames_written);
            const size_t samples_to_copy =
                static_cast<size_t>(frames_to_copy) * state_->info.channels;
            std::memcpy(
                output + static_cast<size_t>(frames_written) * state_->info.channels,
                state_->pending_interleaved.data() + state_->pending_offset,
                samples_to_copy * sizeof(int32_t));
            state_->pending_offset += samples_to_copy;
            frames_written += frames_to_copy;
            state_->current_frame += frames_to_copy;
            if (state_->pending_offset == state_->pending_interleaved.size()) {
                state_->pending_interleaved.clear();
                state_->pending_offset = 0;
            }
            continue;
        }
        if (state_->end_of_stream) {
            break;
        }
        state_->callback_error = {};
        if (!FLAC__stream_decoder_process_single(state_->decoder)) {
            const FlacResult error = state_->callback_error.ok()
                ? FlacResult{FlacError::kDecodeFailed, "Failed to decode FLAC frame."}
                : state_->callback_error;
            return {error.error, frames_written, false, error.message};
        }
        if (!state_->callback_error.ok()) {
            return {
                state_->callback_error.error,
                frames_written,
                false,
                state_->callback_error.message,
            };
        }
        state_->end_of_stream =
            FLAC__stream_decoder_get_state(state_->decoder) ==
            FLAC__STREAM_DECODER_END_OF_STREAM;
    }

    const bool known_end = state_->info.total_frames > 0 &&
        state_->current_frame >= state_->info.total_frames;
    return {FlacError::kNone, frames_written, state_->end_of_stream || known_end, {}};
}

FlacResult FlacDecoder::seekToFrame(uint64_t frame) {
    if (state_->decoder == nullptr || !state_->metadata_ready) {
        return {FlacError::kInvalidStream, "FLAC decoder is not open."};
    }
    state_->pending_interleaved.clear();
    state_->pending_offset = 0;
    state_->end_of_stream = false;
    state_->callback_error = {};
    if (!FLAC__stream_decoder_seek_absolute(state_->decoder, frame)) {
        return {FlacError::kSeekFailed, "Failed to seek FLAC stream."};
    }
    state_->current_frame = frame;
    return {};
}

void FlacDecoder::close() {
    if (state_->decoder != nullptr) {
        FLAC__stream_decoder_finish(state_->decoder);
        FLAC__stream_decoder_delete(state_->decoder);
    }
    state_->decoder = nullptr;
    state_->info = {};
    state_->pending_interleaved.clear();
    state_->pending_offset = 0;
    state_->current_frame = 0;
    state_->metadata_ready = false;
    state_->end_of_stream = false;
    state_->callback_error = {};
}

}  // namespace sylvakru
