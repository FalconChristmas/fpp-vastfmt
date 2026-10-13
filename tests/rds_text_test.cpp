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

int main() {
    const int TITLE = 1, ARTIST = 4;

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

    std::printf("%s\n", failures ? "FAILED" : "all passed");
    return failures ? 1 : 0;
}
