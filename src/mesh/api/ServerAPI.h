#pragma once

#include "RedirectablePrint.h"
#include "StreamAPI.h"
#include "mesh/LogRecordQueue.h"
#include "mesh/StreamFrameWriter.h"
#include <cstdlib>
#include <memory>

#define SERVER_API_DEFAULT_PORT 4403

// Bytes of LogRecords queued for a TCP client while debug_log_api_enabled is set; 0 turns the stream off.
// Off on portduino: its Lock is a no-op while more than one thread logs.
#ifndef API_LOG_QUEUE_SIZE
#ifdef ARCH_PORTDUINO
#define API_LOG_QUEUE_SIZE 0
#else
#define API_LOG_QUEUE_SIZE 4096
#endif
#endif

/**
 * Serves the protobuf API to one TCP client; with debug_log_api_enabled the debug log goes along as FromRadio.log_record.
 */
template <class T> class ServerAPI : public StreamAPI, public LogRecordSink, private concurrency::OSThread
{
  private:
    T client;
    StreamFrameWriter frameWriter;
    LogRecordQueue logQueue;
    bool logQueueRefused = false;

    /// Allocate or release the log queue to follow debug_log_api_enabled.
    void updateLogQueue();

  public:
    explicit ServerAPI(T &_client);

    virtual ~ServerAPI();

    /// override close to also shutdown the TCP link
    virtual void close();

    /// Check the current underlying physical link to see if the client is currently connected
    virtual bool checkIsConnected() override;

    /// Queue one log line for this client; runs on whichever task logged it.
    void onLogRecord(meshtastic_LogRecord_Level level, const char *source, const char *format, va_list arg) override;

  protected:
    /// We override this method to prevent publishing EVENT_SERIAL_CONNECTED/DISCONNECTED for wifi links (we want the board to
    /// stay in the POWERED state to prevent disabling wifi)
    virtual void onConnectionChanged(bool connected) override {}
    /// Write or retain one framed TCP message.
    virtual bool writeFrame(uint8_t *buf, size_t len, bool bestEffort) override;
    /// Continue retained TCP output before PhoneAPI advances.
    virtual bool finishPendingFrame() override;
    /// Report a retained TCP frame awaiting transmit space.
    virtual bool hasRetainedFrame() override;
    /// Return whether the dedicated log buffer can be safely overwritten.
    virtual bool canEncodeLogRecord() override;
    /// Report whether a frame can go out now without the client's write() blocking.
    virtual bool canWriteFrame(size_t frameLen) override;

    virtual int32_t runOnce() override; // Check for dropped client connections
};

/**
 * Listens for incoming connections and does accepts and creates instances of ServerAPI as needed
 */
template <class T, class U> class APIServerPort : public U, private concurrency::OSThread
{
    /** The currently open port
     *
     * FIXME: We currently only allow one open TCP connection at a time, because we depend on the loop() call in this class to
     * delegate to the worker.  Once coroutines are implemented we can relax this restriction.
     *
     * The ServerAPI is built in a malloc()'d block with placement new rather than operator new: on ESP32 the framework
     * is compiled with CONFIG_COMPILER_CXX_EXCEPTIONS=n and every throw is wrapped to abort(), which makes a failed
     * operator new - the plain form and, because libstdc++ implements it as a try/catch around the plain form, the
     * std::nothrow form too - a reboot. malloc() is the one allocation on that platform that hands back nullptr, so
     * a fragmented heap drops the incoming client instead of the node. The deleter runs the destructor and free()s.
     */
    struct MallocDeleter {
        void operator()(T *p) const
        {
            if (p) {
                p->~T();
                free(p);
            }
        }
    };
    std::unique_ptr<T, MallocDeleter> openAPI;
#if defined(RAK_4631) || defined(RAK11310)
    // Track wait time for RAK13800 Ethernet requests
    int32_t waitTime = 100;
#endif

  public:
    explicit APIServerPort(int port);

    void init();

  protected:
    int32_t runOnce() override;
};
