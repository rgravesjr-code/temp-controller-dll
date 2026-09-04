using System.Windows;
using TempSim.Core.Native;

namespace TempSim.Wpf;

public partial class App : Application
{
    /// <summary>--screenshot out.png [--scenario NAME] [--seconds N]: run headless, render the window, save a PNG, exit.</summary>
    public static string? ScreenshotPath { get; private set; }
    public static string ScreenshotScenario { get; private set; } = "sensor-failover";
    public static int ScreenshotSeconds { get; private set; } = 70;

    void OnStartup(object sender, StartupEventArgs e)
    {
        for (int i = 0; i < e.Args.Length; i++)
        {
            switch (e.Args[i])
            {
                case "--screenshot": ScreenshotPath = e.Args[++i]; break;
                case "--scenario": ScreenshotScenario = e.Args[++i]; break;
                case "--seconds": ScreenshotSeconds = int.Parse(e.Args[++i]); break;
                case "--native-dir": NativeLoader.NativeDir = e.Args[++i]; break;
            }
        }
        DispatcherUnhandledException += (_, ex) =>
        {
            MessageBox.Show(ex.Exception.ToString(), "TempSim error");
            ex.Handled = ScreenshotPath == null;
            if (ScreenshotPath != null) Environment.Exit(3);
        };
        var w = new MainWindow();
        MainWindow = w;
        w.Show();
    }
}
