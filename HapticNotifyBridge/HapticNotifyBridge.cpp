#include <windows.h>
#include <winioctl.h>
#include <winevt.h>
#include <setupapi.h>
#include <cfgmgr32.h>
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

namespace {

constexpr wchar_t kServiceName[] = L"AW869XNotifySvc";
constexpr wchar_t kServiceDisplayName[] = L"AW869X Notification Service";
constexpr wchar_t kQuery[] = L"*";
constexpr DWORD kNotificationDebounceMs = 4000;
constexpr DWORD kSubscriptionRetryMs = 5000;
constexpr ULONG kNotificationIntensity = 50;
constexpr DWORD kPulseOnMs = 222;
constexpr LONGLONG kMaxLogSizeBytes = 300 * 1024;

SERVICE_STATUS g_serviceStatus = {};
SERVICE_STATUS_HANDLE g_serviceStatusHandle = nullptr;
HANDLE g_stopEvent = nullptr;
EVT_HANDLE g_subscriptionPush = nullptr;
HANDLE g_hwnHandle = INVALID_HANDLE_VALUE;
volatile ULONGLONG g_lastPulseTick = 0;
WCHAR g_logPath[MAX_PATH] = {};

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
    StringCchPrintfW(g_logPath, _countof(g_logPath), L"%s\\AW869XNotifySvc.log", logDir);
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

bool SendHwnState(HWN_STATE state, ULONG intensity)
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
        if (state == HWN_ON && error == ERROR_GEN_FAILURE) {
            Log(L"haptic-bridge: treating IOCTL_HWN_SET_STATE state=%lu intensity=%lu error=%lu as driver-managed success",
                (ULONG)state,
                intensity,
                error);
            return true;
        }
        Log(L"haptic-bridge: IOCTL_HWN_SET_STATE failed state=%lu intensity=%lu error=%lu", (ULONG)state, intensity, error);
        CloseHwnHandle();
        return false;
    }

    Log(L"haptic-bridge: state sent state=%lu intensity=%lu", (ULONG)state, intensity);
    return true;
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

    const ULONGLONG now = GetTickCount64();
    const ULONGLONG last = g_lastPulseTick;
    if ((now - last) < kNotificationDebounceMs) {
        if (eventHandle != nullptr) {
            EvtClose(eventHandle);
        }
        return ERROR_SUCCESS;
    }

    g_lastPulseTick = now;
    TriggerNotificationPulse();
    if (eventHandle != nullptr) {
        EvtClose(eventHandle);
    }
    return ERROR_SUCCESS;
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
    return StartSubscriptionForChannel(L"Microsoft-Windows-PushNotification-Platform/Operational", &g_subscriptionPush);
}

void StopSubscriptions()
{
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

void RunWorker()
{
    g_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (g_stopEvent == nullptr) {
        Log(L"haptic-bridge: CreateEvent failed, error=%lu", GetLastError());
        return;
    }

    EnsureHwnHandle();
    while (WaitForSingleObject(g_stopEvent, 0) != WAIT_OBJECT_0) {
        if (StartSubscriptions()) {
            WaitForSingleObject(g_stopEvent, INFINITE);
            break;
        }

        Log(L"haptic-bridge: subscription retry in %lu ms", kSubscriptionRetryMs);
        if (WaitForSingleObject(g_stopEvent, kSubscriptionRetryMs) == WAIT_OBJECT_0) {
            break;
        }
    }

    StopSubscriptions();
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
