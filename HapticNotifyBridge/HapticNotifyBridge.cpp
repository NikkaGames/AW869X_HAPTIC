#include <windows.h>
#include <winioctl.h>
#include <winevt.h>
#include <setupapi.h>
#include <cfgmgr32.h>
#include <cstdarg>
#include <cstdio>
#include <string>
#include <vector>

#include <initguid.h>
#include <hwn.h>

#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "wevtapi.lib")

namespace {

constexpr wchar_t kServiceName[] = L"HapticNotifyBridge";
constexpr wchar_t kServiceDisplayName[] = L"Haptic Notification Bridge";
constexpr wchar_t kChannelPath[] = L"Microsoft-Windows-PushNotification-Platform/Operational";
constexpr wchar_t kQuery[] = L"*";
constexpr DWORD kNotificationDebounceMs = 1500;
constexpr ULONG kNotificationIntensity = 50;
constexpr ULONG kNotificationPeriodMs = 120;
constexpr ULONG kNotificationDutyCycle = 100;
constexpr ULONG kNotificationCycles = 1;

SERVICE_STATUS g_serviceStatus = {};
SERVICE_STATUS_HANDLE g_serviceStatusHandle = nullptr;
HANDLE g_stopEvent = nullptr;
EVT_HANDLE g_subscription = nullptr;
HANDLE g_hwnHandle = INVALID_HANDLE_VALUE;
volatile ULONGLONG g_lastPulseTick = 0;

struct HWN_SET_PACKET_ONE {
    ULONG HwNPayloadSize;
    ULONG HwNPayloadVersion;
    ULONG HwNRequests;
    HWN_SETTINGS HwNSettingsInfo[1];
};

void Log(const wchar_t* fmt, ...)
{
    wchar_t buffer[512];
    va_list args;
    va_start(args, fmt);
    _vsnwprintf_s(buffer, _countof(buffer), _TRUNCATE, fmt, args);
    va_end(args);
    OutputDebugStringW(buffer);
    OutputDebugStringW(L"\n");
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

bool TriggerNotificationPulse()
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
    packet.HwNSettingsInfo[0].HwNSettings[HWN_INTENSITY] = kNotificationIntensity;
    packet.HwNSettingsInfo[0].HwNSettings[HWN_PERIOD] = kNotificationPeriodMs;
    packet.HwNSettingsInfo[0].HwNSettings[HWN_DUTY_CYCLE] = kNotificationDutyCycle;
    packet.HwNSettingsInfo[0].HwNSettings[HWN_CYCLE_COUNT] = kNotificationCycles;
    packet.HwNSettingsInfo[0].OffOnBlink = HWN_BLINK;

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
        Log(L"haptic-bridge: IOCTL_HWN_SET_STATE failed, error=%lu", error);
        CloseHwnHandle();
        return false;
    }

    Log(L"haptic-bridge: notification pulse sent");
    return true;
}

DWORD WINAPI EventCallback(EVT_SUBSCRIBE_NOTIFY_ACTION action, PVOID, EVT_HANDLE eventHandle)
{
    if (action != EvtSubscribeActionDeliver) {
        return ERROR_SUCCESS;
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

bool StartSubscription()
{
    g_subscription = EvtSubscribe(
        nullptr,
        g_stopEvent,
        kChannelPath,
        kQuery,
        nullptr,
        nullptr,
        reinterpret_cast<EVT_SUBSCRIBE_CALLBACK>(EventCallback),
        EvtSubscribeToFutureEvents);

    if (g_subscription == nullptr) {
        Log(L"haptic-bridge: EvtSubscribe failed, error=%lu", GetLastError());
        return false;
    }

    Log(L"haptic-bridge: subscribed to %s", kChannelPath);
    return true;
}

void StopSubscription()
{
    if (g_subscription != nullptr) {
        EvtClose(g_subscription);
        g_subscription = nullptr;
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
    if (!StartSubscription()) {
        SetEvent(g_stopEvent);
    }

    WaitForSingleObject(g_stopEvent, INFINITE);

    StopSubscription();
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
    }

    SERVICE_TABLE_ENTRYW dispatchTable[] = {
        { const_cast<LPWSTR>(kServiceName), ServiceMain },
        { nullptr, nullptr }
    };

    if (!StartServiceCtrlDispatcherW(dispatchTable)) {
        return 1;
    }

    return 0;
}
