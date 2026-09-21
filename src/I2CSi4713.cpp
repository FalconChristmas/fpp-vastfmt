#include <fpp-pch.h>

#include <thread>

#include "log.h"

#include "I2CSi4713.h"

#include "util/I2CUtils.h"
#include "util/GPIOUtils.h"

// Commands
#define SI4710_CMD_POWER_UP         0x01
#define SI4710_CMD_GET_REV          0x10
#define SI4710_CMD_POWER_DOWN       0x11
#define SI4710_CMD_SET_PROPERTY     0x12
#define SI4710_CMD_GET_PROPERTY     0x13
#define SI4710_CMD_GET_INT_STATUS   0x14
#define SI4710_CMD_PATCH_ARGS       0x15
#define SI4710_CMD_PATCH_DATA       0x16
#define SI4710_CMD_TX_TUNE_FREQ     0x30
#define SI4710_CMD_TX_TUNE_POWER    0x31
#define SI4710_CMD_TX_TUNE_MEASURE  0x32
#define SI4710_CMD_TX_TUNE_STATUS   0x33
#define SI4710_CMD_TX_ASQ_STATUS    0x34
#define SI4710_CMD_TX_RDS_BUFF      0x35
#define SI4710_CMD_TX_RDS_PS        0x36
#define SI4710_CMD_TX_AGC_OVERRIDE  0x48
#define SI4710_CMD_GPO_CTL          0x80
#define SI4710_CMD_GPO_SET          0x81


#define SI4713_PROP_REFCLK_FREQ   0x0201

#if defined(PLATFORM_BBB) || defined(PLATFORM_BB64)
#define I2CBUS 2
#else
#define I2CBUS 1
#endif

I2CSi4713::I2CSi4713(const std::string &gpioPin) {
    // getPinByName() returns a null-object for a name the platform does not
    // have, and its ptr() is nullptr. Settings now apply without an fppd
    // restart, so an unknown reset pin typed into the web UI used to take fppd
    // down from under a running show rather than just failing to start.
    resetPin = PinCapabilities::getPinByName(gpioPin).ptr();
    if (resetPin == nullptr) {
        LogErr(VB_PLUGIN, "Si4713/I2C: reset pin \"%s\" is not a pin on this board; "
                          "transmitter not started\n", gpioPin.c_str());
        return;
    }
    resetPin->configPin("gpio", "out");
    resetPin->setValue(1);
    std::this_thread::sleep_for(std::chrono::microseconds(200000));
    resetPin->setValue(0);
    std::this_thread::sleep_for(std::chrono::microseconds(200000));
    resetPin->setValue(1);
    std::this_thread::sleep_for(std::chrono::microseconds(300000));
    i2c = new I2CUtils(I2CBUS, 0x63);
    if (i2c->isOk()) {
        sendSi4711Command(SI4710_CMD_POWER_UP, {0x12, 0x50});
        std::this_thread::sleep_for(std::chrono::microseconds(200000));
        setProperty(SI4713_PROP_REFCLK_FREQ, 32768);
    } else {
        delete i2c;
        i2c = nullptr;
    }
}
I2CSi4713::~I2CSi4713() {
    if (i2c) {
        //sendSi4711Command(SI4710_CMD_POWER_DOWN, {});
        delete i2c;
    }
}


bool I2CSi4713::isOk() {
    return i2c != nullptr;
}
void I2CSi4713::powerUp() {
}
void I2CSi4713::powerDown() {
}
void I2CSi4713::reset() {
}


std::string I2CSi4713::getRev() {
    std::vector<uint8_t> response;
    response.resize(9);
    sendSi4711Command(SI4710_CMD_GET_REV, {0}, response);
    
    int pn = response[1];
    int fw = response[2] << 8 | response[3];
    int patch = response[4] << 8 | response[5];
    int cmp = response[6] << 8 | response[7];
    int chiprev = response[8];
    
    char buf[250];
    snprintf(buf, sizeof(buf), "Si4713 - pn: %X  fw: %X   patch: %X   cmp: %X    chiprev: %X", pn, fw, patch, cmp, chiprev);
    return buf;
}

std::string I2CSi4713::getASQ() {
    std::vector<uint8_t> out;
    out.resize(8);
    sendSi4711Command(SI4710_CMD_TX_ASQ_STATUS, {0x1}, out);
    std::string r = "ASQ Flags: ";
    r += std::to_string(out[1])  + " " +  std::to_string(out[2]) + " " +  std::to_string(out[3]);
    r += "  - InLevel:";
    int lev = out[4];
    if (lev > 127) {
        lev -= 256;
    }
    r += std::to_string(lev);
    r += " dBfs";
    return r;
}
bool I2CSi4713::readTuneStatus(int &freq, int &power, int &antCapRaw) {
    std::vector<uint8_t> out;
    out.resize(8);
    if (!sendSi4711Command(SI4710_CMD_TX_TUNE_STATUS, {0x1}, out)) {
        return false;
    }
    freq = out[2] << 8 | out[3];
    power = out[5];
    antCapRaw = out[6];
    return true;
}

std::string I2CSi4713::getTuneStatus() {
    std::vector<uint8_t> out;
    out.resize(8);
    sendSi4711Command(SI4710_CMD_TX_TUNE_STATUS, {0x1}, out);
    int currFreq = out[2] << 8 | out[3];
    int currdBuV = out[5];
    int currAntCap = out[6];
    
    float f = currFreq / 100.0f;
    
    char buf[100];
    snprintf(buf, sizeof(buf), "Freq: %.1f MHz  -  Power: %d dBuV  -  ANTcap: %d", f, currdBuV, currAntCap);
    return buf;
}
bool I2CSi4713::sendSi4711Command(uint8_t cmd, const std::vector<uint8_t> &data, bool ignoreFailures) {
    LogDebug(VB_PLUGIN, "Sending command %X    datasize: %d (no resp)(if: %d)\n", cmd, data.size(), ignoreFailures);
    int i = i2c->writeBlockData(cmd, data.empty() ? nullptr : &data[0], data.size());
    std::this_thread::sleep_for(std::chrono::microseconds(10000));
    uint8_t out[1];
    i = i2c->readI2CBlockData(0x00, out, 1);

    if (cmd == SI4710_CMD_POWER_UP || cmd == SI4710_CMD_TX_TUNE_FREQ) {
        for (int x = 0; x < 100; x++) {
            std::this_thread::sleep_for(std::chrono::milliseconds(3));
            int i2 = i2c->writeBlockData(SI4710_CMD_GET_INT_STATUS, nullptr, 0);
            i2 = i2c->readI2CBlockData(0x00, out, 1);
            LogExcess(VB_PLUGIN, "   resp %X\n", (int)out[0]);
            if (cmd == SI4710_CMD_POWER_UP) {
                if (i2 > 0 && out[0] & 0x80) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(3));
                    return true;
                }
            } else {
                if (i2 > 0 && out[0] & 0x1) {
                    return true;
                }
            }
        }
    } else if (SI4710_CMD_GET_INT_STATUS == 0x14 && !(out[0] & 0x80)) {
        for (int x = 0; x < 10; x++) {
            std::this_thread::sleep_for(std::chrono::milliseconds(3));
            int i2 = i2c->writeBlockData(SI4710_CMD_GET_INT_STATUS, nullptr, 0);
            i2 = i2c->readI2CBlockData(0x00, out, 1);
            LogExcess(VB_PLUGIN, "   resp %X\n", (int)out[0]);
            if (out[0] & 0x80) {
                return true;
            }
        }
    }
    if (ignoreFailures || i >= 0) {
        LogExcess(VB_PLUGIN, "   resp2 %d,  %X\n", i, (int)out[0]);
        return true;
    }
    LogExcess(VB_PLUGIN, "   Failed: %d\n", i);
    return false;
}

bool I2CSi4713::sendSi4711Command(uint8_t cmd, const std::vector<uint8_t> &data, std::vector<uint8_t> &out, bool ignoreFailures) {
    LogDebug(VB_PLUGIN, "Sending command %X    datasize: %d     toRead:  %d   if: %d\n", cmd, data.size(), out.size(), ignoreFailures);
    int i = i2c->writeBlockData(cmd, data.empty() ? nullptr : &data[0], data.size());
    if (!ignoreFailures && i < 0) {
        return false;
    }
    std::this_thread::sleep_for(std::chrono::microseconds(10000));
    if (out.size()) {
        i = i2c->readI2CBlockData(0x00, &out[0], out.size());
        LogExcess(VB_PLUGIN, "   read1 %d   %X\n", i, (int)out[0]);
    } else {
        uint8_t b[1];
        i = i2c->readI2CBlockData(0x00, &b[0], 1);
        LogExcess(VB_PLUGIN, "   read2 %d   %X\n", i, (int)b[0]);
        if (!(b[0] & 0x80)) {
            LogWarn(VB_PLUGIN, "Failed sending command: %X\n", cmd);
            return false;
        }
    }
    return true;
}


bool I2CSi4713::setProperty(uint16_t prop, uint16_t val) {
    LogDebug(VB_PLUGIN, "Set property: %X  val: %X\n", prop, val);
    std::vector<uint8_t> aucBuf(5);
    aucBuf[0] = 0x00;
    aucBuf[1] = prop >> 8;
    aucBuf[2] = prop;
    aucBuf[3] = val >> 8;
    aucBuf[4] = val;
    
    std::vector<uint8_t> retBuf(1);

    bool b = sendSi4711Command(SI4710_CMD_SET_PROPERTY, aucBuf, retBuf, false);
    int i = retBuf[0];
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    if (!(retBuf[0] & 0x80)) {
        LogWarn(VB_PLUGIN, "Failed to set property: %X  val: %X\n", prop, val);
        return false;
    }
    return b;
}
