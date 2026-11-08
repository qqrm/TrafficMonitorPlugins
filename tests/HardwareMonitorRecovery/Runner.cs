using System;
using System.Globalization;
using System.Runtime.InteropServices;
using System.Threading;

internal static class Runner
{
    [DllImport("HardwareMonitor.RecoveryValidation.dll", CallingConvention = CallingConvention.Cdecl)]
    private static extern int RunHardwareMonitorRecoveryRegression(int liveStartup);

    [STAThread]
    private static int Main(string[] args)
    {
        Thread.CurrentThread.CurrentCulture = CultureInfo.InvariantCulture;
        Thread.CurrentThread.CurrentUICulture = CultureInfo.GetCultureInfo("en-US");
        return RunHardwareMonitorRecoveryRegression(args.Length == 1 && args[0] == "--live-startup" ? 1 : 0);
    }
}
