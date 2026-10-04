#include "stdafx.h"

#include "DedicatedServer.h"

namespace
{
#ifdef DEDICATED_SERVER
volatile LONG g_shutdownRequested = 0;
bool g_controlHandlerRegistered = false;
HANDLE g_shutdownEvent = NULL;
char g_shutdownEventName[128] = {};
char g_startLevel[64] = {};
#endif

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

#ifdef DEDICATED_SERVER
bool parseStartLevel(const char* commandLine)
{
	static const char flag[] = "-zone_server_level=";
	g_startLevel[0] = '\0';
	if (!commandLine)
		return true;

	const size_t flagLength = sizeof(flag) - 1;
	for (const char* match = strstr(commandLine, flag); match; match = strstr(match + flagLength, flag))
	{
		const bool beginsAtBoundary = match == commandLine || isCommandLineBoundary(match[-1]) || match[-1] == '"';
		if (!beginsAtBoundary)
			continue;

		const char* value = match + flagLength;
		const char* end = value;
		while (*end && !isCommandLineBoundary(*end) && *end != '"')
			++end;

		const size_t valueLength = static_cast<size_t>(end - value);
		if (valueLength == 0 || valueLength >= sizeof(g_startLevel))
			return false;

		memcpy(g_startLevel, value, valueLength);
		g_startLevel[valueLength] = '\0';
		return true;
	}

	return true;
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
#endif
} // namespace

#ifdef DEDICATED_SERVER
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
	if (!parseStartLevel(commandLine))
	{
		writeConsoleMessage("Invalid -zone_server_level value. Use a level name shorter than 64 characters.\r\n");
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

const char* DedicatedServerStartLevel()
{
	return g_startLevel[0] ? g_startLevel : NULL;
}

void RequestDedicatedServerShutdown()
{
	InterlockedExchange(&g_shutdownRequested, 1);
	if (g_shutdownEvent)
		SetEvent(g_shutdownEvent);
}

#endif // DEDICATED_SERVER

bool ServerBootstrapTraceEnabled()
{
	return hasCommandLineFlag(Core.Params, "-zone_server_bootstrap_trace");
}

void TraceDedicatedServerBootstrap(const char* stage)
{
	if (!ServerBootstrapTraceEnabled())
		return;

	Msg("* [zone-server-trace] %s", stage ? stage : "unknown stage");
	FlushLog();
}
