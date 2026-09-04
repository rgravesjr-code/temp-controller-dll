using System.Reflection;
using System.Runtime.InteropServices;

namespace TempSim.Core.Native;

/// <summary>
/// Resolves "tempctl" and "cantp" to tempctl.dll / libtempctl.so and friends. Search order: the directory in
/// <see cref="NativeDir"/> (or the TEMPSIM_NATIVE_DIR environment variable), the application directory, then
/// the runtime's default probing.
/// </summary>
public static class NativeLoader
{
    public static string? NativeDir { get; set; } = Environment.GetEnvironmentVariable("TEMPSIM_NATIVE_DIR");
    static bool s_registered;

    public static void Register()
    {
        if (s_registered) return;
        s_registered = true;
        NativeLibrary.SetDllImportResolver(typeof(NativeLoader).Assembly, Resolve);
    }

    static IntPtr Resolve(string name, Assembly asm, DllImportSearchPath? path)
    {
        if (name != TempCtlNative.Lib && name != CanTpNative.Lib) return IntPtr.Zero;
        foreach (var dir in new[] { NativeDir, AppContext.BaseDirectory })
        {
            if (string.IsNullOrEmpty(dir)) continue;
            foreach (var file in Candidates(name))
            {
                var full = Path.Combine(dir, file);
                if (File.Exists(full) && NativeLibrary.TryLoad(full, out var h)) return h;
            }
        }
        return IntPtr.Zero;
    }

    static IEnumerable<string> Candidates(string name)
    {
        if (OperatingSystem.IsWindows()) { yield return name + ".dll"; }
        else if (OperatingSystem.IsMacOS()) { yield return "lib" + name + ".dylib"; }
        else { yield return "lib" + name + ".so"; yield return name + ".so"; }
    }

    /// <summary>Loaded library versions, for logs and the About text.</summary>
    public static string Describe()
    {
        Register();
        return $"tempctl {Ver(TempCtlNative.TcVersion())}, cantp {Ver(CanTpNative.CanTp_Version())}, {RuntimeInformation.RuntimeIdentifier}";
    }
    static string Ver(uint v) => $"{v >> 16}.{(v >> 8) & 0xFF}.{v & 0xFF}";
}
