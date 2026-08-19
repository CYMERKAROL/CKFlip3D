// ---------------------------------------------------------------------------
// The touchpad gesture vocabulary in one table: the token config.json carries,
// the name shown on screen, and the numbers the recogniser measures against.
// Kept in step by hand with the parser in hook/touchpadhook.cpp.
//
// Copyright © 2026 Karol Cymerman (CYMERKAROL) — https://github.com/CYMERKAROL/CKFlip3D
// ---------------------------------------------------------------------------
namespace CKFlip3D.Settings.Models;

/// <summary>
/// The touchpad gesture vocabulary: the tokens config.json carries, what each
/// one is called on screen, and the two numbers the recogniser needs.
///
/// One table rather than a <c>switch</c> per page, because the same four
/// diagonals have to be listed by the picker, named by the gesture list, named
/// again by the Controls summary and the diagnostics report, and MEASURED by
/// the preview recogniser — and a gesture that is spelled one way in the file
/// and another in the picker is a binding that silently never fires.
///
/// Keep in step with TouchpadHook::Parse*List (hook/touchpadhook.cpp); the two
/// are one contract.
/// </summary>
public static class TouchpadGestures
{
    /// <param name="Token">What config.json holds.</param>
    /// <param name="Label">What the settings window shows.</param>
    /// <param name="Fingers">Contacts the gesture is made with.</param>
    /// <param name="XSign">Opening diagonals only: +1 travels right ("\"), −1 left ("/").</param>
    public sealed record Choice(string Token, string Label, int Fingers, int XSign = 0);

    /// <summary>
    /// Strokes that OPEN the cascade. Diagonals on purpose: Windows' own slide
    /// recogniser only claims the four cardinal directions, so a diagonal is
    /// free and nothing of the user's touchpad configuration has to be touched.
    /// Two or four fingers, never three — three-finger slides are the ones
    /// Windows ships bound to Alt+Tab and Task View.
    /// </summary>
    public static readonly IReadOnlyList<Choice> Activate =
    [
        new("TwoDownRight",  "Two fingers ↘ (top-left → bottom-right)",  2, +1),
        new("TwoDownLeft",   "Two fingers ↙ (top-right → bottom-left)",  2, -1),
        new("FourDownRight", "Four fingers ↘ (top-left → bottom-right)", 4, +1),
        new("FourDownLeft",  "Four fingers ↙ (top-right → bottom-left)", 4, -1),
    ];

    /// <summary>Swipes that step through the stack while it is open.</summary>
    public static readonly IReadOnlyList<Choice> Cycle =
    [
        new("TwoSwipe",  "Two fingers left / right",  2),
        new("FourSwipe", "Four fingers left / right", 4),
    ];

    /// <summary>Gestures that confirm the selection — the touchpad's Enter.</summary>
    public static readonly IReadOnlyList<Choice> Commit =
    [
        new("OneTap",  "One-finger tap",   1),
        new("TwoTap",  "Two-finger tap",   2),
        new("TwoDown", "Two fingers down", 2),
    ];

    /// <summary>The gesture a token names, or null when nothing does.</summary>
    public static Choice? Find(IReadOnlyList<Choice> among, string? token) =>
        token == null ? null
            : among.FirstOrDefault(c => string.Equals(c.Token, token,
                                                      StringComparison.OrdinalIgnoreCase));

    /// <summary>
    /// The on-screen name of a token, or the token itself when it is one this
    /// build does not know — a hand-edited config should show what it says
    /// rather than a blank row.
    /// </summary>
    public static string Label(IReadOnlyList<Choice> among, string token) =>
        Find(among, token)?.Label ?? token;

    /// <summary>The live (not parked) entries of a list, in file order.</summary>
    public static List<Choice> Live(IReadOnlyList<Choice> among, IEnumerable<Binding> list) =>
        list.Where(b => b.Enabled)
            .Select(b => Find(among, b.Token))
            .Where(c => c != null)
            .Select(c => c!)
            .ToList();
}
