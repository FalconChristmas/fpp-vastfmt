// separatorGap(), stationTextAsLine() and fitTitleToRoom(), the RDS Text template helpers.
// Needs nothing from FPP:
//   make -C tests rds_text_template_test && ./tests/rds_text_template_test
#include "../src/RDSTextTemplate.h"

#include <cstdio>
#include <string>

static int failures = 0;
static void check(const std::string &got, const std::string &want, const char *what) {
    bool ok = got == want;
    std::printf("%s  %s%s%s\n", ok ? "PASS" : "FAIL", what, ok ? "" : "  got: ", ok ? "" : got.c_str());
    if (!ok) failures++;
}
static void checkFits(const std::string &got, size_t room, const char *what) {
    bool ok = got.size() <= room;
    std::printf("%s  %s (%zu <= %zu)\n", ok ? "PASS" : "FAIL", what, got.size(), room);
    if (!ok) failures++;
}
int main() {
    // The separator between {Artist} and {Title} goes when only one of them has a value.
    auto gap = [](const std::string &tmpl, bool ae, bool te) {
        size_t f = 0, t = 0;
        if (!separatorGap(tmpl, ae, te, f, t)) return tmpl;
        return tmpl.substr(0, f) + tmpl.substr(t);
    };
    check(gap("[{Artist} - {Title}]", true, false), "[{Artist}{Title}]", "empty artist: ' - ' dropped");
    check(gap("[{Artist} - {Title}]", false, true), "[{Artist}{Title}]", "empty title: ' - ' dropped");
    check(gap("{Title} by {Artist}", true, false), "{Title}{Artist}", "title first: ' by ' dropped");
    check(gap("[{Artist} - {Title}]", false, false), "[{Artist} - {Title}]", "both set: kept");
    check(gap("[{Artist} - {Title}]", true, true), "[{Artist} - {Title}]", "both empty: left to the [ ] rule");
    check(gap("Now: {Title}", true, false), "Now: {Title}", "one placeholder: kept");
    check(gap("{Artist} [on] {Title}", true, false), "{Artist} [on] {Title}", "bracket between: kept");

    // An empty RDS Text falls back to the Station Text as one line.
    check(stationTextAsLine("Happy   HolidaysFrom OurStreet"), "Happy Holidays From Our Street",
          "8-character PS blocks: trimmed and joined with one space");
    check(stationTextAsLine("Merry   Christ- mas"), "Merry Christmas",
          "the shipped default: a block ending in '-' joins the next without it");
    check(stationTextAsLine("Merry Christmas Everyone"), "Merry Christmas Everyone",
          "plain line: not split mid-word");
    check(stationTextAsLine("Light   Show    88.1 FM"), "Light Show 88.1 FM", "PS blocks with a blank-padded tail");
    check(stationTextAsLine("WXYZ  FM  Rocks "), "WXYZ FM Rocks", "plain line: runs of spaces collapsed, ends trimmed");
    check(stationTextAsLine("Lights  "), "Lights", "one block: trimmed");
    check(stationTextAsLine("        "), "", "blank: empty");
    check(stationTextAsLine(""), "", "empty: empty");

    // A long title is shortened before it goes into the line.
    check(fitTitleToRoom("Day-O (The Banana Boat Song) (Beetlejuice/Soundtrack Version)", 46),
          "Day-O (The Banana Boat Song)", "trailing group dropped, the rest fits");
    check(fitTitleToRoom("Grim Grinning Ghosts (From \"The Haunted Mansion\") [Instrumental]", 40),
          "Grim Grinning Ghosts", "two trailing groups dropped, last first");
    check(fitTitleToRoom("Silent Night (Live)", 64), "Silent Night (Live)", "fits already, left alone");
    check(fitTitleToRoom("One Way or Another (Hocus Pocus 2 Version)", 30),
          "One Way or Another", "a group is dropped only when needed");
    check(fitTitleToRoom("Halloween Party - Scary Background Sounds: Castle Thunder, Howling Wind", 33),
          "Halloween Party - Scary", "no group: cut at a word");
    check(fitTitleToRoom("(Untitled)", 5), "(Unti", "a title that is only a group is cut, not emptied");
    check(fitTitleToRoom("Song (Remix) [Edit] (Live)", 12), "Song (Remix)", "drops groups until it fits, no further");
    check(fitTitleToRoom("Supercalifragilisticexpialidocious", 10), "Supercalif", "no space fits: cut at the limit");
    check(fitTitleToRoom("Unbalanced (paren title", 12), "Unbalanced", "an unclosed group is not a group");
    check(fitTitleToRoom("Song", 0), "", "no room left: empty title");
    check(fitTitleToRoom("Rock & Roll - Live", 13), "Rock & Roll", "trailing ' -' trimmed after the cut");
    checkFits(fitTitleToRoom("A very long title with many words that will not fit anywhere at all", 20), 20,
              "result never exceeds the room");

    std::printf("%s\n", failures ? "FAILED" : "all passed");
    return failures ? 1 : 0;
}
