#pragma once

#ifdef DEDICATED_SERVER
bool InitializeDedicatedServer(const char* commandLine);
void ShutdownDedicatedServer();
bool DedicatedServerShutdownRequested();
void RequestDedicatedServerShutdown();
void TraceDedicatedServerBootstrap(const char* stage);
#endif // DEDICATED_SERVER
