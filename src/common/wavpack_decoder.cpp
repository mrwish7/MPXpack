#include "wavpack_decoder.h"

#include "byte_ring_buffer.h"

namespace mpx {

WavpackDecoder::~WavpackDecoder() {
    close();
}

int32_t WavpackDecoder::readBytes(void* id, void* data, int32_t bcount) {
    auto* self = static_cast<WavpackDecoder*>(id);
    auto* out = static_cast<uint8_t*>(data);
    int32_t written = 0;

    if (bcount > 0 && self->pushback_ >= 0) {
        out[0] = static_cast<uint8_t>(self->pushback_);
        self->pushback_ = -1;
        written = 1;
    }

    if (written < bcount) {
        size_t got = self->source_->popBlocking(out + written,
                                                 static_cast<size_t>(bcount - written),
                                                 self->read_timeout_ms_);
        written += static_cast<int32_t>(got);
    }

    self->pos_ += written;
    return written;
}

int32_t WavpackDecoder::writeBytes(void*, void*, int32_t) {
    return 0; // unused: we never decode tags/edit in place
}

int64_t WavpackDecoder::getPos(void* id) {
    return static_cast<WavpackDecoder*>(id)->pos_;
}

int WavpackDecoder::setPosAbs(void*, int64_t) {
    return -1; // not seekable; dead code path under OPEN_STREAMING
}

int WavpackDecoder::setPosRel(void*, int64_t, int) {
    return -1;
}

int WavpackDecoder::pushBackByte(void* id, int c) {
    static_cast<WavpackDecoder*>(id)->pushback_ = c;
    return c;
}

int64_t WavpackDecoder::getLength(void*) {
    return -1; // unknown/unbounded
}

int WavpackDecoder::canSeek(void*) {
    return 0;
}

int WavpackDecoder::truncateHere(void*) {
    return 0;
}

int WavpackDecoder::closeStream(void*) {
    return 0; // ByteRingBuffer lifetime is owned externally, not by the context
}

bool WavpackDecoder::open(ByteRingBuffer* source, int read_timeout_ms) {
    close();
    source_ = source;
    read_timeout_ms_ = read_timeout_ms;
    pos_ = 0;
    pushback_ = -1;

    reader_.read_bytes = &WavpackDecoder::readBytes;
    reader_.write_bytes = &WavpackDecoder::writeBytes;
    reader_.get_pos = &WavpackDecoder::getPos;
    reader_.set_pos_abs = &WavpackDecoder::setPosAbs;
    reader_.set_pos_rel = &WavpackDecoder::setPosRel;
    reader_.push_back_byte = &WavpackDecoder::pushBackByte;
    reader_.get_length = &WavpackDecoder::getLength;
    reader_.can_seek = &WavpackDecoder::canSeek;
    reader_.truncate_here = &WavpackDecoder::truncateHere;
    reader_.close = &WavpackDecoder::closeStream;

    char error[80] = {0};
    // Deliberately NOT OPEN_NO_CHECKSUM: our transport can deliver corrupted
    // bytes that a file source never would, so we want WavPack's per-block
    // checksum defense active.
    ctx_ = WavpackOpenFileInputEx64(&reader_, this, nullptr, error, OPEN_STREAMING, 0);
    if (!ctx_) {
        last_error_ = error;
        source_ = nullptr;
        return false;
    }
    return true;
}

uint32_t WavpackDecoder::decode(int32_t* out, uint32_t max_samples) {
    if (!ctx_) return 0;
    return WavpackUnpackSamples(ctx_, out, max_samples);
}

void WavpackDecoder::close() {
    if (ctx_) {
        WavpackCloseFile(ctx_);
        ctx_ = nullptr;
    }
    source_ = nullptr;
}

int WavpackDecoder::lastErrorCount() const {
    return ctx_ ? WavpackGetNumErrors(ctx_) : 0;
}

int WavpackDecoder::sampleRate() const {
    return ctx_ ? static_cast<int>(WavpackGetSampleRate(ctx_)) : 0;
}

int WavpackDecoder::numChannels() const {
    return ctx_ ? WavpackGetNumChannels(ctx_) : 0;
}

} // namespace mpx
