#pragma once

// Helpers for filling the RDS Text template. Works on RDS-charset bytes (one byte per on-air
// character), as formatAndSendText() does.
// No function-local statics (see RDSCharset.h): they would stop the library from unloading.

#include <cstddef>
#include <string>

// When only one of {Artist} and {Title} has a value, the literal text between the two
// placeholders (" - ", " by ") has nothing to separate, so it is left out: an empty artist
// gives "Title", not " - Title". Returns false, and leaves from/to alone, when both or neither
// are empty, when either placeholder is missing, or when the text between them holds a
// bracket or another placeholder.
inline bool separatorGap(const std::string &tmpl, bool artistEmpty, bool titleEmpty,
                         size_t &from, size_t &to) {
    if (artistEmpty == titleEmpty) {
        return false;
    }
    const std::string A = "{Artist}", T = "{Title}";
    size_t a = tmpl.find(A), t = tmpl.find(T);
    if (a == std::string::npos || t == std::string::npos) {
        return false;
    }
    size_t f = a < t ? a + A.size() : t + T.size();
    size_t e = a < t ? t : a;
    if (f > e || tmpl.substr(f, e - f).find_first_of("[]{}") != std::string::npos) {
        return false;
    }
    from = f;
    to = e;
    return true;
}

// The Station Text (PS) as one RadioText line, for when the RDS Text comes out empty. PS goes
// out in 8-character blocks, and Station Text is often typed to suit that: "Happy   HolidaysFrom
// OurStreet" shows as "Happy", "Holidays", "From Our", "Street". A block other than the last
// that ends in two or more spaces is that kind of padding, and then each block is trimmed and
// the blocks are joined with one space: "Happy Holidays From Our Street". A block ending in "-"
// is a word split across blocks and joins the next without the hyphen: "Merry   Christ- mas"
// gives "Merry Christmas". Otherwise the text is a plain line ("Merry Christmas Everyone") and
// is not split, only its runs of spaces collapsed.
// The padding test is a heuristic: a plain line with two spaces ending exactly on a block
// boundary ("Joy To  The World") is taken as blocks and comes out "Joy To The Worl d".
inline std::string stationTextAsLine(const std::string &ps) {
    bool blocks = false;
    for (size_t b = 8; b < ps.size(); b += 8) {
        if (ps[b - 1] == ' ' && ps[b - 2] == ' ') {
            blocks = true;
            break;
        }
    }
    std::string spaced;
    for (size_t b = 0; b < ps.size(); b += blocks ? 8 : ps.size()) {
        std::string block = ps.substr(b, blocks ? 8 : std::string::npos);
        size_t end = block.find_last_not_of(' ');
        block = end == std::string::npos ? "" : block.substr(0, end + 1);
        if (blocks && !block.empty() && block.back() == '-' && b + 8 < ps.size()) {
            block.pop_back();
            spaced += block;
        } else {
            spaced += block + ' ';
        }
    }
    std::string line;
    for (char c : spaced) {
        if (c == ' ' && (line.empty() || line.back() == ' ')) {
            continue;
        }
        line += c;
    }
    if (!line.empty() && line.back() == ' ') {
        line.pop_back();
    }
    return line;
}

// RadioText holds at most 64 characters, and Si4713::setRDSBuffer() cuts a longer line at the
// last space that fits. With a long title that can still cut where the line reads badly:
// "Harry Belafonte - Day-O (The Banana Boat Song) (Beetlejuice/Soundtrack Version)" would end
// "...(Beetlejuice/Soundtrack". fitTitleToRoom() shortens the title before the line is built:
//   1. drop whole trailing "(...)" or "[...]" groups, last first, while the title is too long and
//      something is left: "Grim Grinning Ghosts (Live) [Instrumental]" becomes
//      "Grim Grinning Ghosts";
//   2. if it is still too long, cut at the last space that fits and drop a dangling " -";
//   3. if no space fits, cut at the limit.
// With no room at all the title comes back empty; the " - " before it is then dropped by
// setRDSBuffer()'s cut if the line is still over 64 characters.

inline void trimTrailingSeparators(std::string &s) {
    while (!s.empty() && (s.back() == ' ' || s.back() == '-')) {
        s.pop_back();
    }
}

// Removes one balanced "(...)" or "[...]" group from the end of s, with the separators before it.
// Leaves s alone, and returns false, when it does not end in a closed group or when nothing
// would be left.
inline bool dropTrailingGroup(std::string &s) {
    size_t end = s.find_last_not_of(' ');
    if (end == std::string::npos) {
        return false;
    }
    char close = s[end];
    char open;
    if (close == ')') {
        open = '(';
    } else if (close == ']') {
        open = '[';
    } else {
        return false;
    }
    int depth = 0;
    for (size_t i = end + 1; i-- > 0;) {
        if (s[i] == close) {
            depth++;
        } else if (s[i] == open && --depth == 0) {
            std::string rest = s.substr(0, i);
            trimTrailingSeparators(rest);
            if (rest.empty()) {
                return false;
            }
            s = rest;
            return true;
        }
    }
    return false;
}

inline std::string fitTitleToRoom(const std::string &title, size_t room) {
    if (title.size() <= room) {
        return title;
    }
    std::string t = title;
    while (t.size() > room && dropTrailingGroup(t)) {
    }
    if (t.size() <= room) {
        return t;
    }
    size_t sp = t.rfind(' ', room);
    std::string out = (sp != std::string::npos && sp > 0) ? t.substr(0, sp) : t.substr(0, room);
    trimTrailingSeparators(out);
    return out.empty() ? t.substr(0, room) : out;
}
