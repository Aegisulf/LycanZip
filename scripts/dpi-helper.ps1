# Reads and changes the display scale of the main monitor (what Settings > System > Display > Scale does), for the tests that look at the layout
# at 100% / 125% / 150% / 200%. Dot-source it:  . .\scripts\dpi-helper.ps1
#   Get-DisplayScale            -> the current scale in percent (e.g. 200)
#   Set-DisplayScale 150        -> changes it (the windows are laid out again at once); returns $true when it took
# The caller must put the old value back (try / finally): the scale is a setting of the user, not of the test.
# Uses DisplayConfigGetDeviceInfo / SetDeviceInfo with the undocumented types -3 / -4 (the ones the Settings app uses).
Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
public static class DispScale {
  [StructLayout(LayoutKind.Sequential)] public struct LUID { public uint Low; public int High; }
  [StructLayout(LayoutKind.Sequential)] public struct HEADER { public int type; public uint size; public LUID adapterId; public uint id; }
  [StructLayout(LayoutKind.Sequential)] public struct GETDPI { public HEADER h; public int minRel, curRel, maxRel; }
  [StructLayout(LayoutKind.Sequential)] public struct SETDPI { public HEADER h; public int rel; }
  [StructLayout(LayoutKind.Sequential)] public struct PATH_SOURCE { public LUID adapterId; public uint id; public uint modeInfoIdx; public uint statusFlags; }
  [StructLayout(LayoutKind.Sequential)] public struct RATIONAL { public uint n, d; }
  [StructLayout(LayoutKind.Sequential)] public struct PATH_TARGET { public LUID adapterId; public uint id; public uint modeInfoIdx; public int outputTechnology, rotation, scaling; public RATIONAL refresh; public int scanLineOrdering; public int targetAvailable; public uint statusFlags; }
  [StructLayout(LayoutKind.Sequential)] public struct PATH { public PATH_SOURCE source; public PATH_TARGET target; public uint flags; }
  [StructLayout(LayoutKind.Explicit, Size = 64)] public struct MODE { [FieldOffset(0)] public int infoType; [FieldOffset(4)] public uint id; [FieldOffset(8)] public LUID adapterId; }
  [DllImport("user32.dll")] static extern int GetDisplayConfigBufferSizes(uint flags, out uint paths, out uint modes);
  [DllImport("user32.dll")] static extern int QueryDisplayConfig(uint flags, ref uint paths, [Out] PATH[] pathArray, ref uint modes, [Out] MODE[] modeArray, IntPtr topology);
  [DllImport("user32.dll")] static extern int DisplayConfigGetDeviceInfo(ref GETDPI p);
  [DllImport("user32.dll")] static extern int DisplayConfigSetDeviceInfo(ref SETDPI p);
  static readonly int[] Steps = { 100, 125, 150, 175, 200, 225, 250, 300, 350, 400, 450, 500 };
  static bool Source(out LUID adapter, out uint id) {
    adapter = new LUID(); id = 0;
    uint np, nm;
    if (GetDisplayConfigBufferSizes(2, out np, out nm) != 0) return false;   // QDC_ONLY_ACTIVE_PATHS
    PATH[] paths = new PATH[np]; MODE[] modes = new MODE[nm];
    if (QueryDisplayConfig(2, ref np, paths, ref nm, modes, IntPtr.Zero) != 0 || np == 0) return false;
    adapter = paths[0].source.adapterId; id = paths[0].source.id;
    return true;
  }
  static bool Get(out GETDPI g) {
    g = new GETDPI(); LUID a; uint id;
    if (!Source(out a, out id)) return false;
    g.h.type = -3; g.h.size = (uint)Marshal.SizeOf(typeof(GETDPI)); g.h.adapterId = a; g.h.id = id;
    return DisplayConfigGetDeviceInfo(ref g) == 0;
  }
  // the index in Steps of the recommended scale: the current one is recommended + curRel
  public static int Current() {
    GETDPI g; if (!Get(out g)) return -1;
    // the recommended scale is not given: it is the step where curRel is 0, found from the extremes (min is the smallest step that is offered)
    int recommended = Array.IndexOf(Steps, 100) - g.minRel;   // minRel <= 0 : the smallest offered step is recommended + minRel
    return Steps[Math.Max(0, Math.Min(Steps.Length - 1, recommended + g.curRel))];
  }
  public static bool Set(int percent) {
    GETDPI g; if (!Get(out g)) return false;
    int want = Array.IndexOf(Steps, percent); if (want < 0) return false;
    int recommended = Array.IndexOf(Steps, 100) - g.minRel;
    int rel = want - recommended;
    if (rel < g.minRel || rel > g.maxRel) return false;
    LUID a; uint id; Source(out a, out id);
    SETDPI s = new SETDPI(); s.h.type = -4; s.h.size = (uint)Marshal.SizeOf(typeof(SETDPI)); s.h.adapterId = a; s.h.id = id; s.rel = rel;
    return DisplayConfigSetDeviceInfo(ref s) == 0;
  }
  public static string Info() { GETDPI g; if (!Get(out g)) return "unknown"; return "min " + g.minRel + " cur " + g.curRel + " max " + g.maxRel; }
}
"@ -ErrorAction SilentlyContinue

function Get-DisplayScale { [DispScale]::Current() }
function Set-DisplayScale([int]$Percent) { $ok = [DispScale]::Set($Percent); Start-Sleep -Seconds 3; return $ok }
