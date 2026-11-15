// PMD co-op LAN protocol (host-authoritative streaming).
//
// The host runs the emulator. Clients send their pad state and receive
// compressed frames + audio. The game itself (patched ROM) tells the host
// whose turn it is through COOP_CFG in ARM9 ITCM.
#pragma once
#include <cstdint>

namespace Coop
{

constexpr uint16_t DefaultPort = 7064;
constexpr uint8_t ProtocolVersion = 3;

// LAN discovery: clients broadcast DiscoveryRequest to this UDP port, hosts answer with DiscoveryReply
constexpr uint16_t DiscoveryPort = 7065;
constexpr const char* DiscoveryRequest = "PMDCOOP?";
constexpr const char* DiscoveryMagic = "PMDCOOP!";
constexpr int MaxPlayers = 4;

// ENet channels
enum : uint8_t { ChanControl = 0, ChanFrame = 1, ChanInput = 2, NumChannels = 3 };

enum : uint8_t
{
    MsgHello = 1,     // C->H  HelloMsg
    MsgWelcome = 2,   // H->C  WelcomeMsg
    MsgReject = 3,    // H->C  RejectMsg
    MsgInput = 4,     // C->H  InputMsg
    MsgFrame = 5,     // H->C  FrameHeader + zstd(payload)
};

#pragma pack(push, 1)
struct HelloMsg
{
    uint8_t type, version;
    char name[16];
    uint32_t clientId;  // random per client installation: lets a returning player get the same slot back
};
struct WelcomeMsg { uint8_t type, version, player; };
struct RejectMsg { uint8_t type, reason; };   // 1 = full, 2 = version
struct InputMsg
{
    uint8_t type;
    uint16_t keys;    // melonDS key mask layout: bit = 0 means pressed
                      // (A,B,Select,Start,Right,Left,Up,Down,R,L,X,Y)
    uint8_t touching;
    uint8_t touchX, touchY;
    uint8_t hotkey;     // last hotkey pressed: 1-4 = move slot, 5-10 = item slot 1-6
    uint8_t hotkeySeq;  // incremented on every hotkey press
};
struct DiscoveryReply
{
    char magic[8];          // DiscoveryMagic, not NUL terminated
    uint8_t version;        // ProtocolVersion of the host
    uint8_t players;        // host + connected clients
    uint8_t maxPlayers;
    uint8_t coopRom;        // 1 if a co-op patched ROM is running
    uint16_t port;          // ENet port to connect to
    char name[16];          // host player's name
    uint32_t hostId;        // random per hosting session: the same host answers once per network interface
};
struct FrameHeader
{
    uint8_t type;
    uint8_t activePlayer;   // 1..4, whose turn it is (0 = unknown / unpatched ROM)
    uint8_t numPlayers;
    uint8_t flags;          // bit0: payload contains video
    uint32_t frameNum;
    uint32_t audioFreq;
    uint16_t audioFrames;   // stereo s16 frames following the video
    uint16_t reserved;
    uint32_t rawSize;       // uncompressed payload size
};
#pragma pack(pop)

constexpr int ScreenW = 256, ScreenH = 192;
constexpr int ScreenPixels = ScreenW * ScreenH;
// payload: [top RGB32 * ScreenPixels][bottom RGB32 * ScreenPixels][audio s16*2*audioFrames]

// COOP_CFG written by tools/patch_coop.py into the ROM's ITCM
constexpr uint32_t CfgAddr = 0x01FFA700;
constexpr uint32_t CfgMagic = 0x504F4F43; // 'COOP'
// +0 magic, +4 numPlayers, +5 activePlayer, +6 version
// +8 moveHotkey: 1-4 written by the host together with an L+A press; the game then uses
//    that move slot instead of the "set" move and clears the byte

}
