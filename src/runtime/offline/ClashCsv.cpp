#include "ClashCsv.hpp"
#include <algorithm>
#include <charconv>
#include <fstream>
#include <random>
#include <sstream>
#include <stdexcept>

namespace Hydrogen::Runtime::Offline {
namespace {

using Row = std::vector<std::string>;
using Record = std::map<std::string, std::string>;

std::string readFile(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file || file.tellg() < 0 || file.tellg() > 8 * 1024 * 1024)
        throw std::runtime_error("Offline Clash: missing or oversized CSV " + path.filename().string());
    std::string text(static_cast<size_t>(file.tellg()), '\0');
    file.seekg(0);
    file.read(text.data(), text.size());
    if (!file) throw std::runtime_error("Offline Clash: failed to read " + path.filename().string());
    if (text.starts_with("\xef\xbb\xbf")) text.erase(0, 3);
    return text;
}

int32_t parseInt(const std::string& s, int32_t def = 0) {
    if (s.empty()) return def;
    int32_t val = 0;
    auto [end, err] = std::from_chars(s.data(), s.data() + s.size(), val);
    return err == std::errc{} ? val : def;
}

std::vector<Row> parseCsv(const std::string& text) {
    std::vector<Row> rows;
    Row row;
    std::string field;
    bool quoted = false;
    for (size_t i = 0; i < text.size(); ++i) {
        char c = text[i];
        if (c == '"') {
            if (quoted && i + 1 < text.size() && text[i + 1] == '"') {
                field += '"';
                ++i;
            } else {
                quoted = !quoted;
            }
        } else if (!quoted && (c == ',' || c == '\n')) {
            row.push_back(field);
            field.clear();
            if (c == '\n') {
                rows.push_back(row);
                row.clear();
            }
        } else if (c != '\r' || quoted) {
            field += c;
        }
    }
    if (!field.empty() || !row.empty()) {
        row.push_back(field);
        rows.push_back(row);
    }
    return rows;
}

std::vector<Record> readTable(const std::filesystem::path& resDir, const std::string& name, bool allowContinuation = false) {
    auto path = resDir / "csv_logic" / (name + ".csv");
    auto rows = parseCsv(readFile(path));
    if (rows.size() < 3) return {};
    const auto& header = rows[0];
    std::vector<Record> records;
    std::string lastName;
    for (size_t i = 2; i < rows.size(); ++i) {
        const auto& r = rows[i];
        if (r.empty()) continue;
        std::string rowName = r[0];
        if (rowName.empty()) {
            if (!allowContinuation) continue;
            rowName = lastName;
        } else {
            lastName = rowName;
        }
        Record rec;
        for (size_t j = 0; j < header.size(); ++j) {
            rec[header[j]] = j < r.size() ? r[j] : "";
        }
        rec["_RowName"] = r[0];
        records.push_back(rec);
    }
    return records;
}

std::vector<std::string> split(const std::string& s, char delim) {
    std::vector<std::string> out;
    std::stringstream ss(s);
    std::string item;
    while (std::getline(ss, item, delim)) {
        if (!item.empty()) out.push_back(item);
    }
    return out;
}

} // namespace

ClashCsvEngine::ClashCsvEngine(const std::filesystem::path& resDir) {
    loadArenas(resDir);
    loadNpcs(resDir);
    loadSpells(resDir);
    loadChests(resDir);
    loadChestOrder(resDir);
}

void ClashCsvEngine::loadArenas(const std::filesystem::path& resDir) {
    auto rows = readTable(resDir, "arenas");
    for (size_t idx = 0; idx < rows.size(); ++idx) {
        const auto& r = rows[idx];
        std::string name = r.at("Name");
        int32_t arenaNum = parseInt(r.at("Arena"), static_cast<int32_t>(idx));
        int32_t mult = parseInt(r.at("ChestRewardMultiplier"), 100);
        int32_t trophyLimit = r.count("TrophyLimit") ? parseInt(r.at("TrophyLimit"), 0) : 0;
        int32_t demoteLimit = r.count("DemoteTrophyLimit") ? parseInt(r.at("DemoteTrophyLimit"), 0) : 0;
        std::string pvpLoc = r.count("PvpLocation") ? r.at("PvpLocation") : "";
        const int32_t matchCost=r.count("MatchCost")?parseInt(r.at("MatchCost"),-1):(arenaNum? -1:0);
        if(matchCost<0 || matchCost>500000 || (arenaNum>0 && matchCost==0))
            throw std::runtime_error("Offline Clash: missing/invalid arena match cost");
        ArenaData data{name, arenaNum, mult, trophyLimit, demoteLimit, pvpLoc,matchCost};
        arenas_[name] = data;
        arenaByLevel_[arenaNum] = name;
    }
}

void ClashCsvEngine::loadNpcs(const std::filesystem::path& resDir) {
    if (!std::filesystem::exists(resDir / "csv_logic/npcs.csv")) return;
    auto rows = readTable(resDir, "npcs");
    for (size_t idx = 0; idx < rows.size(); ++idx) {
        const auto& r = rows[idx];
        std::string name = r.at("Name");
        int32_t gid = 18000000 + static_cast<int32_t>(idx);
        int32_t trophies = r.count("Trophies") ? parseInt(r.at("Trophies"), 0) : 0;
        int32_t expLevel = r.count("ExpLevel") ? parseInt(r.at("ExpLevel"), 1) : 1;
        std::string loc = r.count("Location") ? r.at("Location") : "";
        std::string deck = r.count("PredefinedDeck") ? r.at("PredefinedDeck") : "";
        std::string tid = r.count("TID") ? r.at("TID") : "";
        NpcData npc{name, gid, trophies, expLevel, loc, deck, tid};
        npcs_[name] = npc;
        npcById_[gid] = name;
    }
}

void ClashCsvEngine::loadSpells(const std::filesystem::path& resDir) {
    const std::tuple<std::string, int32_t> files[] = {
        {"spells_characters", 26000000},
        {"spells_buildings", 27000000},
        {"spells_other", 28000000},
    };
    for (const auto& [file, baseId] : files) {
        auto rows = readTable(resDir, file);
        for (size_t idx = 0; idx < rows.size(); ++idx) {
            const auto& r = rows[idx];
            std::string name = r.at("Name");
            int32_t gid = baseId + static_cast<int32_t>(idx);
            std::string rarity = r.count("Rarity") && !r.at("Rarity").empty() ? r.at("Rarity") : "Common";
            std::string unlock = r.count("UnlockArena") && !r.at("UnlockArena").empty() ? r.at("UnlockArena") : "TrainingCamp";
            int32_t cost = parseInt(r.count("ManaCost") ? r.at("ManaCost") : "0");
            CardData card{name, gid, rarity, unlock, cost};
            cards_[name] = card;
            cardById_[gid] = name;
        }
    }
}

void ClashCsvEngine::loadChests(const std::filesystem::path& resDir) {
    auto rows = readTable(resDir, "treasure_chests");
    std::map<std::string, Record> raw;
    std::vector<std::string> order;
    for (size_t idx = 0; idx < rows.size(); ++idx) {
        std::string name = rows[idx].at("Name");
        raw[name] = rows[idx];
        order.push_back(name);
    }

    for (size_t idx = 0; idx < order.size(); ++idx) {
        const auto& name = order[idx];
        const auto& r = raw[name];
        std::string base = r.count("BaseChest") ? r.at("BaseChest") : "";
        Record merged;
        if (!base.empty() && raw.count(base)) {
            for (const auto& [k, v] : raw[base]) {
                if (!v.empty()) merged[k] = v;
            }
        }
        for (const auto& [k, v] : r) {
            if (!v.empty() || !merged.count(k)) merged[k] = v;
        }

        int32_t days = parseInt(merged["TimeTakenDays"]);
        int32_t hours = parseInt(merged["TimeTakenHours"]);
        int32_t mins = parseInt(merged["TimeTakenMinutes"]);
        int32_t secs = parseInt(merged["TimeTakenSeconds"]);
        int32_t unlockSecs = days * 86400 + hours * 3600 + mins * 60 + secs;

        std::vector<std::string> guaranteed;
        if (merged.count("GuaranteedSpells") && !merged["GuaranteedSpells"].empty()) {
            guaranteed = split(merged["GuaranteedSpells"], ';');
        }

        int32_t gid = 19000000 + static_cast<int32_t>(idx);
        ChestData chest{
            name,
            base,
            gid,
            unlockSecs,
            parseInt(merged["MinGold"]),
            parseInt(merged["MaxGold"]),
            parseInt(merged["RandomSpells"]),
            parseInt(merged["DifferentSpells"]),
            parseInt(merged["RareChance"]),
            parseInt(merged["EpicChance"]),
            guaranteed
        };
        chests_[name] = chest;
        chestById_[gid] = name;
    }
}

void ClashCsvEngine::loadChestOrder(const std::filesystem::path& resDir) {
    auto rows = readTable(resDir, "chest_order", true);
    for (const auto& r : rows) {
        std::string chestType = r.at("Chest");
        if (chestType.empty()) continue;
        if (r.at("Name").starts_with("Newbie")) {
            newbieChests_.push_back(chestType);
        } else {
            mainCycle_.push_back(chestType);
        }
    }
}

const ChestData* ClashCsvEngine::findChest(const std::string& name) const {
    auto it = chests_.find(name);
    return it != chests_.end() ? &it->second : nullptr;
}

const ChestData* ClashCsvEngine::findChestById(int32_t globalId) const {
    auto it = chestById_.find(globalId);
    return it != chestById_.end() ? findChest(it->second) : nullptr;
}

const ChestData* ClashCsvEngine::getChestForCycle(int32_t cycleIndex, int32_t arenaLevel) const {
    if (cycleIndex < static_cast<int32_t>(newbieChests_.size())) {
        return findChest(newbieChests_[cycleIndex]);
    }
    if (mainCycle_.empty()) return nullptr;
    int32_t pos = (cycleIndex - static_cast<int32_t>(newbieChests_.size())) % static_cast<int32_t>(mainCycle_.size());
    std::string baseType = mainCycle_[pos];
    if (arenaLevel <= 0) return findChest(baseType);
    std::string arenaSpecific = baseType + "_Arena" + std::to_string(arenaLevel);
    const auto* found = findChest(arenaSpecific);
    return found ? found : findChest(baseType);
}

const CardData* ClashCsvEngine::findCard(const std::string& name) const {
    auto it = cards_.find(name);
    return it != cards_.end() ? &it->second : nullptr;
}

const CardData* ClashCsvEngine::findCardById(int32_t globalId) const {
    auto it = cardById_.find(globalId);
    return it != cardById_.end() ? findCard(it->second) : nullptr;
}

const ArenaData* ClashCsvEngine::findArena(const std::string& name) const {
    auto it = arenas_.find(name);
    return it != arenas_.end() ? &it->second : nullptr;
}

const ArenaData* ClashCsvEngine::findArenaByLevel(int32_t level) const {
    auto it = arenaByLevel_.find(level);
    return it != arenaByLevel_.end() ? findArena(it->second) : nullptr;
}

int32_t ClashCsvEngine::getArenaForTrophies(int32_t trophies, int32_t currentArena) const {
    if (currentArena <= 0) currentArena = 1;
    // Check demotion
    const auto* cur = findArenaByLevel(currentArena);
    if (cur && cur->demoteTrophyLimit > 0 && trophies < cur->demoteTrophyLimit && currentArena > 1) {
        return currentArena - 1;
    }
    // Check promotion
    int32_t nextArena = currentArena + 1;
    while (true) {
        const auto* next = findArenaByLevel(nextArena);
        if (!next || next->trophyLimit <= 0) break;
        if (trophies >= next->trophyLimit) {
            currentArena = nextArena;
            nextArena++;
        } else {
            break;
        }
    }
    return currentArena;
}

const NpcData* ClashCsvEngine::findNpc(const std::string& name) const {
    auto it = npcs_.find(name);
    return it != npcs_.end() ? &it->second : nullptr;
}

const NpcData* ClashCsvEngine::findNpcById(int32_t globalId) const {
    auto it = npcById_.find(globalId);
    if (it == npcById_.end()) return nullptr;
    return findNpc(it->second);
}

int32_t ClashCsvEngine::findBestNpcForTrophies(int32_t trophies) const {
    if (npcs_.empty()) return 18000007; // Default Npc8
    int32_t bestId = 18000007;
    int32_t bestDiff = INT32_MAX;
    for (const auto& [name, npc] : npcs_) {
        if (npc.globalId < 18000007) continue; // Skip training camp NPCs (Npc1..Npc7)
        int32_t diff = std::abs(npc.trophies - trophies);
        if (diff < bestDiff) {
            bestDiff = diff;
            bestId = npc.globalId;
        }
    }
    return bestId;
}

ChestDropResult ClashCsvEngine::calculateDrops(const ChestData& chest, int32_t arenaLevel,
                                              const std::map<int32_t, int32_t>& currentInventory) const {
    const auto* arena = findArenaByLevel(arenaLevel);
    double mult = (arena ? arena->rewardMultiplier : 100) / 100.0;

    int32_t minG = static_cast<int32_t>(chest.minGold * mult);
    int32_t maxG = static_cast<int32_t>(chest.maxGold * mult);
    int32_t gold = minG >= maxG ? minG : (minG + (std::rand() % (maxG - minG + 1)));

    int32_t totalCards = static_cast<int32_t>(chest.randomSpells * mult);
    ChestDropResult result;
    result.gold = gold;

    // 1. Guaranteed cards
    for (const auto& spellName : chest.guaranteedSpells) {
        const auto* c = findCard(spellName);
        if (c && totalCards > 0) {
            result.cards[c->globalId]++;
            --totalCards;
        }
    }

    // 2. Epics
    if (chest.epicChance > 0 && totalCards > 0) {
        int32_t epics = totalCards >= chest.epicChance ? totalCards / chest.epicChance : 0;
        epics = std::min(epics, totalCards);
        std::vector<const CardData*> eligible;
        for (const auto& [_, card] : cards_) {
            if (card.rarity == "Epic") {
                const auto* cardArena = findArena(card.unlockArena);
                if (cardArena && cardArena->arenaNumber <= arenaLevel) eligible.push_back(&card);
            }
        }
        if (!eligible.empty()) {
            for (int32_t i = 0; i < epics; ++i) {
                const auto* chosen = eligible[std::rand() % eligible.size()];
                result.cards[chosen->globalId]++;
                --totalCards;
            }
        }
    }

    // 3. Rares
    if (chest.rareChance > 0 && totalCards > 0) {
        int32_t rares = totalCards >= chest.rareChance ? totalCards / chest.rareChance : 0;
        rares = std::min(rares, totalCards);
        std::vector<const CardData*> eligible;
        for (const auto& [_, card] : cards_) {
            if (card.rarity == "Rare") {
                const auto* cardArena = findArena(card.unlockArena);
                if (cardArena && cardArena->arenaNumber <= arenaLevel) eligible.push_back(&card);
            }
        }
        if (!eligible.empty()) {
            for (int32_t i = 0; i < rares; ++i) {
                const auto* chosen = eligible[std::rand() % eligible.size()];
                result.cards[chosen->globalId]++;
                --totalCards;
            }
        }
    }

    // 4. Commons (inverse weight by current inventory)
    if (totalCards > 0) {
        std::vector<const CardData*> eligible;
        for (const auto& [_, card] : cards_) {
            if (card.rarity == "Common") {
                const auto* cardArena = findArena(card.unlockArena);
                if (cardArena && cardArena->arenaNumber <= arenaLevel) eligible.push_back(&card);
            }
        }
        if (eligible.empty()) {
            for (const auto& [_, card] : cards_) {
                const auto* cardArena = findArena(card.unlockArena);
                if (cardArena && cardArena->arenaNumber <= arenaLevel) eligible.push_back(&card);
            }
        }
        if (!eligible.empty()) {
            std::vector<double> weights;
            for (const auto* c : eligible) {
                auto it = currentInventory.find(c->globalId);
                int count = it != currentInventory.end() ? it->second : 0;
                weights.push_back(1.0 / (count + 1));
            }
            std::discrete_distribution<size_t> dist(weights.begin(), weights.end());
            std::mt19937 gen(std::random_device{}());
            for (int32_t i = 0; i < totalCards; ++i) {
                const auto* chosen = eligible[dist(gen)];
                result.cards[chosen->globalId]++;
            }
        }
    }

    return result;
}

} // namespace Hydrogen::Runtime::Offline
