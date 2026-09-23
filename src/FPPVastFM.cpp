#include <fpp-pch.h>

#include <string>
#include <vector>
#include <mutex>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <queue>
#include <thread>

#include <unistd.h>
#include <termios.h>

#include "mediadetails.h"
#include "commands/Commands.h"
#include "common.h"
#include "settings.h"
#include "Plugin.h"
#include "log.h"
#include "Player.h"
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

    // The transmitter is reached from one thread and one thread only. Callers
    // - playlist and media callbacks, settings changes, HTTP handlers - put
    // work on this queue instead of touching the device, so a slow I2C or USB
    // exchange never runs on fppd's main loop or a drogon thread, and two of
    // them can never overlap.
    std::atomic<bool> running{false};
    std::mutex lock;
    std::condition_variable condition;
    std::thread workerThread;
    std::queue<std::function<void()>> functions;

    // Everything a queued job touches on a waiter's behalf. Both threads own
    // it, so a wait that times out cannot leave the worker writing into the
    // caller's dead stack.
    struct RadioJob {
        std::mutex mutex;
        std::condition_variable cv;
        bool done = false;
        Json::Value result;
        bool ok = false;
    };

    bool      playlistActive = false;

    // After Hours Music Player streams over mpd while FPP is idle. Polling its
    // track title lets RDS follow the stream instead of sitting on the static
    // station text between playlists.
    bool      mpcAvailable = false;
    uint64_t  nextMpcPoll  = 0;
    std::string mpcTitle;
    std::string mpcArtist;

    // Temporary overrides driven by FPP commands. Non-empty wins over the
    // configured text; an empty string is how a command restores the
    // configuration, so these are deliberately not "is set" flags.
    std::string stationTextOverride;
    std::string rdsTextOverride;
    std::vector<Command*> myCommands;
    // The "true" asks FPP to watch config/plugin.fpp-vastfmt and call
    // settingChanged() below, so retuning the transmitter no longer needs an
    // fppd restart.
    FPPVastFMPlugin() : FPPPlugins::Plugin("fpp-vastfmt", true), FPPPlugins::PlaylistEventPlugin() {
        setDefaultSettings();
        mpcAvailable = FileExists("/usr/bin/mpc") || FileExists("/bin/mpc") ||
                       FileExists("/usr/local/bin/mpc");

        running = true;
        workerThread = std::thread([this]() {
            // Nothing escapes this thread.
            try {
                this->run();
            } catch (const std::exception &e) {
                LogErr(VB_PLUGIN, "VAST-FMT: run() exception: %s\n", e.what());
            } catch (...) {
                LogErr(VB_PLUGIN, "VAST-FMT: run() unknown exception\n");
            }
        });

        registerCommands();

        // Bringing the transmitter up resets the hardware and waits on it, so
        // it belongs on the worker rather than in fppd's plugin load.
        queueWork([this]() {
            if (settings["Start"] == "FPPDStart") {
                startVast();
            } else if (settings["Start"] == "RDSOnly") {
                startVastForRDS();
            } else if (settings["Start"] == "PlaylistStart" &&
                       Player::INSTANCE.IsPlaying()) {
                // Loaded into a show already in progress - the "start" for it
                // happened before this plugin existed and will not come again.
                playlistActive = true;
                startVast();
            }
        });
    }
    // Order matters here. Withdraw the HTTP routes first: that call does not
    // return until no request is executing in the handler and the handler
    // itself has been destroyed, so nothing can queue new work afterwards.
    // Then stop and join the worker, whose body is this plugin's own code and
    // touches every member - it has to finish while the object is whole, and
    // before the library it lives in can be unmapped. Only then close the
    // device, which for the USB part also joins hidapi's read thread.
    virtual std::function<bool()> shutdown() override {
        unregisterApis();
        unregisterCommands();
        stopWorker();
        closeDevice();
        return nullptr;
    }

    virtual ~FPPVastFMPlugin() {
        // Backstop for a teardown that never called shutdown(). All three are
        // no-ops once shutdown() has run - unregisterCommands() empties the
        // list it iterates, so nothing is removed or deleted twice.
        unregisterCommands();
        stopWorker();
        closeDevice();
    }

    // Idempotent, so shutdown() and the destructor can both call it.
    void stopWorker() {
        if (!workerThread.joinable()) {
            return;
        }
        running = false;
        condition.notify_all();
        workerThread.join();
    }

    void queueWork(std::function<void()> fn) {
        if (!running) {
            return;   // never queue work the worker will not come back for
        }
        {
            std::lock_guard<std::mutex> lk(lock);
            functions.emplace(std::move(fn));
        }
        condition.notify_all();
    }

    // Run work on the worker and wait for it. A timeout does NOT cancel the
    // job, so it must not reach anything owned by the caller's frame: it
    // writes into the RadioJob it is handed, and fn captures only `this`.
    std::shared_ptr<RadioJob> runOnWorker(const std::function<void(RadioJob &)> &fn,
                                          int timeoutMs) {
        if (!running) {
            return nullptr;
        }
        auto job = std::make_shared<RadioJob>();
        {
            std::lock_guard<std::mutex> lk(lock);
            functions.emplace([fn, job]() {
                try {
                    fn(*job);
                } catch (const std::exception &e) {
                    LogErr(VB_PLUGIN, "VAST-FMT: exception in job: %s\n", e.what());
                } catch (...) {
                    LogErr(VB_PLUGIN, "VAST-FMT: unknown exception in job\n");
                }
                {
                    std::lock_guard<std::mutex> g(job->mutex);
                    job->done = true;
                }
                job->cv.notify_one();
            });
        }
        condition.notify_all();
        std::unique_lock<std::mutex> ul(job->mutex);
        if (!job->cv.wait_for(ul, std::chrono::milliseconds(timeoutMs),
                              [&job]() { return job->done; })) {
            return nullptr;
        }
        return job;
    }

    // Called with the worker stopped, so it needs no lock of its own.
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

            // A transmitter whose I2C side answers but whose core never comes
            // up reports no part number, and everything after this point
            // then "succeeds" against a chip that is not running - settings
            // apply, the log looks normal, and nothing is transmitted. Say so
            // instead. The family is checked rather than one part: the bare
            // modules are Si4713 and the USB adapter carries an Si4711.
            partNumber = si4713->readPartNumber();
            if (partNumber < 0) {
                // Two very different causes, so say which one to look at first.
                // On I2C the likeliest is that the chip is still in reset: GPIO
                // lines are exclusive, so if anything else already holds the
                // reset pin - a stray gpioset, a service, a previous process -
                // FPP cannot drive it, the chip never comes out of reset and
                // never answers, and nothing above here would have noticed.
                // Sending someone to replace a working module over that is the
                // wrong first move.
                if (settings["Connection"] == "I2C") {
                    LogErr(VB_PLUGIN, "VAST-FMT: no part number from the transmitter. "
                           "It is probably still in reset: check that reset pin \"%s\" is "
                           "correct and that nothing else holds it - GPIO lines are "
                           "exclusive, so a leftover gpioset or another service stops FPP "
                           "driving it ('gpioinfo | grep -i consumer' will show who has "
                           "it). Failing that, check the module's supply and reference "
                           "clock.\n", settings["ResetPin"].c_str());
                } else {
                    LogErr(VB_PLUGIN, "VAST-FMT: no part number from the transmitter - "
                           "it answers but never powers up. Check its supply and "
                           "reference clock, or try another module.\n");
                }
                delete si4713;
                si4713 = nullptr;
                return false;
            }
            if (partNumber < 10 || partNumber > 13) {
                LogWarn(VB_PLUGIN, "VAST-FMT: unexpected part number %d - "
                        "continuing, but this may not be an Si471x\n", partNumber);
            } else {
                LogInfo(VB_PLUGIN, "VAST-FMT: part is %s%s\n",
                        Si4713::partName(partNumber),
                        Si4713::supportsNoiseMeasure(partNumber)
                            ? "" : " (no received-noise measurement on this part)");
            }

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
        formatAndSendText(effectiveStationText(), "", "", true);
        formatAndSendText(effectiveRdsText(), "", "", false);
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
        if (key == "AfterHoursRDS") {
            // Turning it off should drop the stream title rather than leave
            // the last track frozen on air.
            if (settings["AfterHoursRDS"] == "0" && !mpcTitle.empty()) {
                mpcTitle.clear();
                mpcArtist.clear();
                queueWork([this]() {
                    formatAndSendText(effectiveStationText(), "", "", true);
                    formatAndSendText(effectiveRdsText(), "", "", false);
                });
            }
            return;
        }
        if (key == "Start" || key == "Stop" || key == "EnableVolumeChangeHack") {
            // Read at point of use in the playlist callbacks - nothing to do.
            return;
        }
        if (key == "StationText" || key == "RDSTextText") {
            // Just push the new text; no reason to drop the carrier for it.
            queueWork([this]() {
                if (si4713 != nullptr && rdsEnabled) {
                    formatAndSendText(effectiveStationText(), "", "", true);
                    formatAndSendText(effectiveRdsText(), "", "", false);
                }
            });
            return;
        }
        LogInfo(VB_PLUGIN, "VAST-FMT: %s changed, reconfiguring\n", key.c_str());
        queueWork([this]() { applyConfiguration(); });
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
        LogInfo(VB_PLUGIN, "VAST-FMT: playlistCallback action=%s section=%s item=%d\n",
                action.c_str(), section.c_str(), item);

        // FPP builds before the Playlist.cpp fix send one more "playing"
        // immediately after "stop", as the player goes idle. Taking that at
        // face value undoes the stop - with Stop at: Playlist Stop, the
        // transmitter came straight back up after a show ended. Belt and
        // suspenders for those builds: only treat it as a start if the player
        // really is playing.
        if ((action == "start" || action == "playing") &&
                !Player::INSTANCE.IsPlaying()) {
            LogInfo(VB_PLUGIN, "VAST-FMT: ignoring \"%s\" - the player is not playing\n",
                    action.c_str());
            return;
        }

        if (action == "start" || action == "playing") {
            playlistActive = true;
            mpcTitle.clear();   // the playlist's own media data takes over
            mpcArtist.clear();
        } else if (action == "stop") {
            playlistActive = false;
        }
        std::string act = action;
        queueWork([this, act]() {
            if (act == "stop" && rdsEnabled) {
                formatAndSendText(effectiveStationText(), "", "", true);
                formatAndSendText(effectiveRdsText(), "", "", false);
            }
            // FPP only sends "start" when the player was idle. Anything else
            // - a playlist started while one is already running, "Start
            // Playlist At Item", advancing sections - arrives as "playing".
            // Matching only "start" is why Start At Playlist did nothing while
            // FPPD Start worked: the callback fired, just never with the word
            // this was looking for. startVast() is a no-op once the device is
            // open, so taking both is safe.
            if (settings["Start"] == "PlaylistStart" &&
                    (act == "start" || act == "playing")) {
                startVast();
            } else if (settings["Stop"] == "PlaylistStop" && act == "stop") {
                stopVast();
            }
        });
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
        
        formatAndSendText(effectiveStationText(), artist, title, true);
        formatAndSendText(effectiveRdsText(), artist, title, false);
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

    // Runs on the worker, so it may talk to the device directly.
    Json::Value statusJsonOnWorker() {
        Json::Value root;
        root["connection"] = settings["Connection"];
        root["part"] = (partNumber >= 0) ? Si4713::partName(partNumber) : "unknown";
        root["partNumber"] = partNumber;
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
    Json::Value retuneOnWorker() {
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
        Json::Value st = statusJsonOnWorker();
        for (const auto &k : st.getMemberNames()) {
            root[k] = st[k];
        }
        return root;
    }

    void handleApi(const HttpRequestPtr &req, HttpCallback &&callback) {
        const bool retune = req->path().find("retune") != std::string::npos;
        auto job = runOnWorker([this, retune](RadioJob &j) {
            j.result = retune ? retuneOnWorker() : statusJsonOnWorker();
        }, retune ? 25000 : 5000);
        if (!job) {
            Json::Value err;
            err["ok"] = false;
            err["error"] = "timeout";
            err["state"] = "busy";
            callback(makeStringResponse(err.toStyledString(), 200, "application/json"));
            return;
        }
        callback(makeStringResponse(job->result.toStyledString(), 200, "application/json"));
    }

    const std::string &effectiveStationText() {
        return stationTextOverride.empty() ? settings["StationText"] : stationTextOverride;
    }
    const std::string &effectiveRdsText() {
        return rdsTextOverride.empty() ? settings["RDSTextText"] : rdsTextOverride;
    }

    // Set an override and push it out now. Runs on the worker, so the command
    // handler itself never touches the transmitter.
    void applyTextOverride(bool station, const std::string &text) {
        queueWork([this, station, text]() {
            (station ? stationTextOverride : rdsTextOverride) = text;
            LogInfo(VB_PLUGIN, "VAST-FMT: %s override %s\n",
                    station ? "station text" : "RDS text",
                    text.empty() ? "cleared" : ("-> \"" + text + "\"").c_str());
            if (si4713 != nullptr && rdsEnabled) {
                formatAndSendText(effectiveStationText(), "", "", true);
                formatAndSendText(effectiveRdsText(), "", "", false);
            }
        });
    }

    bool afterHoursEnabled() const {
        auto it = settings.find("AfterHoursRDS");
        return mpcAvailable && it != settings.end() && it->second != "0";
    }

    // Ask mpd for one formatted field.
    //
    // stderr is discarded on purpose: with no mpd running, mpc writes
    // "MPD error: Connection refused" there and leaves stdout empty, so
    // without this the station would cheerfully broadcast that as its
    // RadioText.
    static std::string readMpcField(const char *format) {
        std::string out;
        std::string cmd = std::string("mpc current -f '") + format + "' 2>/dev/null";
        FILE *f = popen(cmd.c_str(), "r");
        if (f == nullptr) {
            return out;
        }
        char buf[256];
        if (fgets(buf, sizeof(buf), f) != nullptr) {
            out = buf;
        }
        pclose(f);
        while (!out.empty() &&
               (out.back() == '\n' || out.back() == '\r' || out.back() == ' ')) {
            out.pop_back();
        }
        return out;
    }

    void run() {
        std::unique_lock<std::mutex> lk(lock);
        while (running) {
            uint64_t ct = GetTimeMS();

            // While nothing is playing, follow the After Hours stream's title.
            // Only when idle: a running playlist's own media data is better.
            //
            // Runs with the queue lock released - it starts a subprocess, and
            // the RDS update below talks to the transmitter, neither of which
            // should be done holding the lock the callbacks need.
            if (afterHoursEnabled() && !playlistActive && si4713 != nullptr &&
                    rdsEnabled && ct > nextMpcPoll) {
                nextMpcPoll = ct + 12000;
                lk.unlock();
                std::string t = readMpcField("%title%");
                // Streams usually carry only a title, and often put
                // "Artist - Title" in it, so artist is frequently empty -
                // ask anyway, so {Artist} resolves when it is there.
                std::string a = readMpcField("[%artist%|%performer%|%albumartist%]");
                if (t != mpcTitle || a != mpcArtist) {
                    mpcTitle = t;
                    mpcArtist = a;
                    LogInfo(VB_PLUGIN, "VAST-FMT: After Hours \"%s\"%s%s\n", t.c_str(),
                            a.empty() ? "" : " by ", a.c_str());
                    formatAndSendText(effectiveStationText(), a, t, true);
                    formatAndSendText(effectiveRdsText(), a, t, false);
                }
                lk.lock();
            }

            while (!functions.empty()) {
                auto f = functions.front();
                functions.pop();
                lk.unlock();
                try {
                    f();
                } catch (const std::exception &e) {
                    LogErr(VB_PLUGIN, "VAST-FMT: exception in queued work: %s\n", e.what());
                } catch (...) {
                    LogErr(VB_PLUGIN, "VAST-FMT: unknown exception in queued work\n");
                }
                lk.lock();
            }

            if (running && functions.empty()) {
                condition.wait_for(lk, std::chrono::milliseconds(50));
            }
        }
    }

    // FPP commands, so a show can put something on the air without editing
    // the configuration. Running either with an empty string restores the
    // configured text, which is why blanks are allowed on the argument.
    class StationTextCommand : public Command {
    public:
        StationTextCommand(FPPVastFMPlugin *p) :
            Command("VAST-FMT Station Text",
                    "Temporarily replace the RDS station text. Send an empty value to go "
                    "back to the configured station text."),
            plugin(p) {
            args.push_back(CommandArg("text", "string", "Station Text", true));
        }
        std::unique_ptr<Command::Result> run(const std::vector<std::string> &a) override {
            plugin->applyTextOverride(true, a.empty() ? "" : a[0]);
            return std::make_unique<Command::Result>(
                (a.empty() || a[0].empty()) ? "Station text restored" : "Station text set");
        }
        FPPVastFMPlugin *plugin;
    };

    class RdsTextCommand : public Command {
    public:
        RdsTextCommand(FPPVastFMPlugin *p) :
            Command("VAST-FMT RDS Text",
                    "Temporarily replace the RDS text. Send an empty value to go back to "
                    "the configured text and song information."),
            plugin(p) {
            args.push_back(CommandArg("text", "string", "RDS Text", true));
        }
        std::unique_ptr<Command::Result> run(const std::vector<std::string> &a) override {
            plugin->applyTextOverride(false, a.empty() ? "" : a[0]);
            return std::make_unique<Command::Result>(
                (a.empty() || a[0].empty()) ? "RDS text restored" : "RDS text set");
        }
        FPPVastFMPlugin *plugin;
    };

    void registerCommands() {
        myCommands.push_back(new StationTextCommand(this));
        myCommands.push_back(new RdsTextCommand(this));
        for (auto *c : myCommands) {
            CommandManager::INSTANCE.addCommand(c);
        }
    }
    // A plugin owns what it registers. removeCommand() only unregisters - it
    // does not delete, and does not wait for anything in flight - so the delete
    // is ours, and it has to happen before this library is unmapped: a Command
    // subclass declared here has its vtable in this .so.
    //
    // FPP keeps a backstop that deletes whatever a plugin leaves behind, and it
    // compares the registered pointer before doing so, so withdrawing here is
    // not a double delete - it is the path FPP expects, and skipping it earns a
    // warning naming this plugin at unload.
    //
    // Idempotent: the list is cleared, so a second call finds nothing.
    void unregisterCommands() {
        for (auto *c : myCommands) {
            CommandManager::INSTANCE.removeCommand(c);
            delete c;
        }
        myCommands.clear();
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
        setIfNotFound("AfterHoursRDS", "0");
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
    int partNumber = -1;
};


// Safe to dlclose() on unload, in this order, and the order is the point.
//
// This plugin now runs a worker thread - every exchange with the transmitter
// happens there, so a slow I2C or USB transfer never blocks fppd's main loop
// or a drogon thread. The thread's body is this plugin's own code and touches
// every member, so it has to be stopped and joined while the object is still
// whole, and before the library it lives in can be unmapped.
//
// shutdown() therefore: withdraws the HTTP routes, which does not return until
// no request is executing in the handler and the handler object has been
// destroyed, so nothing can queue new work afterwards; then stops and joins
// the worker; then closes the transmitter. A handler blocked waiting on a job
// cannot outlive the worker, and cannot wake holding a freed device.
//
// It registers no timers, issues no CurlManager requests, holds no epoll
// descriptors and adds no commands.
//
// The USB HID path is why unload used to be withheld entirely. hidapi's LIBUSB
// backend was compiled into this library (src/hid.c) and runs a read thread per
// open device, so that thread's entry point sat inside the .so that dlclose()
// unmaps. It now links the system -lhidapi-hidraw instead, as fpp-kfmt already
// did: the hidraw backend is a thin wrapper over read/write/ioctl on
// /dev/hidraw*, starts no threads of its own, and lives in libhidapi-hidraw.so,
// which is never unloaded. This .so therefore defines no hid_* symbol - it only
// imports them - and the one thread it starts is the worker above, which
// shutdown() joins. Check with:
//   nm -D libfpp-vastfmt.so | grep ' hid_'   -> every line must be U, not T
//
// The I2C path never had any of this.
FPP_PLUGIN_SUPPORTS_UNLOAD()

extern "C" {
    FPPPlugins::Plugin *createPlugin() {
        return new FPPVastFMPlugin();
    }
}
