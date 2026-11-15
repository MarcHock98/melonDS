// Developer control port for automated testing of the PMD co-op mode.
// Enabled only when the environment variable PMDCOOP_DEBUG=<port> is set. Listens on 127.0.0.1.
// Line protocol (one command per line, reply ends with "ok ..." or "err ..."):
//   status                      -> ok frame=<n> active=<p> clients=<c>
//   hold <hexmask>              -> keep DS keys pressed (bit set = pressed, melonDS order), 0 = release
//   tap <hexmask> [frames]      -> press for N frames (default 6), then release; reply after release + 2 frames
//   wait <frames>               -> reply after N frames
//   shot <path>                 -> write both screens as PPM (256x384)
//   save <path> / load <path>   -> melonDS savestate
//   hotkey <n>                  -> same as the host pressing number key n (10 = key 0)
//   peek <addr> <len>           -> ok <hex>   (ARM9 bus)
//   poke <addr> <hexbytes>      -> ok         (ARM9 bus)
#pragma once
#include <string>
#include <cstdint>
#include "types.h"

namespace melonDS { class NDS; }
class EmuInstance;

class CoopDebug
{
public:
    CoopDebug();
    ~CoopDebug();
    // emu thread, before SetKeyMask: may modify the key mask
    void preFrame(EmuInstance* inst, melonDS::NDS* nds, melonDS::u32& keyMask);
    // emu thread, after RunFrame
    void postFrame(melonDS::NDS* nds, int activePlayer, int clients);

private:
    void handleLine(EmuInstance* inst, melonDS::NDS* nds, const std::string& line);
    void reply(const std::string& s);

    intptr_t listenFd = -1, connFd = -1;
    std::string inbuf;
    uint32_t frame = 0;
    uint16_t holdMask = 0, tapMask = 0;
    int tapFrames = 0, waitFrames = -1;
    int lastActive = 0, lastClients = 0;
};
