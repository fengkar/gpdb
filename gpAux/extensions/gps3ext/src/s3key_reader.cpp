#define __STDC_FORMAT_MACROS
#include <inttypes.h>
#include <string.h>

#include "s3common.h"
#include "s3interface.h"
#include "s3key_reader.h"

// Return (offset, length) of next chunk to download,
// or (fileSize, 0) if reach end of file.
Range OffsetMgr::getNextOffset() {
    Range ret;

    pthread_mutex_lock(&this->offsetLock);
    ret.offset = std::min(this->curPos, this->keySize);

    if (this->curPos + this->chunkSize > this->keySize) {
        ret.length = this->keySize - this->curPos;
        this->curPos = this->keySize;
    } else {
        ret.length = this->chunkSize;
        this->curPos += this->chunkSize;
    }
    pthread_mutex_unlock(&this->offsetLock);

    return ret;
}

ChunkBuffer::ChunkBuffer(const string& url, S3KeyReader& reader)
    : sourceUrl(url), offsetMgr(reader.getOffsetMgr()), sharedKeyReader(reader) {
    s3interface = NULL;
    Range range = offsetMgr.getNextOffset();
    curFileOffset = range.offset;
    chunkDataSize = range.length;
    status = ReadyToFill;
    eof = false;
    curChunkOffset = 0;
    pthread_mutex_init(&this->statusMutex, NULL);
    pthread_cond_init(&this->statusCondVar, NULL);
}

ChunkBuffer::~ChunkBuffer() {
    pthread_mutex_destroy(&this->statusMutex);
    pthread_cond_destroy(&this->statusCondVar);
}

ChunkBuffer& ChunkBuffer::operator=(const ChunkBuffer& other) {
    this->sourceUrl = other.sourceUrl;
    this->eof = other.eof;
    this->status = other.status;
    this->curFileOffset = other.curFileOffset;
    this->curChunkOffset = other.curChunkOffset;
    this->chunkDataSize = other.chunkDataSize;

    return *this;
}

// ret < len means EMPTY
// that's why it checks if leftLen is larger than *or equal to* len below[1], provides a chance ret
// is 0, which is smaller than len. Otherwise, other functions won't know when to read next buffer.
uint64_t ChunkBuffer::read(char* buf, uint64_t len) {
    // QueryCancelPending stops s3_import(), this check is not needed if
    // s3_import() every time calls ChunkBuffer->Read() only once, otherwise(as we did in
    // downstreamReader->read() for decompression feature before), first call sets buffer to
    // ReadyToFill, second call hangs.
    CHECK_OR_DIE_MSG(!QueryCancelPending, "%s", "ChunkBuffer reading is interrupted by user");

    pthread_mutex_lock(&this->statusMutex);
    while (this->status != ReadyToRead) {
        pthread_cond_wait(&this->statusCondVar, &this->statusMutex);
    }

    // Error is shared between all chunks.
    if (this->isError()) {
        pthread_mutex_unlock(&this->statusMutex);
        // Don't throw here. Other chunks will set the shared error message,
        // it will be handled by S3KeyReader.
        return 0;
    }

    uint64_t leftLen = this->chunkDataSize - this->curChunkOffset;
    uint64_t lenToRead = std::min(len, leftLen);

    if (lenToRead != 0) {
        memcpy(buf, this->chunkData.data() + this->curChunkOffset, lenToRead);
    }

    if (len <= leftLen) {                   // [1]
        this->curChunkOffset += lenToRead;  // not empty
    } else {                                // empty, reset everything
        this->curChunkOffset = 0;

        if (!this->isEOF()) {
            // Release chunkData memory to reduce consumption.
            this->chunkData = vector<uint8_t>();

            this->status = ReadyToFill;

            Range range = this->offsetMgr.getNextOffset();
            this->curFileOffset = range.offset;
            this->chunkDataSize = range.length;

            pthread_cond_signal(&this->statusCondVar);
        }
    }

    pthread_mutex_unlock(&this->statusMutex);

    return lenToRead;
}

// returning uint64_t(-1) means error
uint64_t ChunkBuffer::fill() {
    pthread_mutex_lock(&this->statusMutex);
    while (this->status != ReadyToFill) {
        pthread_cond_wait(&this->statusCondVar, &this->statusMutex);
    }

    uint64_t offset = this->curFileOffset;
    uint64_t leftLen = this->chunkDataSize;

    uint64_t readLen = 0;

    if (leftLen != 0) {
        try {
            readLen = this->s3interface->fetchData(
                offset, this->chunkData, leftLen, this->sourceUrl,
                this->sharedKeyReader.getRegion(), this->sharedKeyReader.getCredential());
        } catch (std::exception& e) {
            S3DEBUG("Failed to fetch expected data from S3");
            this->setSharedError(true, e.what());
        }

        if (readLen != leftLen) {
            S3DEBUG("Failed to fetch expected data from S3");
            this->setSharedError(true, "Failed to fetch expected data from S3");
        } else {
            S3DEBUG("Got %" PRIu64 " bytes from S3", readLen);
        }
    }

    if (offset + leftLen >= offsetMgr.getKeySize()) {
        readLen = 0;  // Nothing to read, EOF
        S3DEBUG("Reached the end of file");
        this->eof = true;
    }

    this->status = ReadyToRead;
    pthread_cond_signal(&this->statusCondVar);
    pthread_mutex_unlock(&this->statusMutex);

    return (this->isError()) ? -1 : readLen;
}

void* DownloadThreadFunc(void* data) {
    ChunkBuffer* buffer = static_cast<ChunkBuffer*>(data);

    uint64_t filledSize = 0;
    S3DEBUG("Downloading thread starts");
    do {
        if (QueryCancelPending) {
            S3INFO("Downloading thread is interrupted by user");

            // error is shared between all chunks, so all chunks will stop.
            buffer->setSharedError(true, "Downloading thread is interrupted by user");

            // have to unlock ChunkBuffer::read in some certain conditions, for instance, status is
            // not ReadyToRead, and read() is waiting for signal stat_cond.
            buffer->setStatus(ReadyToRead);
            pthread_cond_signal(buffer->getStatCond());

            return NULL;
        }

        filledSize = buffer->fill();

        if (filledSize != 0) {
            if (buffer->isError()) {
                S3DEBUG("Failed to fill downloading buffer");
                break;
            } else {
                S3DEBUG("Size of filled data is %" PRIu64, filledSize);
            }
        }
    } while (!buffer->isEOF());
    S3DEBUG("Downloading thread ended");
    return NULL;
}

void S3KeyReader::open(const ReaderParams& params) {
    CHECK_OR_DIE_MSG(this->s3interface != NULL, "%s", "s3interface must not be NULL");

    this->sharedError = false;
    this->sharedErrorMessage.clear();

    this->numOfChunks = params.getNumOfChunks();
    CHECK_OR_DIE_MSG(this->numOfChunks > 0, "%s", "numOfChunks must not be zero");

    this->region = params.getRegion();
    this->credential = params.getCred();

    this->offsetMgr.setKeySize(params.getKeySize());
    this->offsetMgr.setChunkSize(params.getChunkSize());

    CHECK_OR_DIE_MSG(params.getChunkSize() > 0, "%s", "chunk size must be greater than zero");

    this->chunkBuffers.reserve(this->numOfChunks);

    for (uint64_t i = 0; i < this->numOfChunks; i++) {
        this->chunkBuffers.emplace_back(params.getKeyUrl(), *this);
    }

    for (uint64_t i = 0; i < this->numOfChunks; i++) {
        this->chunkBuffers[i].setS3interface(this->s3interface);

        pthread_t thread;
        pthread_create(&thread, NULL, DownloadThreadFunc, &this->chunkBuffers[i]);
        this->threads.push_back(thread);
    }

    return;
}

uint64_t S3KeyReader::read(char* buf, uint64_t count) {
    uint64_t fileLen = this->offsetMgr.getKeySize();
    uint64_t readLen = 0;

    do {
        // confirm there is no more available data, done with this file
        if (this->transferredKeyLen >= fileLen) {
            return 0;
        }

        ChunkBuffer& buffer = chunkBuffers[this->curReadingChunk % this->numOfChunks];

        readLen = buffer.read(buf, count);

        CHECK_OR_DIE_MSG(!this->sharedError, "%s", this->sharedErrorMessage.c_str());

        this->transferredKeyLen += readLen;

        if (readLen < count) {
            this->curReadingChunk++;
            CHECK_OR_DIE_MSG(!buffer.isError(), "%s", "Error occurs while downloading, skip");
        }

        // retry to confirm whether thread reading is finished or chunk size is
        // divisible by get()'s buffer size
    } while (readLen == 0);

    return readLen;
}

// reset marks before reading next key
void S3KeyReader::reset() {
    this->sharedError = false;
    this->curReadingChunk = 0;
    this->transferredKeyLen = 0;

    this->offsetMgr.reset();

    this->chunkBuffers.clear();
    this->threads.clear();
}

void S3KeyReader::close() {
    for (uint64_t i = 0; i < this->threads.size(); i++) {
        pthread_cancel(this->threads[i]);
        pthread_join(this->threads[i], NULL);
    }

    this->reset();

    return;
}
