#ifndef __SI4713__
#define __SI4713__

#include <stdint.h>
#include <vector>
#include <string>

class Si4713 {
public:
    Si4713();
    virtual ~Si4713();

    void Init();

    virtual bool isOk() = 0;
    virtual std::string getRev() = 0;
    virtual void powerUp() = 0;
    virtual void powerDown() = 0;
    virtual void reset() = 0;
    virtual std::string getASQ() = 0;
    virtual std::string getTuneStatus() = 0;

    // Raw tune readback. freq is MHz*100. Each transport reads this its own
    // way - the USB part must use the adapter's own tune-status request,
    // because TX_TUNE_STATUS through its raw command passthrough returns
    // zeros. Returns false if the status could not be read.
    virtual bool readTuneStatus(int &freq, int &power, int &antCapRaw) = 0;

    // Was the last automatic antenna-cap search able to find a match? The
    // search rails to the end of its range when it cannot, which is the chip
    // saying the antenna is not resonant anywhere near this frequency.
    static bool antennaMatchOk(int antCapRaw) {
        return antCapRaw > 2 && antCapRaw < 190;
    }
    int lastAntCapRaw() const { return lastAntCap; }
    
    void setEUPreemphasis() {isEUPremphasis = true;}

    // Valid TX_TUNE_POWER levels: 0 powers the PA down, otherwise 88-120 dBuV.
    // Anything else is rejected by the chip, so clamp rather than truncate.
    static int clampPower(int power);
    void setFrequency(int frequency); // freq * 100,  so 8790 for 87.9
    void setTXPower(int power, double antCap);
    

    //RDS stuff
    void setPTY(int i) { pty = i;}
    void beginRDS();
    void setRDSStation(const std::vector<std::string> &station);
    void setRDSBuffer(const std::string &rds,
                      int artistPos, int artistLen,
                      int titlePos, int titleLen);
    void sendTimestamp();

    void enableAudioCompression(bool b = true) { audioCompression = b; }
    void enableAudioLimitter(bool b = true) { audioLimitter = b; }
    void setAudioGain(int i) {audioGain = i;}
    void setAudioCompressionThreshold(int i) { audioCompressionThreshold = i;}
private:
    void sendRtPlusInfo(int content1, int content1_pos, int content1_len,
                        int content2, int content2_pos, int content2_len);

    // The tune commands finish asynchronously; wait for the chip to actually
    // report the tune we asked for before believing anything it says.
    bool waitForTune(int wantFreq, int timeoutMs);

    // Settle after a power/antenna-cap change, where the frequency does not
    // move so there is nothing to compare against.
    void settleAfterPowerChange();

    int lastFreq = 0;
    int lastAntCap = -1;
    
    virtual bool sendSi4711Command(uint8_t cmd, const std::vector<uint8_t> &data, bool ignoreFailures = false);
    virtual bool sendSi4711Command(uint8_t cmd, const std::vector<uint8_t> &data, std::vector<uint8_t> &out, bool ignoreFailures = false) = 0;
    virtual bool setProperty(uint16_t prop, uint16_t val) = 0;

    
    bool isEUPremphasis = false;
    int pty = 2;
    std::vector<std::string> lastStation;
    std::string lastRDS;
    
    bool audioCompression = true;
    bool audioLimitter = true;
    int audioGain = 5;
    int audioCompressionThreshold = -15;
};

#endif
