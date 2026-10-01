#include "LogRecordQueue.h"
#include "concurrency/LockGuard.h"
#include <algorithm>
#include <cstdlib>
#include <cstring>

// Each record is a big-endian 16-bit payload length followed by the payload.
static constexpr size_t RECORD_HEADER_LEN = 2;

bool LogRecordQueue::begin(size_t size)
{
    end();
    if (size <= RECORD_HEADER_LEN)
        return false;
    // malloc, not new: a failed operator new aborts on ESP32 instead of returning nullptr.
    uint8_t *storage = static_cast<uint8_t *>(malloc(size));
    if (!storage)
        return false;

    concurrency::LockGuard guard(&lock);
    ring = storage;
    capacity = size;
    readPos = 0;
    used = 0;
    dropped = 0;
    return true;
}

void LogRecordQueue::end()
{
    uint8_t *storage;
    {
        concurrency::LockGuard guard(&lock);
        storage = ring;
        ring = nullptr;
        capacity = 0;
        readPos = 0;
        used = 0;
    }
    free(storage);
}

bool LogRecordQueue::push(const uint8_t *payload, size_t len)
{
    concurrency::LockGuard guard(&lock);
    if (!ring)
        return false;
    if (len > UINT16_MAX || RECORD_HEADER_LEN + len > capacity - used) {
        dropped++;
        return false;
    }

    const uint8_t header[RECORD_HEADER_LEN] = {(uint8_t)(len >> 8), (uint8_t)len};
    const size_t writePos = (readPos + used) % capacity;
    copyIn(writePos, header, RECORD_HEADER_LEN);
    copyIn((writePos + RECORD_HEADER_LEN) % capacity, payload, len);
    used += RECORD_HEADER_LEN + len;
    return true;
}

size_t LogRecordQueue::pop(uint8_t *out, size_t outSize)
{
    concurrency::LockGuard guard(&lock);
    if (!ring || used < RECORD_HEADER_LEN)
        return 0;

    uint8_t header[RECORD_HEADER_LEN];
    copyOut(readPos, header, RECORD_HEADER_LEN);
    const size_t len = ((size_t)header[0] << 8) | header[1];
    const size_t payloadPos = (readPos + RECORD_HEADER_LEN) % capacity;
    readPos = (payloadPos + len) % capacity;
    used -= RECORD_HEADER_LEN + len;

    if (len > outSize) {
        dropped++;
        return 0;
    }
    copyOut(payloadPos, out, len);
    return len;
}

bool LogRecordQueue::isEmpty()
{
    concurrency::LockGuard guard(&lock);
    return used == 0;
}

uint32_t LogRecordQueue::takeDropped()
{
    concurrency::LockGuard guard(&lock);
    const uint32_t count = dropped;
    dropped = 0;
    return count;
}

void LogRecordQueue::copyIn(size_t pos, const uint8_t *src, size_t len)
{
    const size_t first = std::min(len, capacity - pos);
    memcpy(ring + pos, src, first);
    memcpy(ring, src + first, len - first);
}

void LogRecordQueue::copyOut(size_t pos, uint8_t *dst, size_t len) const
{
    const size_t first = std::min(len, capacity - pos);
    memcpy(dst, ring + pos, first);
    memcpy(dst + first, ring, len - first);
}
