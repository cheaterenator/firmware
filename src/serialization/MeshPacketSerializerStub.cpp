// Stub for MeshPacketSerializer: the real impl (serialization/MeshPacketSerializer.cpp) needs
// jsoncpp (<json/json.h>) to back the native/portduino JSON packet-trace feature
// (config.yaml's Logging.JSONFile/TraceFile, see Router.cpp). [env:native-openwrt] excludes that
// dependency, but Router.cpp still references these symbols, so provide empty implementations to
// satisfy the link without pulling in jsoncpp. Mirrors platform/portduino/wasm/stubs/serializer_stub.cpp.
#include "serialization/MeshPacketSerializer.h"

std::string MeshPacketSerializer::JsonSerialize(const meshtastic_MeshPacket *mp, bool shouldLog)
{
    (void)mp;
    (void)shouldLog;
    return "";
}

std::string MeshPacketSerializer::JsonSerializeEncrypted(const meshtastic_MeshPacket *mp)
{
    (void)mp;
    return "";
}
