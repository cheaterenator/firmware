#include "TestUtil.h"
#include "modules/PositionModule.h"
#include <unity.h>

// These exercise PositionModule's pure broadcast-policy helpers (stationary detection and the
// interval floor). They take plain values, so no device globals or fake clock are needed.

// Coordinates sharing the top `precision` bits land in the same grid cell.
static void test_withinPrecisionCell_jitterStaysInCell()
{
    // At precision 16 the top 16 bits define the cell; the low 16 bits are GPS jitter.
    TEST_ASSERT_TRUE(PositionModule::positionWithinPrecisionCell(0x12340000, 0x22340000, 0x1234ABCD, 0x2234EF01, 16));
}

static void test_withinPrecisionCell_movingLatLeavesCell()
{
    TEST_ASSERT_FALSE(PositionModule::positionWithinPrecisionCell(0x12340000, 0x22340000, 0x12350000, 0x22340000, 16));
}

static void test_withinPrecisionCell_movingLonLeavesCell()
{
    TEST_ASSERT_FALSE(PositionModule::positionWithinPrecisionCell(0x12340000, 0x22340000, 0x12340000, 0x22350000, 16));
}

// precision 0 means position sharing is off - never treat as stationary/suppressible.
static void test_withinPrecisionCell_zeroPrecisionNeverSuppresses()
{
    TEST_ASSERT_FALSE(PositionModule::positionWithinPrecisionCell(0x12340000, 0x22340000, 0x12340000, 0x22340000, 0));
}

// Full precision (>=32): any difference matters, and identical full-precision coords still aren't
// "stationary" because there's no coarse cell to hold within.
static void test_withinPrecisionCell_fullPrecisionNeverSuppresses()
{
    TEST_ASSERT_FALSE(PositionModule::positionWithinPrecisionCell(0x12340000, 0x22340000, 0x12340000, 0x22340000, 32));
}

static void test_effectiveInterval_stationaryRaisesToFloor()
{
    TEST_ASSERT_EQUAL_UINT32(43200000U, PositionModule::effectiveBroadcastIntervalMs(60000U, true, 43200000U));
}

static void test_effectiveInterval_movingKeepsConfigured()
{
    TEST_ASSERT_EQUAL_UINT32(60000U, PositionModule::effectiveBroadcastIntervalMs(60000U, false, 43200000U));
}

// A configured interval already longer than the floor is never shortened.
static void test_effectiveInterval_longConfiguredWinsOverFloor()
{
    TEST_ASSERT_EQUAL_UINT32(50000000U, PositionModule::effectiveBroadcastIntervalMs(50000000U, true, 43200000U));
}

static void test_effectiveInterval_zeroFloorIsNoOp()
{
    TEST_ASSERT_EQUAL_UINT32(60000U, PositionModule::effectiveBroadcastIntervalMs(60000U, true, 0U));
}

// Local phone/UI play: positions are opt-in on the mesh, but our own position still streams to
// the connected phone at a fixed cadence (mirroring telemetry's local delivery).

// A never-sent state streams immediately once a valid position exists, regardless of the clock.
static void test_sendToPhone_firstSendIsImmediate()
{
    TEST_ASSERT_TRUE(PositionModule::shouldSendPositionToPhone(true, true, false, 5000U, 0U, 60000U));
    TEST_ASSERT_TRUE(PositionModule::shouldSendPositionToPhone(true, true, false, 0U, 0U, 60000U));
}

static void test_sendToPhone_holdsUntilCadenceElapses()
{
    TEST_ASSERT_FALSE(PositionModule::shouldSendPositionToPhone(true, true, true, 59999U, 1U, 60000U));
}

static void test_sendToPhone_sendsWhenCadenceElapses()
{
    TEST_ASSERT_TRUE(PositionModule::shouldSendPositionToPhone(true, true, true, 60001U, 1U, 60000U));
}

// No valid local position yet (e.g. GPS has no fix since boot): nothing to stream.
static void test_sendToPhone_requiresValidPosition()
{
    TEST_ASSERT_FALSE(PositionModule::shouldSendPositionToPhone(false, true, true, 60001U, 1U, 60000U));
}

// A backed-up toPhone queue means no client is draining it; don't pile on.
static void test_sendToPhone_requiresIdlePhoneQueue()
{
    TEST_ASSERT_FALSE(PositionModule::shouldSendPositionToPhone(true, false, true, 60001U, 1U, 60000U));
}

// millis() rollover: unsigned subtraction keeps the elapsed math correct across the wrap.
static void test_sendToPhone_survivesMillisRollover()
{
    constexpr uint32_t lastSent = UINT32_MAX - 29999U; // exactly 30,000 ms before the wrap to 0
    // "now" 40s after the wrap: 70,000 ms elapsed >= the 60s cadence, so it sends.
    TEST_ASSERT_TRUE(PositionModule::shouldSendPositionToPhone(true, true, true, 40000U, lastSent, 60000U));
    // "now" 10s after the wrap: only 40,000 ms elapsed, still held.
    TEST_ASSERT_FALSE(PositionModule::shouldSendPositionToPhone(true, true, true, 10000U, lastSent, 60000U));
}

// Regression: a send stamped at millis()==0 must still hold the cadence - the never-sent state
// is the everSentToPhone flag, not a lastSentMs==0 sentinel.
static void test_sendToPhone_sentAtTimeZeroStillHoldsCadence()
{
    TEST_ASSERT_FALSE(PositionModule::shouldSendPositionToPhone(true, true, true, 30000U, 0U, 60000U));
    TEST_ASSERT_TRUE(PositionModule::shouldSendPositionToPhone(true, true, true, 60000U, 0U, 60000U));
}

// Mesh time from GPS- or phone-sourced positions is trusted regardless of the fixed-position pref.
static void test_meshTime_gpsAndPhoneSourcesAlwaysTrusted()
{
    TEST_ASSERT_TRUE(PositionModule::meshTimeSourceTrusted(meshtastic_Position_LocSource_LOC_INTERNAL, false));
    TEST_ASSERT_TRUE(PositionModule::meshTimeSourceTrusted(meshtastic_Position_LocSource_LOC_EXTERNAL, false));
    TEST_ASSERT_TRUE(PositionModule::meshTimeSourceTrusted(meshtastic_Position_LocSource_LOC_INTERNAL, true));
    TEST_ASSERT_TRUE(PositionModule::meshTimeSourceTrusted(meshtastic_Position_LocSource_LOC_EXTERNAL, true));
}

// Upstream default: a fixed-position sender (e.g. an NTP-synced gateway) never sets our clock.
static void test_meshTime_fixedSourceRejectedByDefault()
{
    TEST_ASSERT_FALSE(PositionModule::meshTimeSourceTrusted(meshtastic_Position_LocSource_LOC_MANUAL, false));
}

// USERPREFS_MESH_TIME_RELAXED opts fixed-position senders in.
static void test_meshTime_fixedSourceAcceptedWhenOptedIn()
{
    TEST_ASSERT_TRUE(PositionModule::meshTimeSourceTrusted(meshtastic_Position_LocSource_LOC_MANUAL, true));
}

// An unset source says nothing about the sender's clock, so the opt-in does not cover it.
static void test_meshTime_unsetSourceNeverTrusted()
{
    TEST_ASSERT_FALSE(PositionModule::meshTimeSourceTrusted(meshtastic_Position_LocSource_LOC_UNSET, false));
    TEST_ASSERT_FALSE(PositionModule::meshTimeSourceTrusted(meshtastic_Position_LocSource_LOC_UNSET, true));
}

// The primary channel carries usable mesh time whatever its key and the pref.
static void test_meshTimeChannel_primaryAlwaysTrusted()
{
    TEST_ASSERT_TRUE(PositionModule::meshTimeChannelTrusted(true, true, false));
    TEST_ASSERT_TRUE(PositionModule::meshTimeChannelTrusted(true, false, false));
    TEST_ASSERT_TRUE(PositionModule::meshTimeChannelTrusted(true, true, true));
}

// Upstream default: no secondary channel sets our clock, private key or not.
static void test_meshTimeChannel_secondaryRejectedByDefault()
{
    TEST_ASSERT_FALSE(PositionModule::meshTimeChannelTrusted(false, false, false));
    TEST_ASSERT_FALSE(PositionModule::meshTimeChannelTrusted(false, true, false));
}

// USERPREFS_MESH_TIME_RELAXED admits a secondary channel only when its key is private.
static void test_meshTimeChannel_privateSecondaryAcceptedWhenOptedIn()
{
    TEST_ASSERT_TRUE(PositionModule::meshTimeChannelTrusted(false, false, true));
    TEST_ASSERT_FALSE(PositionModule::meshTimeChannelTrusted(false, true, true));
}

void setUp(void) {}

void tearDown(void) {}

extern "C" {
void setup()
{
    initializeTestEnvironment();
    UNITY_BEGIN();
    RUN_TEST(test_withinPrecisionCell_jitterStaysInCell);
    RUN_TEST(test_withinPrecisionCell_movingLatLeavesCell);
    RUN_TEST(test_withinPrecisionCell_movingLonLeavesCell);
    RUN_TEST(test_withinPrecisionCell_zeroPrecisionNeverSuppresses);
    RUN_TEST(test_withinPrecisionCell_fullPrecisionNeverSuppresses);
    RUN_TEST(test_effectiveInterval_stationaryRaisesToFloor);
    RUN_TEST(test_effectiveInterval_movingKeepsConfigured);
    RUN_TEST(test_effectiveInterval_longConfiguredWinsOverFloor);
    RUN_TEST(test_effectiveInterval_zeroFloorIsNoOp);
    RUN_TEST(test_sendToPhone_firstSendIsImmediate);
    RUN_TEST(test_sendToPhone_holdsUntilCadenceElapses);
    RUN_TEST(test_sendToPhone_sendsWhenCadenceElapses);
    RUN_TEST(test_sendToPhone_requiresValidPosition);
    RUN_TEST(test_sendToPhone_requiresIdlePhoneQueue);
    RUN_TEST(test_sendToPhone_survivesMillisRollover);
    RUN_TEST(test_sendToPhone_sentAtTimeZeroStillHoldsCadence);
    RUN_TEST(test_meshTime_gpsAndPhoneSourcesAlwaysTrusted);
    RUN_TEST(test_meshTime_fixedSourceRejectedByDefault);
    RUN_TEST(test_meshTime_fixedSourceAcceptedWhenOptedIn);
    RUN_TEST(test_meshTime_unsetSourceNeverTrusted);
    RUN_TEST(test_meshTimeChannel_primaryAlwaysTrusted);
    RUN_TEST(test_meshTimeChannel_secondaryRejectedByDefault);
    RUN_TEST(test_meshTimeChannel_privateSecondaryAcceptedWhenOptedIn);
    exit(UNITY_END());
}

void loop() {}
}
