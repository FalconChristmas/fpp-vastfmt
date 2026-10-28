// Drives the real Si4713 RDS code with a fake chip that records every command, then decodes the
// RadioText and RT+ groups the way a receiver does (code table E.1 from redsea, an independent
// RDS decoder). Text goes in the way formatAndSendText() sends it: converted with utf8ToRDS(),
// positions counted in that converted text.
//
// Needs an FPP tree (Si4713.cpp logs through libfpp):  make -C tests SRCDIR=/opt/fpp/src && ./tests/rds_text_test
#include "../src/RDSCharset.h"
#include "../src/Si4713.h"

#include <atomic>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

static const char *kG0[256] = {
    " ", " ", " ", " ", " ", " ", " ", " ", " ", " ", "\n", " ", " ", "\r", " ", " ",
    " ", " ", " ", " ", " ", " ", " ", " ", " ", " ", " ", " ", " ", " ", " ", "\u00AD",
    " ", "!", "\"", "#", "¤", "%", "&", "'", "(", ")", "*", "+", ",", "-", ".", "/",
    "0", "1", "2", "3", "4", "5", "6", "7", "8", "9", ":", ";", "<", "=", ">", "?",
    "@", "A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L", "M", "N", "O",
    "P", "Q", "R", "S", "T", "U", "V", "W", "X", "Y", "Z", "[", "\\", "]", "―", "_",
    "‖", "a", "b", "c", "d", "e", "f", "g", "h", "i", "j", "k", "l", "m", "n", "o",
    "p", "q", "r", "s", "t", "u", "v", "w", "x", "y", "z", "{", "|", "}", "¯", " ",
    "á", "à", "é", "è", "í", "ì", "ó", "ò", "ú", "ù", "Ñ", "Ç", "Ş", "β", "¡", "Ĳ",
    "â", "ä", "ê", "ë", "î", "ï", "ô", "ö", "û", "ü", "ñ", "ç", "ş", "ǧ", "ı", "ĳ",
    "ª", "α", "©", "‰", "Ǧ", "ě", "ň", "ő", "π", "€", "£", "$", "←", "↑", "→", "↓",
    "º", "¹", "²", "³", "±", "İ", "ń", "ű", "µ", "¿", "÷", "°", "¼", "½", "¾", "§",
    "Á", "À", "É", "È", "Í", "Ì", "Ó", "Ò", "Ú", "Ù", "Ř", "Č", "Š", "Ž", "Ð", "Ŀ",
    "Â", "Ä", "Ê", "Ë", "Î", "Ï", "Ô", "Ö", "Û", "Ü", "ř", "č", "š", "ž", "đ", "ŀ",
    "Ã", "Å", "Æ", "Œ", "ŷ", "Ý", "Õ", "Ø", "Þ", "Ŋ", "Ŕ", "Ć", "Ś", "Ź", "Ŧ", "ð",
    "ã", "å", "æ", "œ", "ŵ", "ý", "õ", "ø", "þ", "ŋ", "ŕ", "ć", "ś", "ź", "ŧ", " ",
};

// libfpp expects this from fppd's own executable.
std::atomic_int runMainFPPDLoop{1};

class FakeChip : public Si4713 {
public:
    std::vector<std::vector<uint8_t>> rdsBuff;   // every TX_RDS_BUFF argument list
    bool isOk() override { return true; }
    std::string getRev() override { return ""; }
    void powerUp() override {}
    void powerDown() override {}
    void reset() override {}
    std::string getASQ() override { return ""; }
    std::string getTuneStatus() override { return ""; }
    bool readTuneStatus(int &, int &, int &) override { return true; }
private:
    bool sendSi4711Command(uint8_t cmd, const std::vector<uint8_t> &data, std::vector<uint8_t> &out, bool) override {
        if (cmd == 0x35) rdsBuff.push_back(data);
        out.assign(6, 0);
        return true;
    }
    bool setProperty(uint16_t, uint16_t) override { return true; }
};

static int failures = 0;
static void expect(bool ok, const std::string &what, const std::string &got) {
    std::printf("%s  %s%s%s\n", ok ? "PASS" : "FAIL", what.c_str(), ok ? "" : "  got: ", ok ? "" : got.c_str());
    if (!ok) failures++;
}
static std::string decode(const std::string &bytes) {
    std::string s;
    for (unsigned char c : bytes) s += kG0[c];
    return s;
}

struct OnAir {
    std::string rt;
    std::map<int, std::string> tags; // content type -> tagged text
};
static OnAir send(const std::string &rds, int artistPos, int artistLen, int titlePos, int titleLen) {
    FakeChip chip;
    chip.setRDSBuffer(rds, artistPos, artistLen, titlePos, titleLen);
    std::map<int, std::string> segs;
    OnAir a;
    std::vector<uint8_t> plus;
    for (auto &d : chip.rdsBuff) {
        if (d.size() >= 7 && d[1] == 0x20) segs[d[2]] = std::string{(char)d[3], (char)d[4], (char)d[5], (char)d[6]};
        if (d.size() >= 7 && d[1] == 0xB0) plus = d;
    }
    std::string bytes;
    for (auto &kv : segs) bytes += kv.second;
    size_t cr = bytes.find('\r');
    if (cr != std::string::npos) bytes.resize(cr);
    a.rt = decode(bytes);
    while (!a.rt.empty() && a.rt.back() == ' ') a.rt.pop_back();
    if (!plus.empty()) {
        uint8_t b = plus[2], c1 = plus[3], c2 = plus[4], d1 = plus[5], d2 = plus[6];
        int ct1 = ((b & 7) << 3) | (c1 >> 5), s1 = ((c1 & 0x1F) << 1) | (c2 >> 7), l1 = ((c2 >> 1) & 0x3F) + 1;
        int ct2 = ((c2 & 1) << 5) | (d1 >> 3), s2 = ((d1 & 7) << 3) | (d2 >> 5), l2 = (d2 & 0x1F) + 1;
        // A tag pointing past the text shows as "<past end>" rather than crashing the test.
        auto cut = [&](int st, int len) { return st < (int)bytes.size() ? decode(bytes.substr(st, len)) : std::string("<past end>"); };
        if (ct1) a.tags[ct1] = cut(s1, l1);
        if (ct2) a.tags[ct2] = cut(s2, l2);
    }
    return a;
}
static std::string tag(const OnAir &a, int ct) {
    auto it = a.tags.find(ct);
    return it == a.tags.end() ? "<none>" : it->second;
}

// The RadioText groups (2A) exactly as sent: block B's low byte carries the Text A/B flag in
// bit 4 and the segment address in bits 3-0, so only addresses 0-15 exist. An index of 16 is
// address 0 with the flag flipped, which a receiver takes as a new message and clears the
// text. The segments must run 0, 1, 2... with a carriage return inside the data when the text
// is shorter than 64 characters, and none at 64.
static void checkSegments(size_t n) {
    std::string text;
    for (size_t i = 0; i < n; i++) text += (char)('A' + i % 26);
    FakeChip chip;
    chip.setRDSBuffer(text, -1, 0, 0, (int)n);

    const std::string what = "RT of " + std::to_string(n) + " chars: ";
    size_t sl = std::min<size_t>(n, 64);
    size_t want = sl < 64 ? (sl + 4) / 4 : 16;
    std::vector<uint8_t> idx;
    std::string bytes;
    std::vector<uint8_t> plus;
    for (auto &d : chip.rdsBuff) {
        if (d.size() >= 7 && d[1] == 0x20) {
            idx.push_back(d[2]);
            bytes += std::string{(char)d[3], (char)d[4], (char)d[5], (char)d[6]};
        }
        if (d.size() >= 7 && d[1] == 0xB0) plus = d;
    }
    std::string got;
    for (uint8_t i : idx) got += std::to_string(i) + " ";
    bool inRange = true, sameFlag = true, inOrder = true;
    for (size_t i = 0; i < idx.size(); i++) {
        inRange &= (idx[i] & 0xEF) <= 15;
        sameFlag &= (idx[i] & 0x10) == (idx[0] & 0x10);
        inOrder &= (idx[i] & 0x0F) == i;
    }
    expect(inRange, what + "no segment index above 15", got);
    expect(sameFlag, what + "A/B flag never changes", got);
    expect(inOrder && idx.size() == want, what + std::to_string(want) + " segments, addressed 0 up", got);
    std::string shown = bytes.substr(0, std::min(sl, bytes.size()));
    expect(shown == text.substr(0, sl), what + "text arrives intact", shown);
    if (sl < 64) {
        expect(bytes.size() > sl && bytes[sl] == '\r', what + "carriage return right after the text",
               bytes.size() > sl ? std::to_string((int)(unsigned char)bytes[sl]) : "missing");
    } else {
        expect(bytes.find('\r') == std::string::npos, what + "no carriage return at 64", "found one");
    }
    if (plus.size() >= 7) {
        uint8_t c1 = plus[3], c2 = plus[4];
        size_t s1 = ((c1 & 0x1F) << 1) | (c2 >> 7), l1 = ((c2 >> 1) & 0x3F) + 1;
        expect(s1 + l1 <= sl && bytes.substr(s1, l1) == text.substr(0, sl),
               what + "RT+ tag covers the message and stays inside it",
               std::to_string(s1) + "+" + std::to_string(l1));
    } else {
        expect(false, what + "RT+ tag sent", "none");
    }
}

int main() {
    const int TITLE = 1, ARTIST = 4;

    for (size_t n : {1, 4, 57, 59, 60, 61, 63, 64, 70}) checkSegments(n);

    // "{Title} - {Artist}" filled the way formatAndSendText() does it.
    std::string title = utf8ToRDS("Feliz Año Nuevo"), artist = utf8ToRDS("José Pérez");
    OnAir a = send(title + " - " + artist, (int)title.size() + 3, (int)artist.size(), 0, (int)title.size());
    expect(a.rt == "Feliz Año Nuevo - José Pérez", "RadioText shows the accented artist", a.rt);
    expect(tag(a, TITLE) == "Feliz Año Nuevo", "RT+ title tag is exactly the title", tag(a, TITLE));
    expect(tag(a, ARTIST) == "José Pérez", "RT+ artist tag is exactly the artist", tag(a, ARTIST));

    // Template without {Title}: no title tag, and the artist tag still lands right.
    a = send(utf8ToRDS("Now playing: ") + artist, 13, (int)artist.size(), -1, (int)title.size());
    expect(tag(a, TITLE) == "<none>", "missing {Title} sends no title tag", tag(a, TITLE));
    expect(tag(a, ARTIST) == "José Pérez", "lone artist tag decodes correctly", tag(a, ARTIST));

    // Past the 64-character RadioText: the tag is clipped to what was sent.
    std::string longTitle = utf8ToRDS("A Very Long Song Title That Keeps Going Past The Limit Ok!");
    std::string band = utf8ToRDS("Oldham Brass");
    a = send(longTitle + " - " + band, (int)longTitle.size() + 3, (int)band.size(), 0, (int)longTitle.size());
    // The artist starts at position 61, so characters 61-63 are all that fit in 64.
    expect(tag(a, ARTIST) == "Old", "artist tag clipped at character 64", tag(a, ARTIST));

    // Switching from a tagged title to static text: the RT+ group must say the item ended
    // (running bit 0, toggle flipped), or an RT+ receiver keeps the last title and artist.
    // Block B's low byte carries the item toggle bit in bit 4 and the running bit in bit 3.
    {
        FakeChip chip;
        auto lastPlus = [&]() {
            std::vector<uint8_t> p;
            for (auto &d : chip.rdsBuff) if (d.size() >= 7 && d[1] == 0xB0) p = d;
            return p;
        };
        chip.setRDSBuffer("Graham Blvd - Rock Lobster", 0, 11, 14, 12);
        std::vector<uint8_t> song = lastPlus();
        chip.rdsBuff.clear();
        chip.setRDSBuffer("Light Show 88.1 FM", -1, 0, -1, 0);
        std::vector<uint8_t> text = lastPlus();
        bool both = song.size() >= 7 && text.size() >= 7;
        expect(both && (song[2] & 0x08), "song: RT+ item running", both ? "" : "no RT+ group");
        expect(both && (text[2] & 0x08), "static text after a song: a new RT+ item, running", both ? "not running" : "no RT+ group");
        expect(both && ((song[2] ^ text[2]) & 0x10), "static text after a song: RT+ toggle flipped", "same toggle");

        // An empty RadioText after a song (After Hours turned off with "[{Artist} - {Title}]").
        chip.setRDSBuffer("Graham Blvd - Rock Lobster", 0, 11, 14, 12);
        song = lastPlus();
        chip.rdsBuff.clear();
        chip.setRDSBuffer("", -1, 0, -1, 0);
        text = lastPlus();
        both = song.size() >= 7 && text.size() >= 7;
        expect(both && !(text[2] & 0x08) && ((song[2] ^ text[2]) & 0x10),
               "empty text after a song: RT+ item ended, toggle flipped", both ? "still running" : "no RT+ group");
    }

    // The RadioText A/B flag flips once per new text, and never within one.
    {
        FakeChip chip;
        auto flags = [&]() {
            std::vector<int> f;
            for (auto &d : chip.rdsBuff) if (d.size() >= 7 && d[1] == 0x20) f.push_back(d[2] & 0x10);
            return f;
        };
        chip.setRDSBuffer("Graham Blvd - Rock Lobster", 0, 11, 14, 12);
        std::vector<int> a = flags();
        chip.rdsBuff.clear();
        chip.setRDSBuffer("Light Show 88.1 FM", -1, 0, -1, 0);
        std::vector<int> b = flags();
        chip.rdsBuff.clear();
        chip.setRDSBuffer("Light Show 88.1 FM", -1, 0, -1, 0);
        bool ok = !a.empty() && !b.empty() && a.front() != b.front();
        for (int x : a) ok &= x == a.front();
        for (int x : b) ok &= x == b.front();
        expect(ok, "new text flips the RadioText A/B flag once", "unchanged or mixed");
        expect(chip.rdsBuff.empty(), "unchanged text is not resent", "resent");
    }

    // Static text is tagged as the item title, whole line, no artist.
    {
        OnAir s = send("Light Show 88.1 FM", -1, 0, -1, 0);
        expect(tag(s, TITLE) == "Light Show 88.1 FM", "static text: RT+ title is the whole line", tag(s, TITLE));
        expect(tag(s, ARTIST) == " ", "static text: RT+ artist is one blank", "\"" + tag(s, ARTIST) + "\"");
        s = send("Lights", -1, 0, -1, 0);
        expect(tag(s, TITLE) == "Lights" && tag(s, ARTIST) == "<none>", "static text with no space: title only", tag(s, ARTIST));
        std::string full(64, 'x');
        s = send(full, -1, 0, -1, 0);
        expect(tag(s, TITLE) == full, "static text of 64: RT+ title covers all 64", tag(s, TITLE));
    }

    std::printf("%s\n", failures ? "FAILED" : "all passed");
    return failures ? 1 : 0;
}
