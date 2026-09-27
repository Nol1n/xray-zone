#include "stdafx.h"

#ifdef DEDICATED_SERVER
#include "DedicatedServer.h"

namespace
{
volatile LONG g_shutdownRequested = 0;
bool g_controlHandlerRegistered = false;
HANDLE g_shutdownEvent = NULL;
char g_shutdownEventName[128] = {};

bool isCommandLineBoundary(char value)
{
	return value == '\0' || value == ' ' || value == '\t' || value == '\r' || value == '\n';
}

bool hasCommandLineFlag(const char* commandLine, const char* flag)
{
	if (!commandLine || !flag || !*flag)
		return false;

	const size_t flagLength = strlen(flag);
	for (const char* match = strstr(commandLine, flag); match; match = strstr(match + flagLength, flag))
	{
		const bool beginsAtBoundary = match == commandLine || isCommandLineBoundary(match[-1]);
		if (beginsAtBoundary && isCommandLineBoundary(match[flagLength]))
			return true;
	}

	return false;
}

void writeConsoleMessage(const char* message)
{
	HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
	if (!message || output == NULL || output == INVALID_HANDLE_VALUE)
		return;

	DWORD written = 0;
	const DWORD length = static_cast<DWORD>(strlen(message));
	if (!WriteConsoleA(output, message, length, &written, NULL))
		WriteFile(output, message, length, &written, NULL);
}

BOOL WINAPI handleConsoleControl(DWORD controlType)
{
	switch (controlType)
	{
	case CTRL_C_EVENT:
	case CTRL_BREAK_EVENT:
	case CTRL_CLOSE_EVENT:
	case CTRL_LOGOFF_EVENT:
	case CTRL_SHUTDOWN_EVENT:
		InterlockedExchange(&g_shutdownRequested, 1);
		return TRUE;
	default:
		return FALSE;
	}
}
} // namespace

bool InitializeDedicatedServer(const char* commandLine)
{
	if (!AttachConsole(ATTACH_PARENT_PROCESS))
	{
		const DWORD attachError = GetLastError();
		if (attachError != ERROR_ACCESS_DENIED && !AllocConsole())
			return false;
	}

	if (!hasCommandLineFlag(commandLine, "-zone_server"))
	{
		writeConsoleMessage("Usage: AnomalyDedicated.exe -zone_server [server startup arguments]\r\n");
		return false;
	}

	InterlockedExchange(&g_shutdownRequested, 0);
	_snprintf_s(
		g_shutdownEventName,
		sizeof(g_shutdownEventName),
		_TRUNCATE,
		"Local\\STALKERAlive.DedicatedServer.%lu.Shutdown",
		static_cast<unsigned long>(GetCurrentProcessId())
	);
	g_shutdownEvent = CreateEventA(NULL, TRUE, FALSE, g_shutdownEventName);
	if (!g_shutdownEvent)
	{
		g_shutdownEventName[0] = '\0';
		writeConsoleMessage("Failed to create the dedicated server shutdown event.\r\n");
		return false;
	}
	Msg("* [zone-server] shutdown event: %s", g_shutdownEventName);
	FlushLog();
	g_controlHandlerRegistered = SetConsoleCtrlHandler(handleConsoleControl, TRUE) != FALSE;
	if (!g_controlHandlerRegistered)
	{
		CloseHandle(g_shutdownEvent);
		g_shutdownEvent = NULL;
		writeConsoleMessage("Failed to register the dedicated server console shutdown handler.\r\n");
		return false;
	}

	return true;
}

void ShutdownDedicatedServer()
{
	if (g_controlHandlerRegistered)
	{
		SetConsoleCtrlHandler(handleConsoleControl, FALSE);
		g_controlHandlerRegistered = false;
	}
	if (g_shutdownEvent)
	{
		CloseHandle(g_shutdownEvent);
		g_shutdownEvent = NULL;
	}
	g_shutdownEventName[0] = '\0';
}

bool DedicatedServerShutdownRequested()
{
	if (InterlockedCompareExchange(&g_shutdownRequested, 0, 0) != 0)
		return true;
	return g_shutdownEvent && WaitForSingleObject(g_shutdownEvent, 0) == WAIT_OBJECT_0;
}

void RequestDedicatedServerShutdown()
{
	InterlockedExchange(&g_shutdownRequested, 1);
	if (g_shutdownEvent)
		SetEvent(g_shutdownEvent);
}

void TraceDedicatedServerBootstrap(const char* stage)
{
	if (!hasCommandLineFlag(Core.Params, "-zone_server_bootstrap_trace"))
		return;

	Msg("* [zone-server-trace] %s", stage ? stage : "unknown stage");
	FlushLog();
}
#endif // DEDICATED_SERVER
