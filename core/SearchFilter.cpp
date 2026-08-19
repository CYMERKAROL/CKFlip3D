// ---------------------------------------------------------------------------
// Editing the query and testing a window against it.  Tokens are rebuilt on
// every edit rather than matched on the fly, since a query is at most a few
// dozen characters and the test runs once per window per keystroke.
//
// Copyright © 2026 Karol Cymerman (CYMERKAROL) — https://github.com/CYMERKAROL/CKFlip3D
// ---------------------------------------------------------------------------
#include "SearchFilter.h"
#include <algorithm>
#include <cwctype>

namespace {

std::wstring ToLower(std::wstring s)
{
    std::transform(s.begin(), s.end(), s.begin(), ::towlower);
    return s;
}

} // namespace

void SearchFilter::Reset()
{
    m_query.clear();
    m_tokens.clear();
}

bool SearchFilter::Append(wchar_t c)
{
    // Control characters never reach the query — the hook only forwards what
    // the keyboard layout translated to a printable glyph, and this is the
    // second gate so a layout quirk can never inject a newline into a title.
    if (c < 0x20 || c == 0x7F)
        return false;
    if (m_query.size() >= kMaxLength)
        return false;
    // A leading space would make the first token empty and match everything;
    // interior spaces are the token separator and are kept.
    if (c == L' ' && (m_query.empty() || m_query.back() == L' '))
        return false;

    m_query.push_back(c);
    RebuildTokens();
    return true;
}

bool SearchFilter::Backspace()
{
    if (m_query.empty())
        return false;
    m_query.pop_back();
    RebuildTokens();
    return true;
}

bool SearchFilter::Clear()
{
    if (m_query.empty())
        return false;
    m_query.clear();
    m_tokens.clear();
    return true;
}

void SearchFilter::RebuildTokens()
{
    m_tokens.clear();
    const std::wstring lower = ToLower(m_query);
    size_t start = 0;
    while (start < lower.size()) {
        size_t end = lower.find(L' ', start);
        if (end == std::wstring::npos) end = lower.size();
        if (end > start)
            m_tokens.push_back(lower.substr(start, end - start));
        start = end + 1;
    }
}

bool SearchFilter::Matches(const std::wstring& title,
                           const std::wstring& exeLower) const
{
    if (m_tokens.empty())
        return true;   // no query = everything stays

    std::wstring haystack = ToLower(title);
    if (!exeLower.empty()) {
        haystack += L' ';
        haystack += exeLower;
    }

    for (const std::wstring& tok : m_tokens) {
        if (haystack.find(tok) == std::wstring::npos)
            return false;
    }
    return true;
}
