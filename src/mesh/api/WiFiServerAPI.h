#pragma once

#include "ServerAPI.h"
#include <WiFi.h>

#if HAS_ETHERNET && defined(ARCH_ESP32)
#include <ETH.h>
#endif // HAS_ETHERNET

/**
 * Serves the protobuf API to one Wi-Fi (or ESP32 Ethernet) TCP client.
 */
class WiFiServerAPI : public ServerAPI<WiFiClient>
{
  public:
    explicit WiFiServerAPI(WiFiClient &_client);
};

/**
 * Listens for incoming connections and does accepts and creates instances of WiFiServerAPI as needed
 */
class WiFiServerPort : public APIServerPort<WiFiServerAPI, WiFiServer>
{
  public:
    explicit WiFiServerPort(int port);
};

void initApiServer(int port = SERVER_API_DEFAULT_PORT);
void deInitApiServer();