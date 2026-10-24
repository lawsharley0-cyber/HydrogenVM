// Version-pinned offline protocol state. Original client assets are read only.
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <array>
namespace Hydrogen::Runtime::Offline {
using Bytes = std::vector<uint8_t>;
struct ClashProfile {
    std::string token;
    int32_t created=0, npcWins=0, npcLosses=0,lastRewardedWin=0,chestCycleIndex=0;
    int32_t trophies=0, arenaLevel=0;
    std::array<int32_t,4> chests{};
    Bytes nativeHome,nativeAvatar;
    size_t avatarOffset=0;
};
struct ClashFixtures {
    struct Card { int32_t data,level,serial,count,timer; };
    struct Tower { int32_t data,owner,x,y,hp; bool king; };
    std::vector<Card> playerDeck,npcDeck;
    std::vector<Tower> towers;
    int32_t seed=0,npcMana=0,damageTypes=0,freeChestTicks=0;
    int32_t mission=18000000,location=15000000,arena=54000000,tutorial=48000000,tutorialWait=3000;
    std::string trainerName="Trainer Earl";
    explicit ClashFixtures(const std::string& bundle,int32_t npc=18000000,int32_t arenaLevel=0);
    Bytes home(const ClashProfile&) const;
    Bytes sector(const ClashProfile&) const;
};
struct ClashHomeView {
    struct Chest {int32_t data=0;bool unlocked=false,coOpen=false,isNew=true,hasTimer=false;std::array<int32_t,3> timer{};int32_t value8=0,source=0,valueC=-1;};
    size_t chestOffset=0,chestEnd=0;
    std::array<int32_t,4> chests{};
    std::array<Chest,4> states{};
    std::vector<ClashFixtures::Card> deck;
};
ClashHomeView inspectClashHome(const Bytes&);
struct ClashHomeCommand {
    int32_t type=0, argument=0;
    bool flag=false;
};
// Validate the entire pinned home turn before acting on any command. Unsupported
// battle/debug commands are diagnostic only; callers must not acknowledge them.
std::vector<ClashHomeCommand> decodeClashHomeTurn(const Bytes&);
Bytes replaceClashHomeChests(const Bytes&,const std::array<int32_t,4>&,size_t& avatarOffset);
// Native ByteStream primitives, intentionally not ordinary LEB128.
void clashVint(Bytes&,int32_t);
void clashInt(Bytes&,int32_t);
void clashString(Bytes&,const std::string&);
}
