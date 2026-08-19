// ---------------------------------------------------------------------------
// Writing the desktop shortcut that OPENS THE CASCADE, which is a different
// thing from starting the program.  It points at CKFlip3D.Launch.exe, and that
// separation is the whole point of the feature.
//
// Copyright © 2026 Karol Cymerman (CYMERKAROL) — https://github.com/CYMERKAROL/CKFlip3D
// ---------------------------------------------------------------------------
using System.IO;
using CKFlip3D.Settings.Interop;

namespace CKFlip3D.Settings.Services;

/// <summary>
/// "Create launch shortcut" (General → Startup): a desktop shortcut that opens
/// the CASCADE, as opposed to starting the program.
///
/// Its target is CKFlip3D.Launch.exe, never CKFlip3D.exe, and that separation
/// is the whole feature. The core requires administrator, so a shortcut to it
/// would ask for elevation on every use, and starting it has never opened
/// anything anyway. The launcher runs as the ordinary user, hands a running
/// core a request it accepts from nowhere else, and only reaches for the core
/// exe, and so for a UAC prompt, when there is nothing running to ask.
///
/// Not a setting, so not part of the Apply flow: it writes one file on demand,
/// and from then on the file is the user's like any other shortcut. There is
/// deliberately no "remove" counterpart, which would mean claiming ownership
/// of a file they may have moved, renamed or pinned long ago.
///
/// The user's own desktop rather than the all-users one, so no elevation is
/// needed. Windows 11 has no supported way to pin to the taskbar
/// programmatically, so it goes where it can be used and dragged from.
/// </summary>
public static class LaunchShortcutService
{
    private const string ShortcutName = "CKFlip3D Cascade.lnk";

    /// <summary>Keep in step with core/Installer/Engine/InstallContext.cs (uninstall sweeps it).</summary>
    public static string ShortcutPath => Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.DesktopDirectory), ShortcutName);

    /// <summary>
    /// Write the shortcut, replacing one of the same name. Returns null on
    /// success, otherwise a human-readable reason.
    /// </summary>
    public static string? Create()
    {
        try
        {
            string? launcher = CoreLocator.FindLauncherExe();
            if (launcher == null)
                return "CKFlip3D.Launch.exe could not be located — the shortcut was not created.";

            // The icon comes from the CORE exe when it is there: both carry the
            // same cascade icon, but a shortcut whose icon resolves through the
            // main program keeps looking right in places that cache it.
            string? icon = CoreLocator.FindCoreExe() ?? launcher;

            ShellLink.Create(
                ShortcutPath, launcher,
                description: "Open the CKFlip3D cascade",
                workingDirectory: Path.GetDirectoryName(launcher),
                iconPath: icon, iconIndex: 0);
            return null;
        }
        catch (Exception ex)
        {
            return ex.Message;
        }
    }
}
