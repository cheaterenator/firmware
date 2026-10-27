#include "NodeDB.h"
#include "TestUtil.h"
#include "modules/OnDemandModule.h"
#include "modules/Telemetry/DeviceTelemetry.h"
#include <cstring>
#include <unity.h>

// DeviceTelemetryModule::localStatsBroadcastDue() is the cadence of the LocalStats / LocalStatsExtended
// broadcasts (LocalStatsBroadcastConfig in ondemand.proto). It takes plain values, so no device globals
// or fake clock are needed. OnDemandModule::mayChangeLocalStatsBroadcastConfig() is who may set them;
// it reads only the admin keys in config.security.

static constexpr uint32_t HOUR_MS = 3600000U;

// With no earlier send, the first one waits only its random extra delay from our first run - not a
// whole interval, and not zero, so it does not go out together with the device metrics.
static void test_firstSend_waitsOnlyExtraDelay()
{
    TEST_ASSERT_FALSE(DeviceTelemetryModule::localStatsBroadcastDue(1000U + 59999U, 0U, 1000U, HOUR_MS, 60000U));
    TEST_ASSERT_TRUE(DeviceTelemetryModule::localStatsBroadcastDue(1000U + 60000U, 0U, 1000U, HOUR_MS, 60000U));
}

static void test_firstSend_zeroExtraDelayIsImmediate()
{
    TEST_ASSERT_TRUE(DeviceTelemetryModule::localStatsBroadcastDue(1000U, 0U, 1000U, HOUR_MS, 0U));
}

// Later sends wait the interval plus the extra delay drawn for this emission.
static void test_laterSend_waitsIntervalPlusExtraDelay()
{
    constexpr uint32_t last = 5000U;
    TEST_ASSERT_FALSE(DeviceTelemetryModule::localStatsBroadcastDue(last + HOUR_MS, last, 1U, HOUR_MS, 360000U));
    TEST_ASSERT_FALSE(DeviceTelemetryModule::localStatsBroadcastDue(last + HOUR_MS + 359999U, last, 1U, HOUR_MS, 360000U));
    TEST_ASSERT_TRUE(DeviceTelemetryModule::localStatsBroadcastDue(last + HOUR_MS + 360000U, last, 1U, HOUR_MS, 360000U));
}

// A last send restored from TransmitHistory after a reboot can lie "before" millis() 0, i.e. wrapped.
static void test_laterSend_survivesMillisWrap()
{
    constexpr uint32_t last = UINT32_MAX - 999U; // 1,000 ms before the wrap to 0
    TEST_ASSERT_FALSE(DeviceTelemetryModule::localStatsBroadcastDue(HOUR_MS - 1001U, last, 1U, HOUR_MS, 0U));
    TEST_ASSERT_TRUE(DeviceTelemetryModule::localStatsBroadcastDue(HOUR_MS - 1000U, last, 1U, HOUR_MS, 0U));
}

// The longest interval OnDemandModule allows (30 days) plus the largest extra delay (a tenth more)
// still fits the 32-bit sum.
static void test_longestInterval_doesNotOverflow()
{
    constexpr uint32_t intervalSecs = 30U * 24 * 60 * 60;
    constexpr uint32_t intervalMs = intervalSecs * 1000U;
    constexpr uint32_t extraMs = intervalSecs * 100U;
    TEST_ASSERT_FALSE(DeviceTelemetryModule::localStatsBroadcastDue(intervalMs, 1U, 1U, intervalMs, extraMs));
    TEST_ASSERT_TRUE(DeviceTelemetryModule::localStatsBroadcastDue(1U + intervalMs + extraMs, 1U, 1U, intervalMs, extraMs));
}

static const uint8_t ADMIN_KEY[32] = {0x5a, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a,
                                      0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15,
                                      0x16, 0x17, 0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f};
static const uint8_t OTHER_KEY[32] = {0xa5, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a,
                                      0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15,
                                      0x16, 0x17, 0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f};

static void setAdminKeys(const uint8_t *key)
{
    for (auto &slot : config.security.admin_key) {
        memset(&slot, 0, sizeof(slot));
    }
    config.security.admin_key_count = 0;
    if (key) {
        memcpy(config.security.admin_key[0].bytes, key, 32);
        config.security.admin_key[0].size = 32;
        config.security.admin_key_count = 1;
    }
}

// As Router::perhapsDecode() leaves a received packet: public_key is set only with pki_encrypted.
static meshtastic_MeshPacket requestFrom(NodeNum from, bool pkiEncrypted, const uint8_t *senderKey)
{
    meshtastic_MeshPacket mp = meshtastic_MeshPacket_init_zero;
    mp.from = from;
    mp.pki_encrypted = pkiEncrypted;
    if (senderKey) {
        memcpy(mp.public_key.bytes, senderKey, 32);
        mp.public_key.size = 32;
    }
    return mp;
}

// The phone attached to the node may always change it, admin keys or not.
static void test_configAuth_localPhoneAllowed()
{
    setAdminKeys(nullptr);
    TEST_ASSERT_TRUE(OnDemandModule::mayChangeLocalStatsBroadcastConfig(requestFrom(0, false, nullptr)));
}

static void test_configAuth_remoteAdminKeyAllowed()
{
    setAdminKeys(ADMIN_KEY);
    TEST_ASSERT_TRUE(OnDemandModule::mayChangeLocalStatsBroadcastConfig(requestFrom(0x12345678, true, ADMIN_KEY)));
}

static void test_configAuth_remoteOtherKeyRefused()
{
    setAdminKeys(ADMIN_KEY);
    TEST_ASSERT_FALSE(OnDemandModule::mayChangeLocalStatsBroadcastConfig(requestFrom(0x12345678, true, OTHER_KEY)));
}

// A channel-encrypted request proves no key, whatever public_key holds.
static void test_configAuth_remoteWithoutPkiRefused()
{
    setAdminKeys(ADMIN_KEY);
    TEST_ASSERT_FALSE(OnDemandModule::mayChangeLocalStatsBroadcastConfig(requestFrom(0x12345678, false, ADMIN_KEY)));
    TEST_ASSERT_FALSE(OnDemandModule::mayChangeLocalStatsBroadcastConfig(requestFrom(0x12345678, false, nullptr)));
}

// With no admin key configured nothing on the mesh may change it.
static void test_configAuth_noAdminKeysRefusesRemote()
{
    setAdminKeys(nullptr);
    TEST_ASSERT_FALSE(OnDemandModule::mayChangeLocalStatsBroadcastConfig(requestFrom(0x12345678, true, ADMIN_KEY)));
}

void setUp(void) {}

void tearDown(void)
{
    setAdminKeys(nullptr);
}

extern "C" {
void setup()
{
    initializeTestEnvironment();
    UNITY_BEGIN();
    RUN_TEST(test_firstSend_waitsOnlyExtraDelay);
    RUN_TEST(test_firstSend_zeroExtraDelayIsImmediate);
    RUN_TEST(test_laterSend_waitsIntervalPlusExtraDelay);
    RUN_TEST(test_laterSend_survivesMillisWrap);
    RUN_TEST(test_longestInterval_doesNotOverflow);
    RUN_TEST(test_configAuth_localPhoneAllowed);
    RUN_TEST(test_configAuth_remoteAdminKeyAllowed);
    RUN_TEST(test_configAuth_remoteOtherKeyRefused);
    RUN_TEST(test_configAuth_remoteWithoutPkiRefused);
    RUN_TEST(test_configAuth_noAdminKeysRefusesRemote);
    exit(UNITY_END());
}

void loop() {}
}
