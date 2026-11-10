#!/usr/bin/env bash

set -e

echo "This script requires https://jpa.kapsi.fi/nanopb/download/ version 0.4.9 to be located in the"
echo "firmware root directory if the following step fails, you should download the correct"
echo "prebuilt binaries for your computer into nanopb-0.4.9"

# Upstream's Telemetry carries our LocalStatsExtended (defined in protobufs-private/meshtastic/ondemand.proto)
# as oneof member 20. The protobufs/ submodule stays an unmodified checkout of upstream, so that member is
# added to a temporary copy of it, which is what gets generated. The copy sits next to protobufs/ so the
# relative paths below stay the same (and need no MSYS path conversion on Windows).
TMP_PROTOS=$(mktemp -d "$PWD/.regen-protos.XXXXXX")
trap 'rm -rf "$TMP_PROTOS"' EXIT
cp -R protobufs/meshtastic "$TMP_PROTOS/"
awk '
    /^package meshtastic;/ { print; print "import \"meshtastic/ondemand.proto\";"; next }
    /^message Telemetry \{/ { inTelemetry = 1 }
    inTelemetry && /oneof variant \{/ { inOneof = 1 }
    inOneof && /^[[:space:]]*\}/ {
        print "    LocalStatsExtended local_stats_extended = 20;"
        inOneof = 0
        inTelemetry = 0
    }
    { print }
' protobufs/meshtastic/telemetry.proto >"$TMP_PROTOS/meshtastic/telemetry.proto"
if ! grep -q 'import "meshtastic/ondemand.proto";' "$TMP_PROTOS/meshtastic/telemetry.proto" ||
	! grep -q 'LocalStatsExtended local_stats_extended = 20;' "$TMP_PROTOS/meshtastic/telemetry.proto"; then
	echo "Could not add local_stats_extended to Telemetry in telemetry.proto - did upstream change its layout?" >&2
	exit 1
fi

# the nanopb tool seems to require that the .options file be in the current directory!
cd "$TMP_PROTOS"
../nanopb-0.4.9/generator-bin/protoc --experimental_allow_proto3_optional "--nanopb_out=-S.cpp -v:../src/mesh/generated/" -I=. -I=../protobufs-private meshtastic/*.proto
cd ..

# protobufs-private/ holds our own portnum(s) and messages that have no place in
# https://github.com/meshtastic/protobufs - kept out of the protobufs/ submodule so that stays an
# unmodified checkout of upstream. Generated into the same src/mesh/generated/ as everything else.
cd protobufs-private
../nanopb-0.4.9/generator-bin/protoc --experimental_allow_proto3_optional "--nanopb_out=-S.cpp -v:../src/mesh/generated/" -I=../protobufs-private meshtastic/*.proto
