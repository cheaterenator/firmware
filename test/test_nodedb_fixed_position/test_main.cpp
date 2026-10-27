// Fork: fixed-position backup. The fixed coordinates live in nodes.proto, which is rewritten in place
// (old file removed first); /prefs/fixedpos.proto is the atomic copy that must bring them back when the
// node DB is lost, while config still says fixed_position. The tests are a ladder (state=per-suite):
// arrange /prefs, then "reboot" a fresh NodeDB.
#include "MeshTypes.h" // Include BEFORE TestUtil.h
#include "TestUtil.h"
#include <unity.h>

#if defined(ARCH_PORTDUINO)
#define FPB_TEST_ENTRY extern "C"
#else
#define FPB_TEST_ENTRY
#endif

#include "FSCommon.h" // defines FSCom; must precede the feature guard below

// nodes.proto is only persisted once the node has a keypair, and the position must have a home.
#if defined(FSCom) && !(MESHTASTIC_EXCLUDE_PKI_KEYGEN || MESHTASTIC_EXCLUDE_PKI) && !MESHTASTIC_EXCLUDE_POSITIONDB

#include "mesh/NodeDB.h"
#include "mesh/TypeConversions.h"

namespace
{

const int32_t kLat = 521234567;
const int32_t kLon = 210123456;
const int32_t kAlt = 105;

// A real boot starts with a zeroed nodeDatabase and localPosition; in-process the globals retain the
// previous boot's state, so reset them first.
void rebootNodeDB()
{
    nodeDatabase.version = 0;
    nodeDatabase.nodes.clear();
    localPosition = meshtastic_Position_init_default;
    NodeDB *rebooted = new NodeDB();
    delete nodeDB;
    nodeDB = rebooted;
}

bool fileExists(const char *path)
{
    return FSCom.exists(path);
}

meshtastic_Position makePosition(int32_t lat, int32_t lon, int32_t alt)
{
    meshtastic_Position p = meshtastic_Position_init_default;
    p.latitude_i = lat;
    p.has_latitude_i = true;
    p.longitude_i = lon;
    p.has_longitude_i = true;
    p.altitude = alt;
    p.has_altitude = true;
    p.location_source = meshtastic_Position_LocSource_LOC_MANUAL;
    return p;
}

// What AdminModule's set_fixed_position handler does, minus the radio side effects.
void setFixedPositionLikeAdmin(int32_t lat, int32_t lon, int32_t alt)
{
    const meshtastic_Position p = makePosition(lat, lon, alt);
    nodeDB->updatePosition(nodeDB->getNodeNum(), p, RX_SRC_LOCAL);
    nodeDB->setLocalPosition(p);
    config.position.fixed_position = true;
    nodeDB->saveFixedPositionBackup(TypeConversions::ConvertToPositionLite(p));
    TEST_ASSERT_TRUE(nodeDB->saveToDisk(SEGMENT_NODEDATABASE | SEGMENT_CONFIG));
}

void assertOwnPosition(int32_t lat, int32_t lon, int32_t alt)
{
    meshtastic_PositionLite own;
    TEST_ASSERT_TRUE_MESSAGE(nodeDB->copyNodePosition(nodeDB->getNodeNum(), own), "own position slot missing");
    TEST_ASSERT_EQUAL_INT32(lat, own.latitude_i);
    TEST_ASSERT_EQUAL_INT32(lon, own.longitude_i);
    TEST_ASSERT_EQUAL_INT32(alt, own.altitude);
    // The broadcast path reads localPosition, so the boot restore must reach it too.
    TEST_ASSERT_EQUAL_INT32(lat, localPosition.latitude_i);
    TEST_ASSERT_EQUAL_INT32(lon, localPosition.longitude_i);
}

void assertBackup(int32_t lat, int32_t lon, int32_t alt)
{
    meshtastic_PositionLite backup;
    TEST_ASSERT_TRUE_MESSAGE(nodeDB->loadFixedPositionBackup(backup), "fixedpos.proto missing or empty");
    TEST_ASSERT_EQUAL_INT32(lat, backup.latitude_i);
    TEST_ASSERT_EQUAL_INT32(lon, backup.longitude_i);
    TEST_ASSERT_EQUAL_INT32(alt, backup.altitude);
}

} // namespace

void setUp(void) {}
void tearDown(void) {}

static void test_setFixedPosition_writesBackupAndSurvivesReboot(void)
{
    TEST_MESSAGE("=== Keyed boot, set fixed position, plain reboot keeps it ===");
    // A set region arms boot keygen; without a keypair nodes.proto is never written.
    config.lora.region = meshtastic_Config_LoRaConfig_RegionCode_US;
    TEST_ASSERT_TRUE(nodeDB->saveToDisk(SEGMENT_CONFIG));
    rebootNodeDB();
    TEST_ASSERT_EQUAL(32, owner.public_key.size);

    setFixedPositionLikeAdmin(kLat, kLon, kAlt);
    TEST_ASSERT_TRUE(fileExists(fixedPositionFileName));

    rebootNodeDB();

    TEST_ASSERT_TRUE(config.position.fixed_position);
    assertOwnPosition(kLat, kLon, kAlt);
    assertBackup(kLat, kLon, kAlt);
}

static void test_lostNodeDb_restoresFromBackup(void)
{
    TEST_MESSAGE("=== nodes.proto lost: coordinates come back from fixedpos.proto ===");
    TEST_ASSERT_TRUE(FSCom.remove(nodeDatabaseFileName));

    rebootNodeDB();

    TEST_ASSERT_TRUE(config.position.fixed_position);
    assertOwnPosition(kLat, kLon, kAlt);
    TEST_ASSERT_TRUE(fileExists(nodeDatabaseFileName));
}

static void test_restoredPosition_isPersistedInNodeDb(void)
{
    TEST_MESSAGE("=== The restore is written back to nodes.proto; the backup is then re-created ===");
    TEST_ASSERT_TRUE(FSCom.remove(fixedPositionFileName));

    rebootNodeDB();

    // Only nodes.proto could have supplied it this time.
    assertOwnPosition(kLat, kLon, kAlt);
    // A node DB entry without a backup (e.g. first boot on this build) gets one.
    assertBackup(kLat, kLon, kAlt);
}

static void test_staleBackup_isRewrittenFromNodeDb(void)
{
    TEST_MESSAGE("=== The node DB wins over a backup that disagrees ===");
    meshtastic_PositionLite stale = TypeConversions::ConvertToPositionLite(makePosition(kLat + 1000, kLon - 1000, kAlt + 1));
    nodeDB->saveFixedPositionBackup(stale);
    assertBackup(kLat + 1000, kLon - 1000, kAlt + 1);

    rebootNodeDB();

    assertOwnPosition(kLat, kLon, kAlt);
    assertBackup(kLat, kLon, kAlt);
}

static void test_zeroCoordinates_removeBackup(void)
{
    TEST_MESSAGE("=== A fixed position without coordinates leaves no backup ===");
    nodeDB->saveFixedPositionBackup(meshtastic_PositionLite_init_zero);
    TEST_ASSERT_FALSE(fileExists(fixedPositionFileName));

    nodeDB->saveFixedPositionBackup(TypeConversions::ConvertToPositionLite(makePosition(kLat, kLon, kAlt)));
    assertBackup(kLat, kLon, kAlt);
}

static void test_fixedFlagCleared_doesNotRestore(void)
{
    TEST_MESSAGE("=== fixed_position off: a leftover backup is ignored ===");
    config.position.fixed_position = false;
    TEST_ASSERT_TRUE(nodeDB->saveToDisk(SEGMENT_CONFIG));
    TEST_ASSERT_TRUE(fileExists(fixedPositionFileName));
    TEST_ASSERT_TRUE(FSCom.remove(nodeDatabaseFileName));

    rebootNodeDB();

    meshtastic_PositionLite own;
    const bool haveOwn = nodeDB->copyNodePosition(nodeDB->getNodeNum(), own);
    TEST_ASSERT_FALSE(haveOwn && (own.latitude_i != 0 || own.longitude_i != 0));
    TEST_ASSERT_EQUAL_INT32(0, localPosition.latitude_i);

    nodeDB->removeFixedPositionBackup();
    TEST_ASSERT_FALSE(fileExists(fixedPositionFileName));
}

FPB_TEST_ENTRY void setup()
{
    initializeTestEnvironment();
    nodeDB = new NodeDB(); // first boot on the pristine per-suite sandbox

    UNITY_BEGIN();
    RUN_TEST(test_setFixedPosition_writesBackupAndSurvivesReboot);
    RUN_TEST(test_lostNodeDb_restoresFromBackup);
    RUN_TEST(test_restoredPosition_isPersistedInNodeDb);
    RUN_TEST(test_staleBackup_isRewrittenFromNodeDb);
    RUN_TEST(test_zeroCoordinates_removeBackup);
    RUN_TEST(test_fixedFlagCleared_doesNotRestore);
    exit(UNITY_END());
}

FPB_TEST_ENTRY void loop() {}

#else // !FSCom || PKI or PositionDB excluded

void setUp(void) {}
void tearDown(void) {}

FPB_TEST_ENTRY void setup()
{
    initializeTestEnvironment();
    UNITY_BEGIN();
    exit(UNITY_END());
}

FPB_TEST_ENTRY void loop() {}

#endif
