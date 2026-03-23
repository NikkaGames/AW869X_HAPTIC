#include <windows.h>
#include <winioctl.h>
#include <winevt.h>
#include <setupapi.h>
#include <cfgmgr32.h>
#include <wtsapi32.h>
#include <userenv.h>
#include <cstdarg>
#include <cstdio>
#include <string>
#include <vector>
#include <strsafe.h>

#include <initguid.h>
#include <hwn.h>

#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "wevtapi.lib")
#pragma comment(lib, "wtsapi32.lib")
#pragma comment(lib, "userenv.lib")
#pragma comment(lib, "user32.lib")

namespace {

constexpr wchar_t kServiceName[] = L"aw869xhapticnotifysvc";
constexpr wchar_t kServiceDisplayName[] = L"AW869X Haptic Notification Service";
constexpr DWORD kNotificationDebounceMs = 750;
constexpr DWORD kSubscriptionRetryMs = 5000;
constexpr ULONG kNotificationIntensity = 50;
constexpr DWORD kPulseOnMs = 150;
constexpr ULONG kTypingIntensity = 50;
constexpr DWORD kTypingPulseOnMs = 50;
constexpr DWORD kTypingDebounceMs = 40;
constexpr LONGLONG kMaxLogSizeBytes = 300 * 1024;
constexpr ULONGLONG kWnfShelToastPublished = 0x0D83063EA3BD0035ull;

SERVICE_STATUS g_serviceStatus = {};
SERVICE_STATUS_HANDLE g_serviceStatusHandle = nullptr;
HANDLE g_stopEvent = nullptr;
EVT_HANDLE g_subscriptionPush = nullptr;
HANDLE g_hwnHandle = INVALID_HANDLE_VALUE;
volatile ULONGLONG g_lastPulseTick = 0;
WCHAR g_logPath[MAX_PATH] = {};
HMODULE g_ntdll = nullptr;
PVOID g_wnfSubscription = nullptr;
PROCESS_INFORMATION g_workerProcess = {};
HANDLE g_keyboardThread = nullptr;
DWORD g_keyboardThreadId = 0;
HHOOK g_keyboardHook = nullptr;
volatile ULONGLONG g_lastTypingPulseTick = 0;

typedef _Return_type_success_(return >= 0) LONG NTSTATUS;

typedef struct _WNF_TYPE_ID
{
    GUID TypeId;
} WNF_TYPE_ID, * PWNF_TYPE_ID;

typedef ULONG WNF_CHANGE_STAMP, * PWNF_CHANGE_STAMP;

using WNF_CALLBACK = NTSTATUS (NTAPI*)(ULONGLONG, PVOID, PVOID, PVOID, PVOID, PVOID);
using RTL_SUBSCRIBE_WNF_STATE_CHANGE_NOTIFICATION =
    NTSTATUS (NTAPI*)(PVOID*, ULONGLONG, ULONG, WNF_CALLBACK, PVOID, PVOID, PVOID, ULONG);
using RTL_UNSUBSCRIBE_WNF_STATE_CHANGE_NOTIFICATION =
    NTSTATUS (NTAPI*)(PVOID);
using NT_QUERY_WNF_STATE_DATA =
    NTSTATUS (NTAPI*)(PULONG64, PWNF_TYPE_ID, const VOID*, PWNF_CHANGE_STAMP, PVOID, PULONG);

RTL_SUBSCRIBE_WNF_STATE_CHANGE_NOTIFICATION g_RtlSubscribeWnfStateChangeNotification = nullptr;
RTL_UNSUBSCRIBE_WNF_STATE_CHANGE_NOTIFICATION g_RtlUnsubscribeWnfStateChangeNotification = nullptr;
NT_QUERY_WNF_STATE_DATA g_NtQueryWnfStateData = nullptr;

struct HWN_SET_PACKET_ONE {
    ULONG HwNPayloadSize;
    ULONG HwNPayloadVersion;
    ULONG HwNRequests;
    HWN_SETTINGS HwNSettingsInfo[1];
};

void Log(const wchar_t* fmt, ...)
{
    wchar_t buffer[1024];
    wchar_t line[1200];
    va_list args;
    HANDLE file = INVALID_HANDLE_VALUE;
    DWORD bytesWritten = 0;
    int utf8Bytes = 0;
    std::vector<char> utf8;
    SYSTEMTIME st = {};
    LARGE_INTEGER fileSize = {};
    LARGE_INTEGER zero = {};

    GetLocalTime(&st);
    va_start(args, fmt);
    _vsnwprintf_s(buffer, _countof(buffer), _TRUNCATE, fmt, args);
    va_end(args);
    StringCchPrintfW(
        line,
        _countof(line),
        L"[%04u-%02u-%02u %02u:%02u:%02u.%03u] %s\r\n",
        st.wYear,
        st.wMonth,
        st.wDay,
        st.wHour,
        st.wMinute,
        st.wSecond,
        st.wMilliseconds,
        buffer);
    OutputDebugStringW(line);
    if (g_logPath[0] == L'\0') {
        return;
    }
    file = CreateFileW(
        g_logPath,
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        if (GetFileSizeEx(file, &fileSize) && fileSize.QuadPart >= kMaxLogSizeBytes) {
            SetFilePointerEx(file, zero, nullptr, FILE_BEGIN);
            SetEndOfFile(file);
        }

        utf8Bytes = WideCharToMultiByte(CP_UTF8, 0, line, -1, nullptr, 0, nullptr, nullptr);
        if (utf8Bytes > 1) {
            utf8.resize((size_t)utf8Bytes);
            if (WideCharToMultiByte(CP_UTF8, 0, line, -1, utf8.data(), utf8Bytes, nullptr, nullptr) > 0) {
                SetFilePointerEx(file, zero, nullptr, FILE_END);
                WriteFile(file, utf8.data(), (DWORD)strlen(utf8.data()), &bytesWritten, nullptr);
            }
        }
        CloseHandle(file);
    }
}

void InitializeLogPath()
{
    WCHAR programData[MAX_PATH] = {};
    WCHAR logDir[MAX_PATH] = {};

    if (ExpandEnvironmentStringsW(L"%ProgramData%", programData, _countof(programData)) == 0) {
        return;
    }

    StringCchPrintfW(logDir, _countof(logDir), L"%s\\Nikka", programData);
    CreateDirectoryW(logDir, nullptr);
    StringCchPrintfW(g_logPath, _countof(g_logPath), L"%s\\aw869xhapticnotifysvc.log", logDir);
}

void UpdateServiceStatus(DWORD currentState, DWORD win32ExitCode, DWORD waitHint)
{
    if (g_serviceStatusHandle == nullptr) {
        return;
    }

    g_serviceStatus.dwCurrentState = currentState;
    g_serviceStatus.dwWin32ExitCode = win32ExitCode;
    g_serviceStatus.dwWaitHint = waitHint;
    g_serviceStatus.dwControlsAccepted =
        (currentState == SERVICE_START_PENDING) ? 0 : SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN;

    if (currentState == SERVICE_RUNNING || currentState == SERVICE_STOPPED) {
        g_serviceStatus.dwCheckPoint = 0;
    } else {
        g_serviceStatus.dwCheckPoint++;
    }

    SetServiceStatus(g_serviceStatusHandle, &g_serviceStatus);
}

void CloseHwnHandle()
{
    if (g_hwnHandle != INVALID_HANDLE_VALUE) {
        CloseHandle(g_hwnHandle);
        g_hwnHandle = INVALID_HANDLE_VALUE;
    }
}

void CloseWorkerProcess()
{
    if (g_workerProcess.hThread != nullptr) {
        CloseHandle(g_workerProcess.hThread);
        g_workerProcess.hThread = nullptr;
    }
    if (g_workerProcess.hProcess != nullptr) {
        CloseHandle(g_workerProcess.hProcess);
        g_workerProcess.hProcess = nullptr;
    }
    g_workerProcess.dwProcessId = 0;
    g_workerProcess.dwThreadId = 0;
}

void CloseKeyboardThread()
{
    if (g_keyboardThread != nullptr) {
        CloseHandle(g_keyboardThread);
        g_keyboardThread = nullptr;
    }
    g_keyboardThreadId = 0;
}

bool IsSessionZero()
{
    DWORD sessionId = 0;

    if (!ProcessIdToSessionId(GetCurrentProcessId(), &sessionId)) {
        Log(L"haptic-bridge: ProcessIdToSessionId failed error=%lu", GetLastError());
        return true;
    }

    return sessionId == 0;
}

bool GetFirstDeviceInterfacePath(const GUID& interfaceGuid, std::wstring& path)
{
    SP_DEVICE_INTERFACE_DATA ifData = {};
    DWORD requiredSize = 0;
    std::vector<BYTE> detailBuffer;
    auto deviceInfoSet = SetupDiGetClassDevsW(&interfaceGuid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);

    if (deviceInfoSet == INVALID_HANDLE_VALUE) {
        return false;
    }

    ifData.cbSize = sizeof(ifData);
    if (!SetupDiEnumDeviceInterfaces(deviceInfoSet, nullptr, &interfaceGuid, 0, &ifData)) {
        SetupDiDestroyDeviceInfoList(deviceInfoSet);
        return false;
    }

    SetupDiGetDeviceInterfaceDetailW(deviceInfoSet, &ifData, nullptr, 0, &requiredSize, nullptr);
    if (requiredSize == 0) {
        SetupDiDestroyDeviceInfoList(deviceInfoSet);
        return false;
    }

    detailBuffer.resize(requiredSize);
    auto detailData = reinterpret_cast<PSP_DEVICE_INTERFACE_DETAIL_DATA_W>(detailBuffer.data());
    detailData->cbSize = sizeof(*detailData);

    if (!SetupDiGetDeviceInterfaceDetailW(deviceInfoSet, &ifData, detailData, requiredSize, nullptr, nullptr)) {
        SetupDiDestroyDeviceInfoList(deviceInfoSet);
        return false;
    }

    path.assign(detailData->DevicePath);
    SetupDiDestroyDeviceInfoList(deviceInfoSet);
    return true;
}

bool EnsureHwnHandle()
{
    if (g_hwnHandle != INVALID_HANDLE_VALUE) {
        return true;
    }

    const GUID* guids[] = { &HWN_DEVINTERFACE_VIBRATOR, &HWN_DEVINTERFACE };
    std::wstring path;

    for (const GUID* guid : guids) {
        if (!GetFirstDeviceInterfacePath(*guid, path)) {
            continue;
        }

        g_hwnHandle = CreateFileW(
            path.c_str(),
            GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);
        if (g_hwnHandle != INVALID_HANDLE_VALUE) {
            Log(L"haptic-bridge: opened device %s", path.c_str());
            return true;
        }
    }

    Log(L"haptic-bridge: failed to open HWN device, error=%lu", GetLastError());
    return false;
}

bool SendHwnPacket(
    HWN_STATE state,
    ULONG intensity,
    ULONG period,
    ULONG dutyCycle,
    ULONG cycleCount)
{
    DWORD bytesReturned = 0;
    HWN_SET_PACKET_ONE packet = {};

    if (!EnsureHwnHandle()) {
        return false;
    }

    packet.HwNPayloadSize = sizeof(packet);
    packet.HwNPayloadVersion = 1;
    packet.HwNRequests = 1;
    packet.HwNSettingsInfo[0].HwNId = 0;
    packet.HwNSettingsInfo[0].HwNType = HWN_VIBRATOR;
    packet.HwNSettingsInfo[0].HwNSettings[HWN_INTENSITY] = intensity;
    packet.HwNSettingsInfo[0].HwNSettings[HWN_PERIOD] = period;
    packet.HwNSettingsInfo[0].HwNSettings[HWN_DUTY_CYCLE] = dutyCycle;
    packet.HwNSettingsInfo[0].HwNSettings[HWN_CYCLE_COUNT] = cycleCount;
    packet.HwNSettingsInfo[0].OffOnBlink = state;

    if (!DeviceIoControl(
            g_hwnHandle,
            IOCTL_HWN_SET_STATE,
            &packet,
            sizeof(packet),
            nullptr,
            0,
            &bytesReturned,
            nullptr)) {
        const DWORD error = GetLastError();
        if (state != HWN_OFF && error == ERROR_GEN_FAILURE) {
            Log(L"haptic-bridge: treating IOCTL_HWN_SET_STATE state=%lu intensity=%lu period=%lu duty=%lu cycles=%lu error=%lu as driver-managed success",
                (ULONG)state,
                intensity,
                period,
                dutyCycle,
                cycleCount,
                error);
            return true;
        }
        Log(L"haptic-bridge: IOCTL_HWN_SET_STATE failed state=%lu intensity=%lu period=%lu duty=%lu cycles=%lu error=%lu",
            (ULONG)state,
            intensity,
            period,
            dutyCycle,
            cycleCount,
            error);
        CloseHwnHandle();
        return false;
    }

    Log(L"haptic-bridge: state sent state=%lu intensity=%lu period=%lu duty=%lu cycles=%lu",
        (ULONG)state,
        intensity,
        period,
        dutyCycle,
        cycleCount);
    return true;
}

bool SendHwnState(HWN_STATE state, ULONG intensity)
{
    return SendHwnPacket(state, intensity, 0, 0, 0);
}

bool SendHwnPulse(ULONG intensity, DWORD durationMs)
{
    return SendHwnPacket(HWN_BLINK, intensity, durationMs, 100, 1);
}

bool TriggerNotificationPulse()
{
    if (!SendHwnState(HWN_ON, kNotificationIntensity)) {
        return false;
    }
    Log(L"haptic-bridge: notification pulse sent driverManagedOnMs=%lu intensity=%lu",
        (ULONG)kPulseOnMs,
        kNotificationIntensity);
    return true;
}

bool IsTypingVirtualKey(DWORD vkCode)
{
    switch (vkCode) {
    case VK_SHIFT:
    case VK_LSHIFT:
    case VK_RSHIFT:
    case VK_CONTROL:
    case VK_LCONTROL:
    case VK_RCONTROL:
    case VK_MENU:
    case VK_LMENU:
    case VK_RMENU:
    case VK_LWIN:
    case VK_RWIN:
    case VK_CAPITAL:
    case VK_NUMLOCK:
    case VK_SCROLL:
        return false;
    default:
        return true;
    }
}

bool TryTriggerTypingPulse(DWORD vkCode)
{
    const ULONGLONG now = GetTickCount64();
    ULONGLONG last;
    ULONGLONG delta;

    if (!IsTypingVirtualKey(vkCode)) {
        return false;
    }

    for (;;) {
        last = (ULONGLONG)InterlockedCompareExchange64((volatile LONG64*)&g_lastTypingPulseTick, 0, 0);
        delta = now - last;

        if (delta < kTypingDebounceMs) {
            Log(L"haptic-bridge: suppressed typing pulse vk=%lu deltaMs=%llu thresholdMs=%lu",
                (ULONG)vkCode,
                delta,
                kTypingDebounceMs);
            return false;
        }

        if ((ULONGLONG)InterlockedCompareExchange64((volatile LONG64*)&g_lastTypingPulseTick, (LONG64)now, (LONG64)last) == last) {
            break;
        }
    }

    Log(L"haptic-bridge: accepted typing pulse vk=%lu deltaMs=%llu intensity=%lu pulseMs=%lu",
        (ULONG)vkCode,
        delta,
        kTypingIntensity,
        kTypingPulseOnMs);
    return SendHwnPulse(kTypingIntensity, kTypingPulseOnMs);
}

bool TryTriggerNotificationPulse(const wchar_t* source)
{
    const ULONGLONG now = GetTickCount64();
    ULONGLONG last;
    ULONGLONG delta;

    for (;;) {
        last = (ULONGLONG)InterlockedCompareExchange64((volatile LONG64*)&g_lastPulseTick, 0, 0);
        delta = now - last;

        if (delta < kNotificationDebounceMs) {
            Log(L"haptic-bridge: suppressed duplicate source=%s deltaMs=%llu thresholdMs=%lu",
                source,
                delta,
                kNotificationDebounceMs);
            return false;
        }

        if ((ULONGLONG)InterlockedCompareExchange64((volatile LONG64*)&g_lastPulseTick, (LONG64)now, (LONG64)last) == last) {
            break;
        }
    }

    Log(L"haptic-bridge: accepted source=%s deltaMs=%llu", source, delta);
    return TriggerNotificationPulse();
}

NTSTATUS NTAPI WnfCallback(ULONGLONG, PVOID, PVOID, PVOID, PVOID, PVOID)
{
    TryTriggerNotificationPulse(L"WNF");
    return 0;
}

LRESULT CALLBACK KeyboardHookProc(int code, WPARAM wParam, LPARAM lParam)
{
    if (code == HC_ACTION &&
        (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN) &&
        lParam != 0) {
        const KBDLLHOOKSTRUCT* keyboard = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lParam);
        TryTriggerTypingPulse(keyboard->vkCode);
    }

    return CallNextHookEx(g_keyboardHook, code, wParam, lParam);
}

DWORD WINAPI KeyboardHookThreadProc(LPVOID)
{
    MSG msg = {};

    g_keyboardHook = SetWindowsHookExW(WH_KEYBOARD_LL, KeyboardHookProc, GetModuleHandleW(nullptr), 0);
    if (g_keyboardHook == nullptr) {
        Log(L"haptic-bridge: SetWindowsHookEx failed error=%lu", GetLastError());
        return 1;
    }

    Log(L"haptic-bridge: keyboard hook installed");
    PeekMessageW(&msg, nullptr, 0, 0, PM_NOREMOVE);

    while (true) {
        DWORD wait = MsgWaitForMultipleObjects(1, &g_stopEvent, FALSE, INFINITE, QS_ALLINPUT);
        if (wait == WAIT_OBJECT_0) {
            break;
        }
        if (wait == WAIT_OBJECT_0 + 1) {
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                if (msg.message == WM_QUIT) {
                    goto exit;
                }
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            continue;
        }

        Log(L"haptic-bridge: MsgWaitForMultipleObjects failed error=%lu", GetLastError());
        break;
    }

exit:
    if (g_keyboardHook != nullptr) {
        UnhookWindowsHookEx(g_keyboardHook);
        g_keyboardHook = nullptr;
    }
    Log(L"haptic-bridge: keyboard hook stopped");
    return 0;
}

bool StartKeyboardHookThread()
{
    if (g_keyboardThread != nullptr) {
        return true;
    }

    g_keyboardThread = CreateThread(nullptr, 0, KeyboardHookThreadProc, nullptr, 0, &g_keyboardThreadId);
    if (g_keyboardThread == nullptr) {
        Log(L"haptic-bridge: CreateThread for keyboard hook failed error=%lu", GetLastError());
        return false;
    }

    Log(L"haptic-bridge: keyboard hook thread started tid=%lu", g_keyboardThreadId);
    return true;
}

void StopKeyboardHookThread()
{
    if (g_keyboardThread == nullptr) {
        return;
    }

    WaitForSingleObject(g_keyboardThread, 5000);
    CloseKeyboardThread();
}

bool InitializeWnfApi()
{
    if (g_RtlSubscribeWnfStateChangeNotification != nullptr &&
        g_RtlUnsubscribeWnfStateChangeNotification != nullptr &&
        g_NtQueryWnfStateData != nullptr) {
        return true;
    }

    g_ntdll = GetModuleHandleW(L"ntdll.dll");
    if (g_ntdll == nullptr) {
        g_ntdll = LoadLibraryW(L"ntdll.dll");
    }

    if (g_ntdll == nullptr) {
        Log(L"haptic-bridge: failed to load ntdll, error=%lu", GetLastError());
        return false;
    }

    g_RtlSubscribeWnfStateChangeNotification =
        reinterpret_cast<RTL_SUBSCRIBE_WNF_STATE_CHANGE_NOTIFICATION>(
            GetProcAddress(g_ntdll, "RtlSubscribeWnfStateChangeNotification"));
    g_RtlUnsubscribeWnfStateChangeNotification =
        reinterpret_cast<RTL_UNSUBSCRIBE_WNF_STATE_CHANGE_NOTIFICATION>(
            GetProcAddress(g_ntdll, "RtlUnsubscribeWnfStateChangeNotification"));
    g_NtQueryWnfStateData =
        reinterpret_cast<NT_QUERY_WNF_STATE_DATA>(
            GetProcAddress(g_ntdll, "NtQueryWnfStateData"));

    if (g_RtlSubscribeWnfStateChangeNotification == nullptr ||
        g_RtlUnsubscribeWnfStateChangeNotification == nullptr ||
        g_NtQueryWnfStateData == nullptr) {
        Log(L"haptic-bridge: WNF APIs not available subscribe=%p unsubscribe=%p query=%p",
            g_RtlSubscribeWnfStateChangeNotification,
            g_RtlUnsubscribeWnfStateChangeNotification,
            g_NtQueryWnfStateData);
        return false;
    }

    return true;
}

bool RenderEventXml(EVT_HANDLE eventHandle, std::wstring& xml)
{
    DWORD bufferUsed = 0;
    DWORD propertyCount = 0;
    std::vector<wchar_t> buffer;

    if (!EvtRender(nullptr, eventHandle, EvtRenderEventXml, 0, nullptr, &bufferUsed, &propertyCount)) {
        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || bufferUsed == 0) {
            return false;
        }
    }

    buffer.resize(bufferUsed / sizeof(wchar_t) + 1);
    if (!EvtRender(nullptr, eventHandle, EvtRenderEventXml, (DWORD)(buffer.size() * sizeof(wchar_t)), buffer.data(), &bufferUsed, &propertyCount)) {
        return false;
    }

    xml.assign(buffer.data());
    return true;
}

DWORD WINAPI EventCallback(EVT_SUBSCRIBE_NOTIFY_ACTION action, PVOID, EVT_HANDLE eventHandle)
{
    std::wstring xml;

    if (action != EvtSubscribeActionDeliver) {
        return ERROR_SUCCESS;
    }

    if (eventHandle != nullptr && RenderEventXml(eventHandle, xml)) {
        Log(L"haptic-bridge: event=%s", xml.c_str());
        if (xml.find(L"<Task>10</Task>") == std::wstring::npos &&
            xml.find(L"<Task>18</Task>") == std::wstring::npos &&
            xml.find(L"<Task>22</Task>") == std::wstring::npos) {
            if (eventHandle != nullptr) {
                EvtClose(eventHandle);
            }
            return ERROR_SUCCESS;
        }
    }
    TryTriggerNotificationPulse(L"EVENTLOG");
    if (eventHandle != nullptr) {
        EvtClose(eventHandle);
    }
    return ERROR_SUCCESS;
}

bool StartWnfSubscription()
{
    std::vector<BYTE> stateBuffer(8192);
    ULONG stateBufferSize = (ULONG)stateBuffer.size();
    WNF_CHANGE_STAMP changeStamp = 0;
    NTSTATUS status = 0;
    ULONGLONG stateName = kWnfShelToastPublished;

    if (!InitializeWnfApi()) {
        return false;
    }

    status = g_NtQueryWnfStateData(
        &stateName,
        nullptr,
        nullptr,
        &changeStamp,
        stateBuffer.data(),
        &stateBufferSize);
    if (status < 0) {
        Log(L"haptic-bridge: NtQueryWnfStateData failed state=0x%I64x status=0x%08lx size=%lu",
            stateName,
            (ULONG)status,
            stateBufferSize);
        changeStamp = 0;
    }

    status = g_RtlSubscribeWnfStateChangeNotification(
        &g_wnfSubscription,
        stateName,
        changeStamp,
        WnfCallback,
        nullptr,
        nullptr,
        nullptr,
        1);
    if (status < 0 || g_wnfSubscription == nullptr) {
        Log(L"haptic-bridge: RtlSubscribeWnfStateChangeNotification failed state=0x%I64x status=0x%08lx",
            stateName,
            (ULONG)status);
        g_wnfSubscription = nullptr;
        return false;
    }

    Log(L"haptic-bridge: subscribed to WNF toast state=0x%I64x changestamp=%lu",
        stateName,
        changeStamp);
    return true;
}

bool StartSubscriptionForChannel(const wchar_t* channelPath, EVT_HANDLE* subscriptionHandle)
{
    *subscriptionHandle = EvtSubscribe(
        nullptr,
        nullptr,
        channelPath,
        nullptr,
        nullptr,
        nullptr,
        reinterpret_cast<EVT_SUBSCRIBE_CALLBACK>(EventCallback),
        EvtSubscribeToFutureEvents);

    if (*subscriptionHandle == nullptr) {
        Log(L"haptic-bridge: EvtSubscribe failed channel=%s error=%lu", channelPath, GetLastError());
        return false;
    }

    Log(L"haptic-bridge: subscribed to %s", channelPath);
    return true;
}

bool StartSubscriptions()
{
    if (StartWnfSubscription()) {
        return true;
    }

    Log(L"haptic-bridge: WNF subscription unavailable, falling back to event log subscription");
    return StartSubscriptionForChannel(L"Microsoft-Windows-PushNotification-Platform/Operational", &g_subscriptionPush);
}

void StopSubscriptions()
{
    if (g_wnfSubscription != nullptr && g_RtlUnsubscribeWnfStateChangeNotification != nullptr) {
        g_RtlUnsubscribeWnfStateChangeNotification(g_wnfSubscription);
        g_wnfSubscription = nullptr;
    }
    if (g_subscriptionPush != nullptr) {
        EvtClose(g_subscriptionPush);
        g_subscriptionPush = nullptr;
    }
}

DWORD WINAPI ServiceCtrlHandlerEx(DWORD control, DWORD, LPVOID, LPVOID)
{
    switch (control) {
    case SERVICE_CONTROL_STOP:
    case SERVICE_CONTROL_SHUTDOWN:
        UpdateServiceStatus(SERVICE_STOP_PENDING, NO_ERROR, 0);
        if (g_stopEvent != nullptr) {
            SetEvent(g_stopEvent);
        }
        return NO_ERROR;
    default:
        return ERROR_CALL_NOT_IMPLEMENTED;
    }
}

bool StartUserSessionWorker()
{
    DWORD sessionId = WTSGetActiveConsoleSessionId();
    HANDLE userToken = nullptr;
    HANDLE primaryToken = nullptr;
    LPVOID environment = nullptr;
    DWORD creationFlags = CREATE_NEW_PROCESS_GROUP | CREATE_NO_WINDOW;
    STARTUPINFOW startupInfo = {};
    wchar_t modulePath[MAX_PATH] = {};
    wchar_t commandLine[MAX_PATH * 2] = {};
    bool ok = false;

    if (sessionId == 0xFFFFFFFF) {
        Log(L"haptic-bridge: no active user session yet");
        return false;
    }

    if (GetModuleFileNameW(nullptr, modulePath, _countof(modulePath)) == 0) {
        Log(L"haptic-bridge: GetModuleFileName failed error=%lu", GetLastError());
        return false;
    }

    if (!WTSQueryUserToken(sessionId, &userToken)) {
        Log(L"haptic-bridge: WTSQueryUserToken failed session=%lu error=%lu", sessionId, GetLastError());
        return false;
    }

    if (!DuplicateTokenEx(
            userToken,
            TOKEN_ASSIGN_PRIMARY | TOKEN_DUPLICATE | TOKEN_QUERY | TOKEN_ADJUST_DEFAULT | TOKEN_ADJUST_SESSIONID,
            nullptr,
            SecurityImpersonation,
            TokenPrimary,
            &primaryToken)) {
        Log(L"haptic-bridge: DuplicateTokenEx failed session=%lu error=%lu", sessionId, GetLastError());
        goto cleanup;
    }

    if (CreateEnvironmentBlock(&environment, primaryToken, FALSE)) {
        creationFlags |= CREATE_UNICODE_ENVIRONMENT;
    } else {
        environment = nullptr;
    }

    startupInfo.cb = sizeof(startupInfo);
    startupInfo.dwFlags = STARTF_USESHOWWINDOW;
    startupInfo.wShowWindow = SW_HIDE;
    startupInfo.lpDesktop = const_cast<LPWSTR>(L"winsta0\\default");
    StringCchPrintfW(commandLine, _countof(commandLine), L"\"%s\" run", modulePath);

    ok = CreateProcessAsUserW(
        primaryToken,
        nullptr,
        commandLine,
        nullptr,
        nullptr,
        FALSE,
        creationFlags,
        environment,
        nullptr,
        &startupInfo,
        &g_workerProcess) != FALSE;
    if (!ok) {
        Log(L"haptic-bridge: CreateProcessAsUser failed session=%lu error=%lu", sessionId, GetLastError());
        goto cleanup;
    }

    Log(L"haptic-bridge: started user-session worker session=%lu pid=%lu",
        sessionId,
        g_workerProcess.dwProcessId);

cleanup:
    if (environment != nullptr) {
        DestroyEnvironmentBlock(environment);
    }
    if (primaryToken != nullptr) {
        CloseHandle(primaryToken);
    }
    if (userToken != nullptr) {
        CloseHandle(userToken);
    }

    return ok;
}

void RunWorker()
{
    g_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (g_stopEvent == nullptr) {
        Log(L"haptic-bridge: CreateEvent failed, error=%lu", GetLastError());
        return;
    }

    if (IsSessionZero()) {
        Log(L"haptic-bridge: running in session 0, supervising user-session worker");

        while (WaitForSingleObject(g_stopEvent, 0) != WAIT_OBJECT_0) {
            HANDLE waitHandles[2] = { g_stopEvent, g_workerProcess.hProcess };
            DWORD waitCount = (g_workerProcess.hProcess != nullptr) ? 2 : 1;
            DWORD waitResult;

            if (g_workerProcess.hProcess == nullptr && !StartUserSessionWorker()) {
                Log(L"haptic-bridge: worker retry in %lu ms", kSubscriptionRetryMs);
                if (WaitForSingleObject(g_stopEvent, kSubscriptionRetryMs) == WAIT_OBJECT_0) {
                    break;
                }
                continue;
            }

            waitResult = WaitForMultipleObjects(waitCount, waitHandles, FALSE, INFINITE);
            if (waitResult == WAIT_OBJECT_0) {
                break;
            }

            if (waitCount == 2 && waitResult == WAIT_OBJECT_0 + 1) {
                DWORD exitCode = 0;
                GetExitCodeProcess(g_workerProcess.hProcess, &exitCode);
                Log(L"haptic-bridge: user-session worker exited pid=%lu exit=%lu",
                    g_workerProcess.dwProcessId,
                    exitCode);
                CloseWorkerProcess();
                continue;
            }

            Log(L"haptic-bridge: WaitForMultipleObjects failed error=%lu", GetLastError());
            if (WaitForSingleObject(g_stopEvent, kSubscriptionRetryMs) == WAIT_OBJECT_0) {
                break;
            }
        }

        if (g_workerProcess.hProcess != nullptr) {
            Log(L"haptic-bridge: stopping user-session worker pid=%lu", g_workerProcess.dwProcessId);
            TerminateProcess(g_workerProcess.hProcess, 0);
            WaitForSingleObject(g_workerProcess.hProcess, 5000);
            CloseWorkerProcess();
        }

        CloseHandle(g_stopEvent);
        g_stopEvent = nullptr;
        return;
    }

    Log(L"haptic-bridge: running in user session");
    EnsureHwnHandle();
    while (WaitForSingleObject(g_stopEvent, 0) != WAIT_OBJECT_0) {
        if (StartSubscriptions()) {
            StartKeyboardHookThread();
            WaitForSingleObject(g_stopEvent, INFINITE);
            break;
        }

        Log(L"haptic-bridge: subscription retry in %lu ms", kSubscriptionRetryMs);
        if (WaitForSingleObject(g_stopEvent, kSubscriptionRetryMs) == WAIT_OBJECT_0) {
            break;
        }
    }

    StopSubscriptions();
    StopKeyboardHookThread();
    CloseHwnHandle();
    CloseHandle(g_stopEvent);
    g_stopEvent = nullptr;
}

void WINAPI ServiceMain(DWORD, LPWSTR*)
{
    g_serviceStatusHandle = RegisterServiceCtrlHandlerExW(kServiceName, ServiceCtrlHandlerEx, nullptr);
    if (g_serviceStatusHandle == nullptr) {
        return;
    }

    g_serviceStatus.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    g_serviceStatus.dwServiceSpecificExitCode = 0;

    UpdateServiceStatus(SERVICE_START_PENDING, NO_ERROR, 3000);
    UpdateServiceStatus(SERVICE_RUNNING, NO_ERROR, 0);

    RunWorker();

    UpdateServiceStatus(SERVICE_STOPPED, NO_ERROR, 0);
}

bool InstallService()
{
    wchar_t path[MAX_PATH];
    if (GetModuleFileNameW(nullptr, path, _countof(path)) == 0) {
        return false;
    }

    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CREATE_SERVICE);
    if (scm == nullptr) {
        return false;
    }

    SC_HANDLE service = CreateServiceW(
        scm,
        kServiceName,
        kServiceDisplayName,
        SERVICE_ALL_ACCESS,
        SERVICE_WIN32_OWN_PROCESS,
        SERVICE_AUTO_START,
        SERVICE_ERROR_NORMAL,
        path,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        nullptr);

    if (service == nullptr) {
        const DWORD error = GetLastError();
        CloseServiceHandle(scm);
        Log(L"haptic-bridge: CreateService failed, error=%lu", error);
        return false;
    }

    StartServiceW(service, 0, nullptr);
    CloseServiceHandle(service);
    CloseServiceHandle(scm);
    return true;
}

bool UninstallService()
{
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (scm == nullptr) {
        return false;
    }

    SC_HANDLE service = OpenServiceW(scm, kServiceName, SERVICE_STOP | DELETE | SERVICE_QUERY_STATUS);
    if (service == nullptr) {
        CloseServiceHandle(scm);
        return false;
    }

    SERVICE_STATUS status = {};
    ControlService(service, SERVICE_CONTROL_STOP, &status);
    const bool ok = DeleteService(service) != FALSE;

    CloseServiceHandle(service);
    CloseServiceHandle(scm);
    return ok;
}

} // namespace

int wmain(int argc, wchar_t** argv)
{
    InitializeLogPath();

    if (argc > 1) {
        if (_wcsicmp(argv[1], L"install") == 0) {
            return InstallService() ? 0 : 1;
        }
        if (_wcsicmp(argv[1], L"uninstall") == 0) {
            return UninstallService() ? 0 : 1;
        }
        if (_wcsicmp(argv[1], L"run") == 0) {
            RunWorker();
            return 0;
        }
        if (_wcsicmp(argv[1], L"pulse") == 0) {
            return TriggerNotificationPulse() ? 0 : 1;
        }
    }

    SERVICE_TABLE_ENTRYW dispatchTable[] = {
        { const_cast<LPWSTR>(kServiceName), ServiceMain },
        { nullptr, nullptr }
    };

    if (!StartServiceCtrlDispatcherW(dispatchTable)) {
        const DWORD error = GetLastError();
        if (error == ERROR_FAILED_SERVICE_CONTROLLER_CONNECT) {
            RunWorker();
            return 0;
        }
        return 1;
    }

    return 0;
}
