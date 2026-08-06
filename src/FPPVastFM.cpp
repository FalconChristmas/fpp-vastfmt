#include <fpp-pch.h>

#include <string>
#include <vector>

#include <unistd.h>
#include <termios.h>

#include "mediadetails.h"
#include "commands/Commands.h"
#include "common.h"
#include "settings.h"
#include "Plugin.h"
#include "log.h"

#include "VASTFMT.h"
#include "I2CSi4713.h"
#include "util/GPIOUtils.h"

#if defined(PLATFORM_BBB) || defined(PLATFORM_BB64)
#include "util/BBBUtils.h"
constexpr int DEFAULT_GPIO = 3;
#else 
constexpr int DEFAULT_GPIO = 0;
#endif

static std::string padToNearest(std::string s, int l) {
    if (!s.empty()) {
        int n = 0;
        while (n < s.size()) {
            n += l;
        }
        if (n != s.size()) {
            size_t a = n - s.size();
            s.append(a, ' ');
        }
    }
    return s;
}
static void padTo(std::string &s, int l) {
    size_t n = l - s.size();
    if (n) {
        s.append(n, ' ');
    }
}

class FPPVastFMPlugin : public FPPPlugins::Plugin, public FPPPlugins::PlaylistEventPlugin {
public:
    bool enabled = true;
    bool rdsEnabled = false;
    FPPVastFMPlugin() : FPPPlugins::Plugin("fpp-vastfmt"), FPPPlugins::PlaylistEventPlugin() {
        setDefaultSettings();
        if (settings["Start"] == "FPPDStart") {
            startVast();
        } else if (settings["Start"] == "RDSOnly") {
            startVastForRDS();
        }
    }
    // Close the transmitter here rather than in the destructor. For the USB
    // part that also stops hidapi's per-device read thread (hid_close() joins
    // it), and doing it while the plugin is still a whole object means nothing
    // is mid-call into it. Closing on unload rather than at destruction also
    // releases the I2C bus or the USB device promptly when the plugin is
    // uninstalled. Everything here is synchronous, so no readiness predicate.
    virtual std::function<bool()> shutdown() override {
        closeDevice();
        return nullptr;
    }

    virtual ~FPPVastFMPlugin() {
        closeDevice(); // no-op if shutdown() already ran
    }

    // Idempotent, so shutdown() and the destructor can both call it.
    void closeDevice() {
        if (si4713 != nullptr) {
            //si4713->powerDown();
            delete si4713;
            si4713 = nullptr;
        }
    }

    bool initVast() {
        if (si4713 != nullptr) {
            delete si4713;
            si4713 = nullptr;
        }

        if (settings["Connection"] == "I2C") {
            std::string pin = settings["ResetPin"];
            if (pin.empty()) {
#ifdef PLATFORM_PI
                pin = "P1-07";
#elif defined(PLATFORM_BBB) || defined(PLATFORM_BB64)
                if (getBeagleBoneType() == BeagleBoneType::PocketBeagle) {
                    pin = "P1_04";
                } else {
                    pin = "P9-22";
                }
                pin = "P1_04";
#endif
            }
            if (pin[0] >= '0' && pin[0] <= '9') {
                auto a = PinCapabilities::getPinByGPIO(DEFAULT_GPIO, std::atoi(pin.c_str())).ptr();
                if (a) {
                    pin = a->name;
                }
            }
            si4713 = new I2CSi4713(pin);
        } else {
            si4713 = new VASTFMT();
        }
        if (si4713->isOk()) {
            si4713->enableAudioCompression(settings["AudioCompression"] == "True");
            si4713->enableAudioLimitter(settings["AudioLimitter"] == "True");
            si4713->setAudioGain(std::stoi(settings["AudioGain"]));
            si4713->setAudioCompressionThreshold(std::stoi(settings["AudioCompressionThreshold"]));
            if (settings["Preemphasis"] == "50us") {
                si4713->setEUPreemphasis();
            }
            
            si4713->Init();

            std::string rev = si4713->getRev();
            LogInfo(VB_PLUGIN, "VAST-FMT: %s\n", rev.c_str());

            return true;
        }

        LogErr(VB_PLUGIN, "VAST-FMT: Unable to initialize si4713\n");

        delete si4713;
        si4713 = nullptr;

        return false;
    }

    void initRDS() {
        LogInfo(VB_PLUGIN, "Enabling RDS\n");
        si4713->beginRDS();
        formatAndSendText(settings["StationText"], "", "", true);
        formatAndSendText(settings["RDSTextText"], "", "", false);
    }
    
    void startVast() {
        if (si4713 == nullptr) {
            if (initVast()) {
                std::string freq = settings["Frequency"];
                float f = std::stof(freq);
                f *= 100;
                si4713->setFrequency(f);

                f = std::stoi(settings["AntCap"]);
                si4713->setTXPower(std::stoi(settings["Power"]), f);
                
                si4713->setPTY(std::stoi(settings["Pty"]));
                
                std::string asq = si4713->getASQ();
                LogInfo(VB_PLUGIN, "VAST-FMT: %s\n", asq.c_str());
                
                std::string ts = si4713->getTuneStatus();
                LogInfo(VB_PLUGIN, "VAST-FMT: %s\n", ts.c_str());

                rdsEnabled = settings["EnableRDS"] == "True";
                if (rdsEnabled) {
                    initRDS();
                }
            }
        }
    }
    void startVastForRDS() {
        if (si4713 == nullptr) {
            if (initVast()) {
                rdsEnabled = settings["EnableRDS"] == "True";
                if (rdsEnabled) {
                    initRDS();
                } else {
                    LogErr(VB_PLUGIN, "Tried to setup for RDS, but RDS is not enabled\n");
                }
            }
        }
    }
    void stopVast() {
        if (si4713 != nullptr) {
            //si4713->powerDown();
            delete si4713;
            si4713 = nullptr;
        }
    }
    
    void formatAndSendText(const std::string &text, const std::string &artist, const std::string &title, bool station) {
        std::string output;
        
        int artistIdx = -1;
        int titleIdx = -1;

        if (!si4713)
            return;

        for (int x = 0; x < text.length(); x++) {
            if (text[x] == '[') {
                if (artist == "" && title == "") {
                    while (text[x] != ']' && x < text.length()) {
                        x++;
                    }
                }
            } else if (text[x] == ']') {
                //nothing
            } else if (text[x] == '{') {
                const static std::string ARTIST = "{Artist}";
                const static std::string TITLE = "{Title}";
                std::string subs = text.substr(x);
                if (subs.rfind(ARTIST) == 0) {
                    artistIdx = output.length();
                    x += ARTIST.length() - 1;
                    output += artist;
                } else if (subs.rfind(TITLE) == 0) {
                    titleIdx = output.length();
                    x += TITLE.length() - 1;
                    output += title;
                } else {
                    output += text[x];
                }
            } else {
                output += text[x];
            }
        }
        if (station) {
            LogDebug(VB_PLUGIN, "Setting RDS Station text to \"%s\"\n", output.c_str());
            std::vector<std::string> fragments;
            while (output.size()) {
                if (output.size() <= 8) {
                    padTo(output, 8);
                    fragments.push_back(output);
                    output.clear();
                } else {
                    std::string lft = output.substr(0, 8);
                    padTo(lft, 8);
                    output = output.substr(8);
                    fragments.push_back(lft);
                }
            }
            if (fragments.empty()) {
                std::string m = "        ";
                fragments.push_back(m);
            }
            si4713->setRDSStation(fragments);
        } else {
            LogDebug(VB_PLUGIN, "Setting RDS text to \"%s\"\n", output.c_str());
            si4713->setRDSBuffer(output, artistIdx, artist.length(), titleIdx, title.length());
        }
    }
    

    virtual void playlistCallback(const Json::Value &playlist, const std::string &action, const std::string &section, int item) {
        if (action == "stop" && rdsEnabled) {
            formatAndSendText(settings["StationText"], "", "", true);
            formatAndSendText(settings["RDSTextText"], "", "", false);
        }
        if (settings["Start"] == "PlaylistStart" && action == "start") {
            startVast();
        } else if (settings["Stop"] == "PlaylistStop" && action == "stop") {
            stopVast();
        }
        
    }
    virtual void mediaCallback(const Json::Value &playlist, const MediaDetails &mediaDetails) {
        if (!rdsEnabled) {
            return;
        }
        std::string title = mediaDetails.title;
        std::string artist = mediaDetails.artist;
        std::string album = mediaDetails.album;
        int track = mediaDetails.track;
        int length = mediaDetails.length;

        // Bump the volume down and back up to work around Vast-FMT 212R issue
        if (settings["EnableVolumeChangeHack"] == "1") {
            Json::Value cmd;
            Json::Value args(Json::arrayValue);

            args.append("5");
            cmd["args"] = args;

            cmd["command"] = "Volume Decrease";
            CommandManager::INSTANCE.run(cmd);

            cmd["command"] = "Volume Increase";
            CommandManager::INSTANCE.run(cmd);
        }
        
        std::string type = playlist["currentEntry"]["type"].asString();
        if (type != "both" && type != "media") {
            title = "";
            artist = "";
        }
        
        formatAndSendText(settings["StationText"], artist, title, true);
        formatAndSendText(settings["RDSTextText"], artist, title, false);
    }
    
    
    void setDefaultSettings() {
        setIfNotFound("Start", "FPPDStart");
        setIfNotFound("Frequency", "100.10");
        setIfNotFound("Power", "110");
        setIfNotFound("Preemphasis", "75us");
        setIfNotFound("AntCap", "0");
        setIfNotFound("EnableRDS", "False");
        setIfNotFound("StationText", "Merry   Christ- mas", true);
        setIfNotFound("RDSTextText", "[{Artist} - {Title}]", true);
        setIfNotFound("Pty", "2");
        
        setIfNotFound("Connection", "USB");
        setIfNotFound("EnableVolumeChangeHack", "0");
#ifdef PLATFORM_BBB
        setIfNotFound("ResetPin", "14");
#else
        setIfNotFound("ResetPin", "4");
#endif
        setIfNotFound("AudioCompression", "True");
        setIfNotFound("AudioLimitter", "True");
        setIfNotFound("AudioGain", "5");
        setIfNotFound("AudioCompressionThreshold", "-15");
    }
    void setIfNotFound(const std::string &s, const std::string &v, bool emptyAllowed = false) {
        if (settings.find(s) == settings.end()) {
            settings[s] = v;
        } else if (!emptyAllowed && settings[s] == "") {
            settings[s] = v;
        }
        LogDebug(VB_PLUGIN, "Setting \"%s\": \"%s\"\n", s.c_str(), settings[s].c_str());
    }
    
    Si4713 *si4713 = nullptr;
};


// Safe to dlclose() on unload: this plugin starts no threads, registers no
// timers, issues no CurlManager requests, holds no epoll descriptors, adds no
// commands and serves no HTTP routes. shutdown() closes the transmitter -
// hid_close() for the USB part, which also releases the device rather than
// holding it until fppd restarts.
//
// The USB HID path is why this used to be withheld. hidapi's LIBUSB backend was
// compiled into this library (src/hid.c) and runs a read thread per open device,
// so that thread's entry point sat inside the .so that dlclose() unmaps. It now
// links the system -lhidapi-hidraw instead, as fpp-kfmt already did: the hidraw
// backend is a thin wrapper over read/write/ioctl on /dev/hidraw*, starts no
// threads, and lives in libhidapi-hidraw.so, which is never unloaded. This .so
// therefore defines no hid_* symbol and makes no pthread_create call of its own
// - check with:  nm -D libfpp-vastfmt.so | grep -E ' hid_|pthread_create'
//
// The I2C path never had any of this.
FPP_PLUGIN_SUPPORTS_UNLOAD()

extern "C" {
    FPPPlugins::Plugin *createPlugin() {
        return new FPPVastFMPlugin();
    }
}
