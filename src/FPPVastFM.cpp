#include <fpp-pch.h>

#include <string>
#include <vector>
#include <mutex>

#include <unistd.h>
#include <termios.h>

#include "mediadetails.h"
#include "commands/Commands.h"
#include "common.h"
#include "settings.h"
#include "Plugin.h"
#include "log.h"
#include "fpphttp.h"

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

class FPPVastFMPlugin : public FPPPlugins::Plugin,
                        public FPPPlugins::PlaylistEventPlugin,
                        public FPPPlugins::APIProviderPlugin {
public:
    bool enabled = true;
    bool rdsEnabled = false;

    // Everything that touches the transmitter takes this. Until the status API
    // existed the device was only ever reached from fppd's own callbacks, one
    // at a time; an HTTP handler runs on a drogon thread and would otherwise
    // be talking to the same USB or I2C device midway through a playlist
    // callback doing the same.
    std::mutex deviceLock;
    // The "true" asks FPP to watch config/plugin.fpp-vastfmt and call
    // settingChanged() below, so retuning the transmitter no longer needs an
    // fppd restart.
    FPPVastFMPlugin() : FPPPlugins::Plugin("fpp-vastfmt", true), FPPPlugins::PlaylistEventPlugin() {
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
        unregisterApis();
        closeDevice();
        return nullptr;
    }

    virtual ~FPPVastFMPlugin() {
        closeDevice(); // no-op if shutdown() already ran
    }

    // Idempotent, so shutdown() and the destructor can both call it.
    void closeDevice() {
        std::lock_guard<std::mutex> lk(deviceLock);
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
                // Pin names on these boards use a hyphen: "P1-04", not
                // "P1_04". The underscore spelling matches nothing, so
                // getPinByName() handed back a null pin. An unconditional
                // assignment here also made both branches dead code.
                if (getBeagleBoneType() == BeagleBoneType::PocketBeagle) {
                    pin = "P1-04";
                } else {
                    pin = "P9-22";
                }
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
            // All user input, and these now run whenever a setting changes
            // rather than only at startup, so a throw would land on the main
            // loop inside FPP's file-monitor callback and take fppd down.
            si4713->setAudioGain(safeStoi(settings["AudioGain"], 0, "AudioGain"));
            si4713->setAudioCompressionThreshold(safeStoi(settings["AudioCompressionThreshold"], -15, "AudioCompressionThreshold"));
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
                float f = safeStof(settings["Frequency"], 100.1f, "Frequency");
                f *= 100;
                si4713->setFrequency(f);

                f = safeStoi(settings["AntCap"], 0, "AntCap");
                si4713->setTXPower(safeStoi(settings["Power"], 110, "Power"), f);
                
                si4713->setPTY(safeStoi(settings["Pty"], 0, "Pty"));
                
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
    // Re-open the transmitter with the current settings. Everything that
    // shapes the RF or the device connection is applied while the chip is being
    // set up, so a change to any of it means going round again - which is also
    // what retuning physically requires.
    void applyConfiguration() {
        bool wasRunning = (si4713 != nullptr);
        stopVast();
        if (settings["Start"] == "FPPDStart") {
            startVast();
        } else if (settings["Start"] == "RDSOnly") {
            startVastForRDS();
        } else if (wasRunning) {
            // Started by a playlist rather than at boot; keep it on the air.
            startVast();
        }
    }

    // Called by FPP when config/plugin.fpp-vastfmt changes; the base class has
    // already updated settings[key].
    virtual void settingChanged(const std::string &key, const std::string &value) override {
        std::lock_guard<std::mutex> lk(deviceLock);
        if (key == "Start" || key == "Stop" || key == "EnableVolumeChangeHack") {
            // Read at point of use in the playlist callbacks - nothing to do.
            return;
        }
        if (key == "StationText" || key == "RDSTextText") {
            // Just push the new text; no reason to drop the carrier for it.
            if (si4713 != nullptr && rdsEnabled) {
                formatAndSendText(settings["StationText"], "", "", true);
                formatAndSendText(settings["RDSTextText"], "", "", false);
            }
            return;
        }
        LogInfo(VB_PLUGIN, "VAST-FMT: %s changed, reconfiguring\n", key.c_str());
        applyConfiguration();
    }

    static int safeStoi(const std::string &s, int defVal, const char *name) {
        try {
            if (!s.empty()) {
                return std::stoi(s);
            }
        } catch (const std::exception &e) {
            LogErr(VB_PLUGIN, "VAST-FMT: bad value for %s (\"%s\"): %s - using %d\n",
                   name, s.c_str(), e.what(), defVal);
        }
        return defVal;
    }
    static float safeStof(const std::string &s, float defVal, const char *name) {
        try {
            if (!s.empty()) {
                return std::stof(s);
            }
        } catch (const std::exception &e) {
            LogErr(VB_PLUGIN, "VAST-FMT: bad value for %s (\"%s\"): %s - using %0.2f\n",
                   name, s.c_str(), e.what(), defVal);
        }
        return defVal;
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
        std::lock_guard<std::mutex> lk(deviceLock);
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
        std::lock_guard<std::mutex> lk(deviceLock);
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
    
    

    // ---------------------------------------------------------------------
    // Status API
    // ---------------------------------------------------------------------

    virtual void registerApis() override {
        auto handler = [this](const HttpRequestPtr &req, HttpCallback &&cb) {
            handleApi(req, std::move(cb));
        };
        FPPPlugins::registerPluginApi("/vastfmt", handler, {drogon::Get}, false);
        FPPPlugins::registerPluginApi("/vastfmt/retune", handler, {drogon::Get, drogon::Post}, false);
    }
    virtual void unregisterApis() override {
        FPPPlugins::unregisterPluginApi("/vastfmt");
        FPPPlugins::unregisterPluginApi("/vastfmt/retune");
    }

    // Caller holds deviceLock.
    Json::Value statusJsonLocked() {
        Json::Value root;
        root["connection"] = settings["Connection"];
        root["running"] = (si4713 != nullptr);
        root["rdsEnabled"] = rdsEnabled;
        int configured = safeStoi(settings["AntCap"], 0, "AntCap");
        root["antCapSetting"] = configured;
        root["antCapAuto"] = (configured == 0);
        if (si4713 == nullptr) {
            root["state"] = "not running";
            return root;
        }
        int f = 0, p = 0, c = 0;
        if (!si4713->readTuneStatus(f, p, c)) {
            root["state"] = "no response";
            return root;
        }
        root["state"] = "ok";
        root["frequency"] = f / 100.0;
        root["power"] = p;
        root["antCap"] = c;
        root["antCapPf"] = c * 0.25;
        root["matchOk"] = Si4713::antennaMatchOk(c);
        root["asq"] = si4713->getASQ();
        return root;
    }

    // Run a one-off automatic antenna search and report what it picked,
    // without committing it. Someone who has set the capacitor by hand has
    // turned the automatic search off, so this is the only way for them to
    // find out what it would choose - which is the whole point of the button.
    // The configured value is put back afterwards so a show is not left on a
    // different setting than the page shows.
    Json::Value retuneLocked() {
        Json::Value root;
        if (si4713 == nullptr) {
            root["ok"] = false;
            root["error"] = "transmitter is not running";
            return root;
        }
        int power = Si4713::clampPower(safeStoi(settings["Power"], 110, "Power"));
        int configured = safeStoi(settings["AntCap"], 0, "AntCap");

        si4713->setTXPower(power, 0);          // 0 = search
        int found = si4713->lastAntCapRaw();

        if (configured != 0) {
            si4713->setTXPower(power, configured);   // put their setting back
        }

        root["ok"] = true;
        root["antCap"] = found;
        root["antCapPf"] = found * 0.25;
        root["matchOk"] = Si4713::antennaMatchOk(found);
        root["restored"] = configured;
        root["message"] = Si4713::antennaMatchOk(found)
            ? "Automatic tuning found a match."
            : "Automatic tuning found no match - the antenna is probably not "
              "resonant near this frequency. Set the capacitor by hand.";
        Json::Value st = statusJsonLocked();
        for (const auto &k : st.getMemberNames()) {
            root[k] = st[k];
        }
        return root;
    }

    void handleApi(const HttpRequestPtr &req, HttpCallback &&callback) {
        const std::string path = req->path();
        Json::Value root;
        {
            std::lock_guard<std::mutex> lk(deviceLock);
            if (path.find("retune") != std::string::npos) {
                root = retuneLocked();
            } else {
                root = statusJsonLocked();
            }
        }
        callback(makeStringResponse(root.toStyledString(), 200, "application/json"));
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
// timers, issues no CurlManager requests, holds no epoll descriptors and adds
// no commands. It does serve HTTP routes, and shutdown() gives them back with
// unregisterPluginApi() before anything else - that call does not return until
// no request is executing inside the handler and the handler object itself has
// been destroyed, which is what makes unmapping this library safe. It runs
// before the device is closed so a handler already waiting on deviceLock
// cannot be left holding a freed transmitter. shutdown() closes the
// transmitter -
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
