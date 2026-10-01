#pragma once

#include "concurrency/Lock.h"
#include <cstddef>
#include <cstdint>

/// Bounded FIFO of encoded FromRadio payloads: log calls on any task push, the owning API session pops.
class LogRecordQueue
{
  public:
    LogRecordQueue() = default;
    ~LogRecordQueue() { end(); }

    LogRecordQueue(const LogRecordQueue &) = delete;
    LogRecordQueue &operator=(const LogRecordQueue &) = delete;

    /// Allocate a ring of the given size; false when the heap cannot supply it.
    bool begin(size_t size);
    /// Release the ring; later pushes are ignored until the next begin().
    void end();
    bool isActive() const { return ring != nullptr; }

    /// Append one payload, counting it as dropped when it does not fit.
    bool push(const uint8_t *payload, size_t len);
    /// Move the oldest payload into out; returns its length, 0 when there is none.
    size_t pop(uint8_t *out, size_t outSize);
    bool isEmpty();
    /// Return and reset the number of payloads dropped since the last call.
    uint32_t takeDropped();

  private:
    void copyIn(size_t pos, const uint8_t *src, size_t len);
    void copyOut(size_t pos, uint8_t *dst, size_t len) const;

    concurrency::Lock lock;
    uint8_t *ring = nullptr;
    size_t capacity = 0;
    size_t readPos = 0;
    size_t used = 0;
    uint32_t dropped = 0;
};
