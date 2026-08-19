// ---------------------------------------------------------------------------
// Asking a running CKFlip3D to leave before its files are replaced.  The core
// has no visible window to close, so the usual Process helpers do not apply
// and the shutdown has to be requested the way the core actually listens for.
//
// Copyright © 2026 Karol Cymerman (CYMERKAROL) — https://github.com/CYMERKAROL/CKFlip3D
// ---------------------------------------------------------------------------
using System.Diagnostics;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;

namespace CKFlip3D.Installer.Engine;

/// <summary>
/// Stopping a running CKFlip3D before its files are replaced.
///
/// <para>Do NOT reach for <c>CloseMainWindow()</c> here.
/// <see cref="Process.MainWindowHandle"/> only ever finds a <em>visible</em>
/// owner-less window, and the core's window is deliberately hidden: it exists
/// to receive the tray callback and the settings broadcasts, not to be looked
/// at. So the handle is zero, the close posts nothing, and every install waits
/// out the full timeout before hard-killing the process.</para>
///
/// <para>A killed core never reaches its teardown. Windows it cloaked stay
/// cloaked (running, in the taskbar, invisible) and a hidden taskbar stays
/// hidden until something else repaints the shell, during an update, which is
/// when a user is least able to explain it. Its session marker also survives,
/// so the freshly installed version opens with CK0001 for a crash that never
/// happened.</para>
///
/// <para>So: ask the window that actually exists, wait for the process to leave
/// on its own terms, and if it will not, clear the marker for the stop that was
/// expected before insisting.</para>
/// </summary>
internal static class RunningApp
{
    /// <summary>The core's hidden message window (core/app.cpp kWindowClass).</summary>
    private const string CoreWindowClass = "CKFlip3D_MessageWindow";

    /// <summary>How long a process gets to close on its own.</summary>
    private const int GracefulMs = 6000;

    /// <summary>And to die after being killed.</summary>
    private const int KillWaitMs = 5000;

    /// <summary>
    /// Stop <paramref name="proc"/>: politely if it will, forcibly if it will
    /// not. Never throws — a process that is already gone, or that this
    /// installer cannot touch, is not a reason to fail an install.
    /// </summary>
    public static void Stop(Process proc)
    {
        try
        {
            bool asked = RequestClose(proc);
            if (asked && proc.WaitForExit(GracefulMs))
                return;                       // left on its own terms

            // It is being taken down deliberately, so the record must not read
            // as a crash on the next start.  Done BEFORE the kill: afterwards
            // the process is gone and its profile is harder to ask about.
            ClearSessionMarker(proc);

            if (!proc.HasExited)
            {
                proc.Kill();
                proc.WaitForExit(KillWaitMs);
            }
        }
        catch { /* already gone, or not ours to stop */ }
    }

    /// <summary>
    /// Post WM_CLOSE to the windows this process actually has. Returns false
    /// when there was nothing to ask, in which case only a kill is left.
    /// </summary>
    private static bool RequestClose(Process proc)
    {
        int pid = proc.Id;
        bool posted = false;

        // The core: its window is hidden, so it is found by class, not by
        // visibility.  WM_CLOSE reaches its message loop, which unwinds through
        // the normal exit path — uncloaking windows, restoring the taskbar,
        // removing the tray icon and releasing the session marker.
        EnumWindows((hwnd, _) =>
        {
            GetWindowThreadProcessId(hwnd, out uint windowPid);
            if (windowPid == (uint)pid && ClassNameOf(hwnd) == CoreWindowClass)
            {
                // Posted, not sent: the target may be in the middle of a frame,
                // and this must not block the installer on it.
                if (PostMessage(hwnd, WM_CLOSE, IntPtr.Zero, IntPtr.Zero))
                    posted = true;
            }
            return true;
        }, IntPtr.Zero);

        // The settings app is an ordinary window: it has a real main window and
        // CloseMainWindow is exactly right for it.
        if (!posted)
        {
            try { posted = proc.CloseMainWindow(); }
            catch { posted = false; }
        }
        return posted;
    }

    /// <summary>
    /// Delete %APPDATA%\CKFlip3D\session.lock for the user who owns
    /// <paramref name="proc"/>.
    ///
    /// <para>The owner matters: an installer elevated by a standard user runs
    /// under the administrator account, so its own %APPDATA% is not the one the
    /// switcher writes to. The path is therefore resolved from the target
    /// process's own token, and only falls back to this process's profile when
    /// that cannot be asked.</para>
    /// </summary>
    private static void ClearSessionMarker(Process proc)
    {
        foreach (string dir in CandidateAppDataDirs(proc))
        {
            try
            {
                string marker = Path.Combine(dir, "session.lock");
                if (File.Exists(marker))
                    File.Delete(marker);
            }
            catch { /* best effort: a stale marker costs one false log entry */ }
        }
    }

    private static IEnumerable<string> CandidateAppDataDirs(Process proc)
    {
        string? owned = null;
        try
        {
            if (OpenProcessToken(proc.Handle, TOKEN_QUERY, out IntPtr token))
            {
                try
                {
                    Guid roaming = FolderIdRoamingAppData;
                    if (SHGetKnownFolderPath(ref roaming, 0, token, out IntPtr buf) == 0
                        && buf != IntPtr.Zero)
                    {
                        try { owned = Marshal.PtrToStringUni(buf); }
                        finally { CoTaskMemFree(buf); }
                    }
                }
                finally { CloseHandle(token); }
            }
        }
        catch { /* fall through to this process's own profile */ }

        if (!string.IsNullOrEmpty(owned))
            yield return Path.Combine(owned!, InstallContext.AppName);
        yield return InstallContext.AppDataDir;
    }

    private static string ClassNameOf(IntPtr hwnd)
    {
        var sb = new StringBuilder(256);
        int n = GetClassName(hwnd, sb, sb.Capacity);
        return n > 0 ? sb.ToString(0, n) : string.Empty;
    }

    // ---- interop ---------------------------------------------------------
    private const uint WM_CLOSE = 0x0010;
    private const uint TOKEN_QUERY = 0x0008;
    private static Guid FolderIdRoamingAppData =
        new("3EB685DB-65F9-4CF6-A03A-E3EF65729F3D");

    private delegate bool EnumWindowsProc(IntPtr hwnd, IntPtr param);

    [DllImport("user32.dll")]
    private static extern bool EnumWindows(EnumWindowsProc callback, IntPtr param);

    [DllImport("user32.dll")]
    private static extern uint GetWindowThreadProcessId(IntPtr hwnd, out uint pid);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    private static extern int GetClassName(IntPtr hwnd, StringBuilder text, int count);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    private static extern bool PostMessage(IntPtr hwnd, uint msg, IntPtr wParam, IntPtr lParam);

    [DllImport("advapi32.dll", SetLastError = true)]
    private static extern bool OpenProcessToken(IntPtr process, uint access, out IntPtr token);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool CloseHandle(IntPtr handle);

    [DllImport("shell32.dll")]
    private static extern int SHGetKnownFolderPath(ref Guid folderId, uint flags,
                                                   IntPtr token, out IntPtr path);

    [DllImport("ole32.dll")]
    private static extern void CoTaskMemFree(IntPtr mem);
}
