using System.IO;
using System.Text;
using System.Text.Json;

namespace CKFlip3D.Settings.Services;

public enum DiagSeverity { Info = 0, Warning = 1, Critical = 2 }

/// <summary>One line of the log, exactly as the core wrote it.</summary>
public sealed record DiagEntry(
    long Id, string Code, DiagSeverity Severity, string Time,
    string Message, string Detail, bool Sticky);

/// <summary>
/// Every occurrence of one code, folded into the single thing the user reads.
/// A failure that happens on each activation is one problem, not forty.
/// </summary>
public sealed class DiagGroup
{
    public required string Code { get; init; }
    public required DiagSeverity Severity { get; init; }
    public required string Message { get; init; }
    public required string Detail { get; init; }
    public required int Count { get; init; }
    public required string FirstSeen { get; init; }
    public required string LastSeen { get; init; }
    public required long MaxId { get; init; }
    public required bool Sticky { get; init; }
    public required bool Unseen { get; init; }
}

/// <summary>
/// Reader and bookkeeper for %APPDATA%\CKFlip3D\diagnostics.jsonl — the log
/// the core appends to when something goes wrong.
///
/// The core never reads anything back: it appends whole lines and forgets
/// them.  Everything about what the USER has done with an entry — read it,
/// cleared it, told it never to come back — lives here, in a state file the
/// core does not know exists.  That split is what lets the switcher keep
/// running (or crash, or be killed) without ever waiting on this app.
///
/// Clearing is recorded as a WATERMARK rather than by deleting lines: "this
/// code is cleared up to id N".  A later occurrence has a higher id and comes
/// back, which is the point — a problem that happens again is news again.
/// Rewriting the file to delete lines would also mean a diagnostics system
/// that destroys evidence, and would race the core's appends while doing it.
/// </summary>
public static class DiagnosticsLog
{
    private const long StickyCleared = long.MaxValue;

    public static event Action? Changed;

    private static readonly object Gate = new();
    private static List<DiagGroup> _groups = [];
    private static FileSystemWatcher? _watcher;
    private static System.Windows.Threading.DispatcherTimer? _debounce;

    // ---- state -------------------------------------------------------------
    private static long _seenTo;
    private static long _clearedTo;
    private static Dictionary<string, long> _clearedCodes = new(StringComparer.OrdinalIgnoreCase);

    private static string Folder =>
        Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData),
                     "CKFlip3D");

    private static string LogFile => Path.Combine(Folder, "diagnostics.jsonl");
    private static string StateFile => Path.Combine(Folder, "diagnostics.state.json");

    public static IReadOnlyList<DiagGroup> Groups
    {
        get { lock (Gate) return _groups; }
    }

    public static int CountOf(DiagSeverity severity)
    {
        lock (Gate) return _groups.Count(g => g.Severity == severity);
    }

    public static int UnseenOf(DiagSeverity severity)
    {
        lock (Gate) return _groups.Count(g => g.Severity == severity && g.Unseen);
    }

    public static bool HasUnseen
    {
        get { lock (Gate) return _groups.Any(g => g.Unseen); }
    }

    // ---- reading -----------------------------------------------------------

    /// <summary>
    /// Re-read the file and rebuild the groups. Cheap enough to call on every
    /// change notification: the file is capped at a few hundred lines.
    /// </summary>
    public static void Refresh()
    {
        LoadState();
        var entries = ReadEntries();

        var groups = new List<DiagGroup>();
        foreach (var byCode in entries.GroupBy(e => e.Code, StringComparer.OrdinalIgnoreCase))
        {
            long clearedAt = Math.Max(_clearedTo,
                _clearedCodes.TryGetValue(byCode.Key, out long c) ? c : 0);

            var live = byCode.Where(e => e.Id > clearedAt).ToList();
            if (live.Count == 0) continue;

            // The newest occurrence carries the wording: a code whose message
            // was sharpened in a later build should read as the later build.
            var newest = live.OrderByDescending(e => e.Id).First();
            var oldest = live.OrderBy(e => e.Id).First();

            groups.Add(new DiagGroup
            {
                Code = newest.Code,
                Severity = newest.Severity,
                Message = newest.Message,
                Detail = newest.Detail,
                Count = live.Count,
                FirstSeen = oldest.Time,
                LastSeen = newest.Time,
                MaxId = newest.Id,
                Sticky = live.Any(e => e.Sticky),
                Unseen = newest.Id > _seenTo,
            });
        }

        // Worst first, then most recent — the order someone opening this page
        // in a hurry needs.
        groups.Sort((a, b) =>
        {
            int bySeverity = b.Severity.CompareTo(a.Severity);
            return bySeverity != 0 ? bySeverity : b.MaxId.CompareTo(a.MaxId);
        });

        lock (Gate) _groups = groups;
        Changed?.Invoke();
    }

    private static List<DiagEntry> ReadEntries()
    {
        var list = new List<DiagEntry>();
        try
        {
            if (!File.Exists(LogFile)) return list;

            // Share everything: the core may be appending to this file right
            // now, and a diagnostics reader that locks out the writer would be
            // a fine way to lose the very entry someone is waiting for.
            using var stream = new FileStream(LogFile, FileMode.Open, FileAccess.Read,
                                              FileShare.ReadWrite | FileShare.Delete);
            using var reader = new StreamReader(stream, Encoding.UTF8);
            string? line;
            while ((line = reader.ReadLine()) != null)
            {
                if (line.Length == 0) continue;
                if (Parse(line) is { } entry) list.Add(entry);
            }
        }
        catch
        {
            // An unreadable log is not worth an exception dialog; the page
            // simply shows what it managed to read.
        }
        return list;
    }

    /// <summary>
    /// One line, or null. A half-written final line is expected rather than
    /// exceptional — the core appends while this runs.
    /// </summary>
    private static DiagEntry? Parse(string line)
    {
        try
        {
            using var doc = JsonDocument.Parse(line);
            var root = doc.RootElement;
            if (root.ValueKind != JsonValueKind.Object) return null;

            long id = root.TryGetProperty("id", out var idEl) && idEl.TryGetInt64(out long v) ? v : 0;
            string code = root.TryGetProperty("code", out var cEl) ? cEl.GetString() ?? "" : "";
            if (id == 0 || code.Length == 0) return null;

            int sev = root.TryGetProperty("sev", out var sEl) && sEl.TryGetInt32(out int s) ? s : 1;
            string time = root.TryGetProperty("t", out var tEl) ? tEl.GetString() ?? "" : "";
            string msg = root.TryGetProperty("m", out var mEl) ? mEl.GetString() ?? "" : "";
            string det = root.TryGetProperty("d", out var dEl) ? dEl.GetString() ?? "" : "";
            bool sticky = root.TryGetProperty("k", out var kEl)
                          && kEl.ValueKind == JsonValueKind.Number && kEl.GetInt32() != 0;

            return new DiagEntry(id, code,
                (DiagSeverity)Math.Clamp(sev, 0, 2),
                time, msg, det, sticky);
        }
        catch
        {
            return null;
        }
    }

    // ---- writing (the Settings app's own findings) --------------------------

    /// <summary>
    /// Append an entry from this app. Same file, same format, same rules — the
    /// Settings app sees failures the core cannot (a missing core, a startup
    /// task that would not register) and they belong in the same list.
    /// </summary>
    public static void Append(string code, DiagSeverity severity, string message,
                              string? detail = null, bool sticky = false)
    {
        try
        {
            Directory.CreateDirectory(Folder);

            // Sticky entries state a fact about the machine rather than
            // something that just happened, so one is enough: re-appending on
            // every launch would turn "Windows 10 is not supported" into a
            // counter that climbs forever.
            if (sticky && Groups.Any(g => g.Code.Equals(code, StringComparison.OrdinalIgnoreCase)))
                return;

            long id = DateTime.UtcNow.ToFileTimeUtc();
            var sb = new StringBuilder(256);
            sb.Append("{\"id\":").Append(id)
              .Append(",\"sev\":").Append((int)severity)
              .Append(",\"code\":\"").Append(Escape(code))
              .Append("\",\"t\":\"").Append(DateTime.Now.ToString("yyyy-MM-ddTHH:mm:ss"))
              .Append("\",\"m\":\"").Append(Escape(message)).Append('"');
            if (!string.IsNullOrEmpty(detail))
                sb.Append(",\"d\":\"").Append(Escape(detail)).Append('"');
            if (sticky) sb.Append(",\"k\":1");
            sb.Append("}\n");

            using var stream = new FileStream(LogFile, FileMode.Append, FileAccess.Write,
                                              FileShare.ReadWrite | FileShare.Delete);
            byte[] bytes = Encoding.UTF8.GetBytes(sb.ToString());
            stream.Write(bytes, 0, bytes.Length);
        }
        catch
        {
            // Never the caller's problem.
        }
    }

    private static string Escape(string value)
    {
        var sb = new StringBuilder(value.Length + 8);
        foreach (char c in value)
        {
            switch (c)
            {
                case '"': sb.Append("\\\""); break;
                case '\\': sb.Append("\\\\"); break;
                case '\n': sb.Append("\\n"); break;
                case '\r': break;
                case '\t': sb.Append(' '); break;
                default: if (c >= ' ') sb.Append(c); break;
            }
        }
        return sb.ToString();
    }

    // ---- clearing ----------------------------------------------------------

    public static void Clear(DiagGroup group)
    {
        LoadState();
        // A sticky entry is cleared for good: the user has read the statement
        // and does not need it again on every launch. Everything else is
        // cleared only up to what has happened SO FAR, so a repeat comes back.
        _clearedCodes[group.Code] = group.Sticky ? StickyCleared : group.MaxId;
        if (group.MaxId > _seenTo) _seenTo = group.MaxId;
        SaveState();
        Refresh();
    }

    /// <summary>
    /// Retract a report whose subject has just been put right — the entry
    /// described a STATE ("CKFlip3D is not running"), that state is over, and
    /// leaving it listed would be the log saying something untrue about the
    /// machine as it is now.
    ///
    /// Goes through the ordinary Clear, so a non-sticky code is cleared only
    /// up to what has happened so far: if the core stops again, the entry is
    /// news again and comes back. Silent when the code is not listed, which is
    /// the normal case for a caller that fixes something pre-emptively.
    /// </summary>
    public static void ClearCode(string code)
    {
        // Read the file as it is NOW rather than trusting the last grouping.
        // The entry being retracted was usually appended by an EARLIER session
        // — that is the whole point of this call — so the groups may not have
        // been rebuilt since, and Clear works from group.MaxId: clearing to a
        // stale MaxId would leave the newest occurrence of the code still
        // listed, which is the exact bug this is here to prevent.
        Refresh();

        foreach (var g in Groups)
        {
            if (g.Code == code) { Clear(g); return; }
        }
    }

    public static void ClearAll()
    {
        LoadState();
        long max = 0;
        foreach (var g in Groups)
        {
            if (g.MaxId > max) max = g.MaxId;
            if (g.Sticky) _clearedCodes[g.Code] = StickyCleared;
        }
        if (max > _clearedTo) _clearedTo = max;
        if (max > _seenTo) _seenTo = max;
        SaveState();
        Refresh();
    }

    /// <summary>Everything currently listed stops counting as new.</summary>
    public static void MarkAllSeen()
    {
        LoadState();
        long max = _seenTo;
        foreach (var g in Groups)
            if (g.MaxId > max) max = g.MaxId;
        if (max == _seenTo) return;
        _seenTo = max;
        SaveState();
        Refresh();
    }

    // ---- state file --------------------------------------------------------

    private static void LoadState()
    {
        try
        {
            if (!File.Exists(StateFile)) return;
            using var doc = JsonDocument.Parse(File.ReadAllText(StateFile));
            var root = doc.RootElement;
            _seenTo = root.TryGetProperty("seenTo", out var s) && s.TryGetInt64(out long sv) ? sv : 0;
            _clearedTo = root.TryGetProperty("clearedTo", out var c) && c.TryGetInt64(out long cv) ? cv : 0;
            _clearedCodes = new Dictionary<string, long>(StringComparer.OrdinalIgnoreCase);
            if (root.TryGetProperty("codes", out var codes) && codes.ValueKind == JsonValueKind.Object)
                foreach (var p in codes.EnumerateObject())
                    if (p.Value.TryGetInt64(out long id)) _clearedCodes[p.Name] = id;
        }
        catch
        {
            // A corrupt state file means the user sees entries they had
            // already cleared — annoying, and the only safe direction to fail.
        }
    }

    private static void SaveState()
    {
        try
        {
            Directory.CreateDirectory(Folder);
            var sb = new StringBuilder(256);
            sb.Append("{\n  \"seenTo\": ").Append(_seenTo)
              .Append(",\n  \"clearedTo\": ").Append(_clearedTo)
              .Append(",\n  \"codes\": {");
            bool first = true;
            foreach (var kv in _clearedCodes)
            {
                sb.Append(first ? "\n    " : ",\n    ");
                sb.Append('"').Append(Escape(kv.Key)).Append("\": ").Append(kv.Value);
                first = false;
            }
            sb.Append(first ? "}\n}\n" : "\n  }\n}\n");
            File.WriteAllText(StateFile, sb.ToString(), Encoding.UTF8);
        }
        catch
        {
            // Nothing to do: the worst case is a cleared entry coming back.
        }
    }

    // ---- live updates ------------------------------------------------------

    /// <summary>
    /// Follow the file while the window is open, so a failure that happens
    /// WHILE someone is looking at the page appears without them doing
    /// anything. Debounced: the core can append several lines in a burst.
    /// </summary>
    public static void StartWatching()
    {
        if (_watcher != null) return;
        try
        {
            Directory.CreateDirectory(Folder);
            _debounce = new System.Windows.Threading.DispatcherTimer
            {
                Interval = TimeSpan.FromMilliseconds(400),
            };
            _debounce.Tick += (_, _) => { _debounce!.Stop(); Refresh(); };

            _watcher = new FileSystemWatcher(Folder, "diagnostics.jsonl")
            {
                NotifyFilter = NotifyFilters.LastWrite | NotifyFilters.Size
                             | NotifyFilters.FileName,
                EnableRaisingEvents = true,
            };
            void Bump(object? _, FileSystemEventArgs __) =>
                _debounce!.Dispatcher.BeginInvoke(() => { _debounce.Stop(); _debounce.Start(); });
            _watcher.Changed += Bump;
            _watcher.Created += Bump;
            _watcher.Deleted += Bump;
        }
        catch
        {
            _watcher = null;   // no live updates; the page still refreshes on open
        }
    }

    // ---- codes this app raises ---------------------------------------------

    public static class Code
    {
        public const string OsUnsupported = "CK0003";
        public const string CoreExeMissing = "CK0601";
        public const string CoreNotRunning = "CK0602";
        public const string ConfigSaveFailed = "CK0603";
        public const string StartupTaskFailed = "CK0604";
        public const string CoreUnreachable = "CK0605";
        public const string ConfigUnreadable = "CK0101";
    }

    /// <summary>
    /// The questions worth asking once, when the settings window opens: they
    /// are about the machine and the installation, and the core cannot answer
    /// them if it is the thing that is missing.
    /// </summary>
    public static void RunEnvironmentChecks()
    {
        Refresh();

        if (Environment.OSVersion.Version.Build is > 0 and < 22000)
        {
            Append(Code.OsUnsupported, DiagSeverity.Warning,
                   "This version of Windows is not supported",
                   $"Windows build {Environment.OSVersion.Version.Build} — CKFlip3D "
                   + "targets Windows 11 (build 22000 or newer). The cascade, the "
                   + "taskbar preview and the window capture it relies on all behave "
                   + "differently on Windows 10 and are not tested there.",
                   sticky: true);
        }

        // Both of these describe a STATE, not an event, so this scan has to be
        // able to take them back as well as raise them. It only ever appended
        // before, which meant an entry from a session where the core was absent
        // greeted someone who had since started it — the log reporting a fact
        // about the machine that had stopped being true. The liveness watch
        // does not cover this: it sees TRANSITIONS while the window is open, and
        // a core started before the window opened never produces one.
        if (CoreLocator.FindCoreExe() == null)
        {
            Append(Code.CoreExeMissing, DiagSeverity.Critical,
                   "CKFlip3D itself could not be found",
                   "the settings application is running, but CKFlip3D.exe is not "
                   + "next to it and not registered — nothing here can take effect, "
                   + "and the switcher cannot be started. Reinstall CKFlip3D.");
        }
        else
        {
            // The exe is where it belongs; retract any report that it was not.
            ClearCode(Code.CoreExeMissing);

            if (!CoreLocator.IsCoreRunning()) ReportCoreNotRunning();
            else                              ClearCode(Code.CoreNotRunning);
        }

        Refresh();
    }

    /// <summary>
    /// Raise CK0602. The single wording for it, because two callers now ask
    /// the same question at different moments — this scan, once, when the
    /// window opens, and the liveness watch, if the core dies while it is
    /// still open — and an entry that read differently depending on which
    /// noticed would be describing the same fact two ways.
    ///
    /// Not sticky: the answer changes the moment the switcher is started, and
    /// someone who cleared this while it was true should hear about it again
    /// next time it is.
    /// </summary>
    public static void ReportCoreNotRunning()
    {
        Append(Code.CoreNotRunning, DiagSeverity.Warning,
               "CKFlip3D is not running",
               "settings can be changed and saved, but the switcher and its "
               + "hotkey are dead until it is started.");
        Refresh();
    }
}
