#pragma once

bool ServerBootstrapTraceEnabled();
void TraceDedicatedServerBootstrap(const char* stage);

#ifdef DEDICATED_SERVER
bool InitializeDedicatedServer(const char* commandLine);
void ShutdownDedicatedServer();
bool DedicatedServerShutdownRequested();
void RequestDedicatedServerShutdown();
const char* DedicatedServerStartLevel();
#endif // DEDICATED_SERVER
