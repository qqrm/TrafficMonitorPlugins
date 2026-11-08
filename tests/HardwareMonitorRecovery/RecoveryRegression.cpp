#include "HardwareMonitor.h"
#include "HardwareMonitorHelper.h"
#include "Common.h"
#include "../utilities/IniHelper.h"
#include <cmath>
#include <stdexcept>

using namespace HardwareMonitor;
using namespace HardwareMonitorTests;
using namespace System::IO;
using namespace System::Threading;
using namespace System::Windows::Forms;

namespace
{
    class RecordingHost : public ITrafficMonitor
    {
    public:
        int notifications{};
        int GetAPIVersion() override { return 1; }
        const wchar_t* GetVersion() override { return L"recovery-test"; }
        double GetMonitorValue(MonitorItem) override { return 0; }
        const wchar_t* GetMonitorValueString(MonitorItem, int) override { return L""; }
        void ShowNotifyMessage(const wchar_t*) override { ++notifications; }
        unsigned short GetLanguageId() const override { return 1033; }
        const wchar_t* GetPluginConfigDir() const override { return L""; }
        int GetDPI(DPIType) const override { return 96; }
        unsigned int GetThemeColor() const override { return 0; }
    };

    const wchar_t* sensor_id = L"/gpu-nvidia/recovery-fixture/Temperature/GPU Core";
    const wchar_t* missing_id = L"/storage/unavailable/Temperature/Offline Drive";
    const wchar_t* flag_names[] = { L"IsCpuEnabled", L"IsGpuEnabled", L"IsMotherboardEnabled",
        L"IsStorageEnabled", L"IsBatteryEnabled", L"IsNetworkEnabled", L"IsMemoryEnabled",
        L"IsControllerEnabled", L"IsPsuEnabled" };
    const bool saved_flags[] = { false, true, false, false, true, true, true, false, true };

    void Require(bool condition, const char* message)
    {
        if (!condition)
            throw std::runtime_error(message);
    }

    void SeedConfig(const std::wstring& path)
    {
        utilities::CIniHelper ini(path);
        for (int i = 0; i < 9; ++i)
            ini.WriteBool(L"hardware", flag_names[i], saved_flags[i]);
        ini.WriteInt(L"config", L"item_count", 2);
        ini.WriteBool(L"config", L"hardware_info_auto_refresh", true);
        ini.WriteBool(L"config", L"show_mouse_tooltip", false);
        for (int i = 0; i < 2; ++i)
        {
            std::wstring section = L"item" + std::to_wstring(i);
            ini.WriteString(section.c_str(), L"identifier", i == 0 ? sensor_id : missing_id);
            ini.WriteInt(section.c_str(), L"decimal_places", 2);
            ini.WriteBool(section.c_str(), L"specify_value_width", false);
            ini.WriteInt(section.c_str(), L"value_width", 5);
            ini.WriteString(section.c_str(), L"unit", L"\u00b0C");
            ini.WriteBool(section.c_str(), L"show_unit", false);
            ini.WriteBool(section.c_str(), L"show_notify", i == 1);
            ini.WriteDouble(section.c_str(), L"notify_value", 87.5);
        }
        ini.WriteStringList(L"other", L"hardware_info_collapsed_nodes", { L"saved tree node" });
        Require(ini.Save(), "Could not create the isolated INI fixture");
    }

    void CheckSavedConfig(const std::wstring& path)
    {
        utilities::CIniHelper ini(path);
        for (int i = 0; i < 9; ++i)
            Require(ini.GetBool(L"hardware", flag_names[i], !saved_flags[i]) == saved_flags[i],
                "SaveConfig changed a saved hardware enable flag");
        Require(ini.GetInt(L"config", L"item_count") == 2, "SaveConfig lost the selected sensor count");
        Require(ini.GetBool(L"config", L"hardware_info_auto_refresh"), "Auto-refresh setting was lost");
        Require(!ini.GetBool(L"config", L"show_mouse_tooltip", true), "Tooltip setting was lost");
        for (int i = 0; i < 2; ++i)
        {
            std::wstring section = L"item" + std::to_wstring(i);
            Require(ini.GetString(section.c_str(), L"identifier") == (i == 0 ? sensor_id : missing_id),
                "SaveConfig changed a selected identifier or its order");
            Require(ini.GetInt(section.c_str(), L"decimal_places") == 2, "Decimal places were lost");
            Require(!ini.GetBool(section.c_str(), L"specify_value_width", true), "Width mode was lost");
            Require(ini.GetInt(section.c_str(), L"value_width") == 5, "Width was lost");
            Require(ini.GetString(section.c_str(), L"unit") == L"\u00b0C", "Unit was lost");
            Require(!ini.GetBool(section.c_str(), L"show_unit", true), "Unit display setting was lost");
            Require(ini.GetBool(section.c_str(), L"show_notify") == (i == 1), "Notification setting was lost");
            Require(std::fabs(ini.GetDouble(section.c_str(), L"notify_value") - 87.5) < 0.001,
                "Notification threshold was lost");
        }
        std::vector<std::wstring> nodes;
        ini.GetStringList(L"other", L"hardware_info_collapsed_nodes", nodes, {});
        Require(nodes.size() == 1 && nodes[0] == L"saved tree node", "Tree expansion state was lost");
    }

    void ExerciseRecovery(String^ directory)
    {
        std::wstring config_dir = HardwareMonitor::Common::StringToStdWstring(directory) + L"\\";
        std::wstring path = config_dir + L"HardwareMonitor.ini";
        SeedConfig(path);

        auto monitor = CHardwareMonitor::GetInstance();
        ITMPlugin* plugin = monitor;
        auto global = MonitorGlobal::Instance();
        Require(global->computer == nullptr && RecoveryComputer::OpenAttempts == 1,
            "The startup fault was not exercised");
        Require(RecoveryMessageBox::Messages->Count == 1, "Startup failure was not reported once");

        // Match PluginTester: config -> host init -> metadata -> item enumeration.
        plugin->OnExtenedInfo(ITMPlugin::EI_CONFIG_DIR, config_dir.c_str());
        RecordingHost host;
        plugin->OnInitialize(&host);
        for (int i = 0; i < ITMPlugin::TMI_MAX; ++i)
            Require(plugin->GetInfo(static_cast<ITMPlugin::PluginInfoIndex>(i)) != nullptr, "Plugin metadata unavailable");
        IPluginItem* first = plugin->GetItem(0);
        IPluginItem* unavailable = plugin->GetItem(1);
        Require(first != nullptr && unavailable != nullptr && plugin->GetItem(2) == nullptr,
            "RECOVERY_ASSERT: startup Open failure discarded the configured sensor choices");
        Require(RecoveryMessageBox::Messages->Count == 1, "Loading an offline configuration raised another error");
        const std::wstring first_id = first->GetItemId();
        const std::wstring unavailable_id = unavailable->GetItemId();

        // Both dialogs must be constructible while acquisition is unavailable.
        auto settings = gcnew SettingsForm();
        auto info = gcnew HardwareInfoForm();
        delete settings;
        delete info;
        monitor->SaveConfig();
        CheckSavedConfig(path);

        plugin->DataRequired();
        Require(global->computer != nullptr && RecoveryComputer::OpenAttempts == 2,
            "DataRequired did not retry the failed startup");
        for (int i = 0; i < 9; ++i)
            Require(RecoveryComputer::LastOpenedFlags[i] == saved_flags[i], "Retry used default hardware flags");

        // A sensor appears without another Open. An absent lookup must not be cached forever.
        RecoveryComputer::SensorPresent = true;
        plugin->DataRequired();
        Require(plugin->GetItem(0) == first && plugin->GetItem(1) == unavailable,
            "Recovery invalidated an item pointer retained by the host");
        Require(std::wstring(first->GetItemId()) == first_id && std::wstring(unavailable->GetItemId()) == unavailable_id,
            "Recovery changed an item's identity");
        Require(std::fabs(first->GetResourceUsageGraphValue() - 0.42f) < 0.001f,
            "The original item did not receive the newly available sensor value");
        Require(std::wstring(first->GetItemName()).find(L"Recovery fixture GPU") != std::wstring::npos,
            "Placeholder metadata was not resolved");
        monitor->SaveConfig();
        CheckSavedConfig(path);

        // Also exercise a later Update failure followed by a failed Open and a successful retry.
        RecoveryComputer::ThrowOnUpdate = true;
        RecoveryComputer::RemainingOpenFailures = 1;
        Thread::Sleep(5100); // Respect the production retry throttle.
        plugin->DataRequired();
        Require(global->computer == nullptr && RecoveryComputer::OpenAttempts == 3,
            "The runtime failed-rebuild path was not exercised");
        monitor->SaveConfig();
        CheckSavedConfig(path);
        RecoveryComputer::ThrowOnUpdate = false;
        RecoveryComputer::NextValue = 84;
        Thread::Sleep(5100);
        plugin->DataRequired();
        Require(RecoveryComputer::OpenAttempts == 4 && plugin->GetItem(0) == first,
            "The second retry did not preserve the original item");
        Require(std::fabs(first->GetResourceUsageGraphValue() - 0.84f) < 0.001f,
            "The original item still refers to a sensor from the old computer");
        monitor->SaveConfig();
        CheckSavedConfig(path);
        Require(host.notifications == 0, "An item with notifications disabled sent a notification");
        global->UnInit();
    }

    void ExerciseMetadataBeforeInitialize(String^ directory)
    {
        RecoveryComputer::RemainingOpenFailures = 0;
        RecoveryComputer::SensorPresent = true;
        std::wstring config_dir = HardwareMonitor::Common::StringToStdWstring(directory) + L"\\";
        std::wstring path = config_dir + L"HardwareMonitor.ini";
        SeedConfig(path);
        utilities::CIniHelper ini(path);
        ini.WriteInt(L"config", L"item_count", 1);
        ini.WriteBool(L"item0", L"show_notify", true);
        ini.WriteDouble(L"item0", L"notify_value", 1);
        Require(ini.Save(), "Could not prepare the notification fixture");

        auto monitor = CHardwareMonitor::GetInstance();
        ITMPlugin* plugin = monitor;
        RecordingHost host;
        Require(monitor->GetMainApp() == nullptr, "The host was initialized too early");
        plugin->OnExtenedInfo(ITMPlugin::EI_CONFIG_DIR, config_dir.c_str());
        plugin->OnInitialize(&host);
        for (int i = 0; i < ITMPlugin::TMI_MAX; ++i)
            Require(plugin->GetInfo(static_cast<ITMPlugin::PluginInfoIndex>(i)) != nullptr, "Plugin metadata unavailable");
        IPluginItem* first = plugin->GetItem(0);
        Require(first != nullptr && plugin->GetItem(1) == nullptr, "Live startup lost its selected item");
        Require(RecoveryMessageBox::Messages->Count == 0, "Live configuration loading raised an error");
        Require(std::wstring(first->GetItemName()).find(L"Recovery fixture GPU") != std::wstring::npos,
            "The host could not enumerate the resolved display name");
        Require(first->GetResourceUsageGraphValue() == 0, "Configuration loading polled item values before OnInitialize");
        Require(host.notifications == 0, "Metadata lookup emitted a notification");
        plugin->DataRequired();
        Require(host.notifications == 1, "The first normal poll did not emit the configured threshold notification");
        Require(std::fabs(first->GetResourceUsageGraphValue() - 0.42f) < 0.001f, "The first normal poll lost its value");
        MonitorGlobal::Instance()->UnInit();
    }
}

extern "C" __declspec(dllexport) int RunHardwareMonitorRecoveryRegression(int live_startup)
{
    String^ directory = Path::Combine(Path::GetTempPath(), "hardware-monitor-recovery-" + Guid::NewGuid().ToString("N"));
    Directory::CreateDirectory(directory);
    try
    {
        if (live_startup)
            ExerciseMetadataBeforeInitialize(directory);
        else
            ExerciseRecovery(directory);
        Console::WriteLine("RECOVERY_REGRESSION_PASS");
        return 0;
    }
    catch (const std::exception& error)
    {
        Console::Error->WriteLine(gcnew String(error.what()));
        return 1;
    }
    catch (System::Exception^ error)
    {
        Console::Error->WriteLine(error);
        return 2;
    }
    finally
    {
        Directory::Delete(directory, true);
    }
}
