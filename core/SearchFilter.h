#pragma once

#include <string>
#include <vector>

/// Type-to-filter query for the cascade (Settings → Search).
///
/// Holds the typed text and answers one question: does this window match?
/// It owns no window state and touches nothing else — the controller decides
/// what to do with the verdict, which is what keeps the filtering mechanics
/// (windows leaving and re-entering the stack) entirely in one place.
///
/// Matching is case-insensitive, over SPACE-SEPARATED tokens, and every token
/// must appear somewhere in the window's title or its executable name.  So
/// "code set" finds "Settings — Visual Studio Code" regardless of the order
/// the words appear in, which is what people actually type when they are
/// hunting for a window they can already see.
class SearchFilter {
public:
    /// Longest query worth carrying.  Nobody types a novel into a switcher,
    /// and a bound keeps the plate from growing past the screen.
    static constexpr size_t kMaxLength = 64;

    void Reset();

    bool                Empty() const { return m_query.empty(); }
    const std::wstring& Query() const { return m_query; }

    /// Edits.  Each returns true when the query actually changed, which is the
    /// caller's cue to re-run the filter — no change, no work.
    bool Append(wchar_t c);
    bool Backspace();
    bool Clear();

    /// `exeLower` is the owning executable's file name, already lowercased
    /// (the controller resolves it once per window at activation).
    bool Matches(const std::wstring& title, const std::wstring& exeLower) const;

private:
    void RebuildTokens();

    std::wstring              m_query;
    std::vector<std::wstring> m_tokens;   // lowercased, rebuilt on every edit
};
