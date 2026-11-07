// CSV parser and chest progression tables for offline Clash Royale v2.1306.1.
#pragma once
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace Hydrogen::Runtime::Offline {

struct ChestData {
    std::string name;
    std::string baseChest;
    int32_t globalId = 0;
    int32_t unlockSeconds = 0;
    int32_t minGold = 0;
    int32_t maxGold = 0;
    int32_t randomSpells = 0;
    int32_t differentSpells = 0;
    int32_t rareChance = 0;
    int32_t epicChance = 0;
    std::vector<std::string> guaranteedSpells;
};

struct CardData {
    std::string name;
    int32_t globalId = 0;
    std::string rarity; // "Common", "Rare", "Epic", "Legendary"
    std::string unlockArena;
    int32_t manaCost = 0;
};

struct ArenaData {
    std::string name;
    int32_t arenaNumber = 0;
    int32_t rewardMultiplier = 100;
    int32_t trophyLimit = 0;
    int32_t demoteTrophyLimit = 0;
    std::string pvpLocation;
    int32_t matchCost = 0;
};

struct NpcData {
    std::string name;
    int32_t globalId = 0;
    int32_t trophies = 0;
    int32_t expLevel = 1;
    std::string location;
    std::string predefinedDeck;
    std::string tid;
};

struct ChestDropResult {
    int32_t gold = 0;
    std::map<int32_t, int32_t> cards; // globalId -> count
};

class ClashCsvEngine {
public:
    explicit ClashCsvEngine(const std::filesystem::path& resDir);

    const ChestData* findChest(const std::string& name) const;
    const ChestData* findChestById(int32_t globalId) const;
    const ChestData* getChestForCycle(int32_t cycleIndex, int32_t arenaLevel = 1) const;

    const CardData* findCard(const std::string& name) const;
    const CardData* findCardById(int32_t globalId) const;

    const ArenaData* findArena(const std::string& name) const;
    const ArenaData* findArenaByLevel(int32_t level) const;
    int32_t getArenaForTrophies(int32_t trophies, int32_t currentArena = 1) const;

    const NpcData* findNpc(const std::string& name) const;
    const NpcData* findNpcById(int32_t globalId) const;
    int32_t findBestNpcForTrophies(int32_t trophies) const;

    size_t newbieCount() const { return newbieChests_.size(); }
    size_t cycleLength() const { return mainCycle_.size(); }
    size_t npcCount() const { return npcs_.size(); }

    ChestDropResult calculateDrops(const ChestData& chest, int32_t arenaLevel,
                                  const std::map<int32_t, int32_t>& currentInventory = {}) const;

private:
    std::map<std::string, ChestData> chests_;
    std::map<int32_t, std::string> chestById_;
    std::map<std::string, CardData> cards_;
    std::map<int32_t, std::string> cardById_;
    std::map<std::string, ArenaData> arenas_;
    std::map<int32_t, std::string> arenaByLevel_;
    std::map<std::string, NpcData> npcs_;
    std::map<int32_t, std::string> npcById_;
    std::vector<std::string> newbieChests_;
    std::vector<std::string> mainCycle_;

    void loadArenas(const std::filesystem::path& resDir);
    void loadNpcs(const std::filesystem::path& resDir);
    void loadSpells(const std::filesystem::path& resDir);
    void loadChests(const std::filesystem::path& resDir);
    void loadChestOrder(const std::filesystem::path& resDir);
};

} // namespace Hydrogen::Runtime::Offline
