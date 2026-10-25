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
    Bytes pendingReward; // immutable native command; replay only against its pre-claim checkpoint
    int32_t pendingRewardSlot=-1;
    int32_t pendingRewardKind=0; // 0 none, 1 slotted, 2 native free chest
    int32_t rewardSerial=0;
    size_t avatarOffset=0;
};
struct ClashFixtures {
    struct Card { int32_t data,level,serial,count,timer; };
    struct Tower { int32_t data,owner,x,y,hp; bool king; };
    std::vector<Card> playerDeck,npcDeck;
    std::vector<Tower> towers;
    std::vector<int32_t> decoData; // native addDecos order; existing object IDs 1,000,000+i
    int32_t seed=0,npcMana=0,damageTypes=0,freeChestTicks=0;
    int32_t npcTrophies=0,npcExpLevel=1;
    int32_t npcTowerHpPercent=100,kingHpGrowth=0,towerHpGrowth=0;
    std::array<int32_t,13> kingLevels{},towerLevels{};
    int32_t mission=18000000,location=15000000,arena=54000000,tutorial=48000000,tutorialWait=3000;
    std::string trainerName="Trainer Earl";
    explicit ClashFixtures(const std::string& bundle,int32_t npc=18000000,int32_t arenaLevel=0);
    Bytes home(const ClashProfile&) const;
    Bytes sector(const ClashProfile&) const;
    std::pair<int32_t,int32_t> towerStats(const Tower&,int32_t expLevel) const;
};
struct ClashHomeView {
    struct Chest {int32_t data=0;bool unlocked=false,claimed=false,isNew=true,hasTimer=false;std::array<int32_t,3> timer{};int32_t value8=0,source=0,valueC=-1;size_t flagOffset=0;};
    size_t chestOffset=0,chestEnd=0;
    size_t chestCounterEnd=0;
    int32_t lastChestId=0;
    std::array<int32_t,4> chests{};
    std::array<Chest,4> states{};
    Chest freeChest{},spareFreeChest{},crownChest{};
    int32_t crownCount=0;
    bool crownCooldown=false;
    std::vector<ClashFixtures::Card> deck;
};
ClashHomeView inspectClashHome(const Bytes&);
Bytes unlockedClashRewardCheckpoint(const Bytes&,int32_t slot,int32_t chestId);
struct ClashAvatarView { int32_t arena=0,trophies=0,expLevel=1,chestCycleIndex=0,pendingCrowns=0; };
ClashAvatarView inspectClashAvatar(const Bytes&);
// Apply earned offline ledger values to the serialized native avatar. Retain
// all unknown fields; no live guest objects, historical crowns or rewards are fabricated.
Bytes withClashBattleProgress(const Bytes&,int32_t chestCycleIndex,int32_t earnedCrowns=0);
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
