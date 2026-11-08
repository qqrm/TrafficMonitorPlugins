using System;
using System.Collections.Generic;
using LibreHardwareMonitor.Hardware;

namespace HardwareMonitorTests
{
    // Keep the real LHM interfaces and property implementation. Only acquisition
    // is replaced: the test never calls Computer.Open or installs a driver.
    public sealed class RecoveryComputer : Computer
    {
        private readonly List<IHardware> hardware = new List<IHardware>();
        public static int RemainingOpenFailures = 1;
        public static int OpenAttempts;
        public static bool ThrowOnUpdate;
        public static bool SensorPresent;
        public static float NextValue = 42;
        public static bool[] LastOpenedFlags;

        public new IList<IHardware> Hardware { get { return hardware; } }

        public new void Open()
        {
            OpenAttempts++;
            LastOpenedFlags = new[] { IsCpuEnabled, IsGpuEnabled, IsMotherboardEnabled,
                IsStorageEnabled, IsBatteryEnabled, IsNetworkEnabled, IsMemoryEnabled,
                IsControllerEnabled, IsPsuEnabled };
            if (RemainingOpenFailures > 0)
            {
                RemainingOpenFailures--;
                throw new InvalidOperationException("Injected startup/retry Open failure");
            }
            hardware.Add(new RecoveryHardware());
        }

        public new void Close() { hardware.Clear(); }

        public new void Accept(IVisitor visitor)
        {
            foreach (IHardware device in hardware)
                device.Accept(visitor);
        }
    }

    internal sealed class MemorySettings : ISettings
    {
        public bool Contains(string name) { return false; }
        public string GetValue(string name, string fallback) { return fallback; }
        public void SetValue(string name, string value) { }
        public void Remove(string name) { }
    }

    internal sealed class RecoveryHardware : Hardware
    {
        private readonly RecoverySensor sensor;
        public RecoveryHardware()
            : base("Recovery fixture GPU", new Identifier("gpu-nvidia", "recovery-fixture"), new MemorySettings())
        {
            sensor = new RecoverySensor(this);
            ActivateSensor(sensor);
        }

        public override HardwareType HardwareType { get { return HardwareType.GpuNvidia; } }
        public override ISensor[] Sensors
        {
            get { return RecoveryComputer.SensorPresent ? base.Sensors : new ISensor[0]; }
        }
        public override void Update()
        {
            if (RecoveryComputer.ThrowOnUpdate)
                throw new InvalidOperationException("Injected driver update failure");
            sensor.CurrentValue = RecoveryComputer.NextValue;
        }
    }

    internal sealed class RecoverySensor : ISensor
    {
        public RecoverySensor(IHardware hardware) { Hardware = hardware; Name = "GPU Core"; }
        public float CurrentValue;
        public IControl Control { get { return null; } }
        public IHardware Hardware { get; private set; }
        public Identifier Identifier { get { return new Identifier(Hardware.Identifier, "temperature", "0"); } }
        public int Index { get { return 0; } }
        public bool IsDefaultHidden { get { return false; } }
        public float? Max { get { return CurrentValue; } }
        public float? Min { get { return CurrentValue; } }
        public string Name { get; set; }
        public IReadOnlyList<IParameter> Parameters { get { return new IParameter[0]; } }
        public SensorType SensorType { get { return SensorType.Temperature; } }
        public float? Value { get { return CurrentValue; } }
        public IEnumerable<SensorValue> Values { get { return new SensorValue[0]; } }
        public TimeSpan ValuesTimeWindow { get; set; }
        public void ResetMin() { }
        public void ResetMax() { }
        public void ClearValues() { }
        public void Accept(IVisitor visitor) { visitor.VisitSensor(this); }
        public void Traverse(IVisitor visitor) { }
    }
}

namespace System.Windows.Forms
{
    // Record diagnostics instead of blocking the unattended test on a modal box.
    public static class RecoveryMessageBox
    {
        public static readonly List<string> Messages = new List<string>();
        public static DialogResult Show(string text, string caption, MessageBoxButtons buttons)
        {
            Messages.Add(text);
            return DialogResult.OK;
        }
        public static DialogResult Show(string text, string caption, MessageBoxButtons buttons, MessageBoxIcon icon)
        {
            return Show(text, caption, buttons);
        }
    }
}
