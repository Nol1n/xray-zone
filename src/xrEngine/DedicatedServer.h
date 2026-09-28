#pragma once

#ifdef DEDICATED_SERVER
bool InitializeDedicatedServer(const char* commandLine);
void ShutdownDedicatedServer();
bool DedicatedServerShutdownRequested();
void RequestDedicatedServerShutdown();
const char* DedicatedServerStartLevel();
void TraceDedicatedServerBootstrap(const char* stage);
#endif // DEDICATED_SERVER
