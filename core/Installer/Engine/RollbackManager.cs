using System.IO;

namespace CKFlip3D.Installer.Engine;

/// <summary>
/// Transactional safety net: every step that changes the machine registers an
/// undo action here. On failure or cancellation the actions run in reverse
/// order, returning the system to its pre-install state. Undo actions must
/// never throw out (each one is isolated) so one bad step can't strand the rest.
/// </summary>
public sealed class RollbackManager
{
    private readonly Stack<(string Description, Action Undo)> _actions = new();

    /// <summary>Directory holding pre-upgrade backups of overwritten files.</summary>
    public string BackupDir { get; } = Path.Combine(
        Path.GetTempPath(), "CKFlip3D.Setup.backup." + Environment.ProcessId);

    public void Push(string description, Action undo) => _actions.Push((description, undo));

    /// <summary>Move an existing file into the backup dir and register its restore.</summary>
    public void BackupFile(string path)
    {
        if (!File.Exists(path)) return;

        Directory.CreateDirectory(BackupDir);
        string backup = Path.Combine(BackupDir, Guid.NewGuid().ToString("N"));
        File.Move(path, backup);
        Push($"Restoring previous {Path.GetFileName(path)}", () =>
        {
            Directory.CreateDirectory(Path.GetDirectoryName(path)!);
            File.Copy(backup, path, overwrite: true);
        });
    }

    /// <summary>Undo everything, newest change first.</summary>
    public void RollbackAll(IProgress<InstallProgress>? progress = null)
    {
        int total = _actions.Count;
        int done = 0;
        while (_actions.Count > 0)
        {
            var (description, undo) = _actions.Pop();
            done++;
            progress?.Report(new InstallProgress(
                total == 0 ? 0 : done * 100.0 / total, "Rolling back changes…", description));
            try { undo(); }
            catch { /* keep unwinding — a failed undo must not stop the rest */ }
        }
        CleanupBackups();
    }

    /// <summary>Install succeeded: drop the undo log and the file backups.</summary>
    public void Commit()
    {
        _actions.Clear();
        CleanupBackups();
    }

    private void CleanupBackups()
    {
        try
        {
            if (Directory.Exists(BackupDir))
                Directory.Delete(BackupDir, recursive: true);
        }
        catch { /* stray temp files are harmless */ }
    }
}
