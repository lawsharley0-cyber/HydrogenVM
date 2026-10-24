#include "runtime/offline/ClashState.hpp"
#include "runtime/offline/ClashCsv.hpp"
#include <cassert>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <unistd.h>

using namespace Hydrogen::Runtime::Offline;
namespace fs = std::filesystem;

int main(int argc, char** argv) {
    // 1. Existing VInt tests
    struct Vector { int32_t n; Bytes bytes; };
    for (auto& v : std::vector<Vector>{{-64, {0xc0, 0x7f}}, {-8192, {0xc0, 0x80, 0x7f}},
        {INT32_MIN, {0xc0, 0x80, 0x80, 0x80, 0}}, {INT32_MAX, {0xbf, 0xff, 0xff, 0xff, 0x0f}},
        {-134217728, {0xc0, 0x80, 0x80, 0x80, 0x0f}}}) {
        Bytes actual; clashVint(actual, v.n);
        if (actual != v.bytes) { std::fprintf(stderr, "native VInt mismatch\n"); return 1; }
    }
    std::puts("ok native VInt literal vectors");

    // Literal wire vectors derived from the pinned native command decoders,
    // not round-tripped through our encoder. Each includes the -1 debug trailer.
    const Bytes chestTurn{10,0,3,
        0xb6,7,0,0,0,1,2,             // 502: chest slot 2
        0xb8,7,0,1,0,1,26,0,          // 504: Knight data reference 26000000
        0xba,7,0,2,0,1,2,1,           // 506: chest slot 2, claim=true
        255,255,255,255};
    const auto commands=decodeClashHomeTurn(chestTurn);
    if(commands.size()!=3 || commands[0].type!=502 || commands[0].argument!=2 ||
        commands[1].type!=504 || commands[1].argument!=26000000 ||
        commands[2].type!=506 || commands[2].argument!=2 || !commands[2].flag) {
        std::fprintf(stderr,"FAIL native home command layout\n");return 1;
    }
    auto rejected=[](const Bytes& bytes){try{decodeClashHomeTurn(bytes);return false;}catch(const std::runtime_error&){return true;}};
    for(size_t n=0;n<chestTurn.size();++n)if(!rejected(Bytes(chestTurn.begin(),chestTurn.begin()+n))) {
        std::fprintf(stderr,"FAIL accepted truncated home turn\n");return 1;
    }
    auto wrongId=chestTurn;wrongId[3]=0xb9; // 505 is not StartExploring or SpeedUpExploring
    auto wrongAccount=chestTurn;wrongAccount[8]=2;
    auto trailing=chestTurn;trailing.push_back(0);
    auto badReference=chestTurn;badReference[16]=0xc0;badReference[17]=0x7f;
    if(!rejected(wrongId) || !rejected(wrongAccount) || !rejected(trailing) || !rejected(badReference)) {
        std::fprintf(stderr,"FAIL invalid home turn validation\n");return 1;
    }
    std::puts("ok native home command vectors, truncation, IDs, executor and references");
    const auto claim=decodeClashHomeTurn({10,0,1,0xb7,7,0,0,0,1,0,255,255,255,255});
    if(claim.size()!=1 || claim[0].type!=503 || claim[0].argument!=0) {
        std::fprintf(stderr,"FAIL native StartRewardClaim command\n");return 1;
    }
    const auto laterClaim=decodeClashHomeTurn({10,0,1,0xb7,7,0,0,0,1,17,255,255,255,255});
    if(laterClaim[0].argument!=17) {std::fprintf(stderr,"FAIL chest ID treated as a slot index\n");return 1;}

    // 2. ClashCsvEngine tests with mock Supercell tables
    const fs::path root = fs::temp_directory_path() / ("hvm-clash-csv-test-" + std::to_string(::getpid()));
    auto put = [&](const std::string& file, const std::string& data) {
        auto path = root / file;
        fs::create_directories(path.parent_path());
        std::ofstream(path) << data;
    };

    put("csv_logic/arenas.csv",
        "Name,Arena,ChestRewardMultiplier,TrophyLimit,DemoteTrophyLimit,PvpLocation\n"
        "String,int,int,int,int,String\n"
        "TrainingCamp,0,100,0,0,TrainingCamp\n"
        "Arena1,1,100,0,0,PvP_goblin\n"
        "Arena2,2,135,400,350,PvP_bone\n"
        "Arena3,3,160,800,750,PvP_barbarian\n");

    put("csv_logic/npcs.csv",
        "Name,Location,PredefinedDeck,Trophies,ExpLevel,TID\n"
        "String,String,String,int,int,String\n"
        "Npc1,FirstMatch,BasicDeck1,0,1,TID_NPC_1\n"
        "Npc2,FirstMatch,BasicDeck2,0,1,TID_NPC_2\n"
        "Npc3,TrainingCamp,BasicDeck3,0,1,TID_NPC_3\n"
        "Npc4,TrainingCamp,BasicDeck4,0,1,TID_NPC_4\n"
        "Npc5,TrainingCamp,BasicDeck5,0,1,TID_NPC_5\n"
        "Npc6,TrainingCamp,BasicDeck6,0,1,TID_NPC_6\n"
        "Npc7,TrainingCamp,BasicDeck7,0,1,TID_NPC_7\n"
        "Npc8,TrainingCamp,BasicDeck8,50,2,TID_NPC_8\n"
        "Npc9,TrainingCamp,BasicDeck9,150,2,TID_NPC_9\n"
        "Npc10,TrainingCamp,BasicDeck10,300,2,TID_NPC_10\n");

    put("csv_logic/spells_characters.csv",
        "Name,Rarity,UnlockArena,ManaCost\n"
        "String,String,String,int\n"
        "Knight,Common,TrainingCamp,3\n"
        "Archer,Common,TrainingCamp,3\n"
        "Goblins,Common,Arena1,2\n"
        "Musketeer,Rare,TrainingCamp,4\n"
        "BabyDragon,Epic,TrainingCamp,4\n");

    put("csv_logic/spells_buildings.csv",
        "Name,Rarity,UnlockArena,ManaCost\n"
        "String,String,String,int\n"
        "Cannon,Common,Arena1,3\n");

    put("csv_logic/spells_other.csv",
        "Name,Rarity,UnlockArena,ManaCost\n"
        "String,String,String,int\n"
        "Fireball,Rare,TrainingCamp,4\n"
        "Arrows,Common,TrainingCamp,3\n");

    put("csv_logic/treasure_chests.csv",
        "Name,BaseChest,Arena,TimeTakenHours,TimeTakenSeconds,MinGold,MaxGold,RandomSpells,DifferentSpells,RareChance,EpicChance,GuaranteedSpells\n"
        "String,String,String,int,int,int,int,int,int,int,int,String\n"
        "Newbie1,,TrainingCamp,,15,16,16,2,2,,,Knight\n"
        "Newbie2,,TrainingCamp,,15,16,16,2,2,10,200,Musketeer\n"
        "Silver,,TrainingCamp,3,,19,29,3,3,12,540,\n"
        "Gold,,TrainingCamp,8,,64,96,10,5,10,285,\n"
        "Silver_Arena1,Silver,Arena1,,,,,,,,,\n"
        "Gold_Arena1,Gold,Arena1,,,,,,,,,\n"
        "Silver_Arena2,Silver,Arena2,,,,,,,,,\n"
        "Gold_Arena2,Gold,Arena2,,,,,,,,,\n");

    put("csv_logic/chest_order.csv",
        "Name,Chest\n"
        "String,String\n"
        "Newbie1,Newbie1\n"
        "Newbie2,Newbie2\n"
        "MainCycle,Silver\n"
        ",Silver\n"
        ",Gold\n");

    try {
        ClashCsvEngine csv(root);

        // Check arena
        auto* arena1 = csv.findArena("Arena1");
        if (!arena1 || arena1->arenaNumber != 1 || arena1->rewardMultiplier != 100 || arena1->pvpLocation != "PvP_goblin") {
            std::fprintf(stderr, "FAIL arena1 mismatch\n"); return 1;
        }

        // Check trophy progression & arena thresholds
        if (csv.getArenaForTrophies(0, 1) != 1 ||
            csv.getArenaForTrophies(399, 1) != 1 ||
            csv.getArenaForTrophies(400, 1) != 2 ||
            csv.getArenaForTrophies(360, 2) != 2 ||
            csv.getArenaForTrophies(340, 2) != 1 ||
            csv.getArenaForTrophies(850, 1) != 3) {
            std::fprintf(stderr, "FAIL arena trophy promotion/demotion mismatch\n"); return 1;
        }

        // Check NPC matchmaking by trophies
        if (csv.findBestNpcForTrophies(40) != 18000007 ||  // Npc8 (50 trophies)
            csv.findBestNpcForTrophies(140) != 18000008 || // Npc9 (150 trophies)
            csv.findBestNpcForTrophies(280) != 18000009) { // Npc10 (300 trophies)
            std::fprintf(stderr, "FAIL NPC trophy matchmaking mismatch\n"); return 1;
        }

        // Check cards
        auto* knight = csv.findCard("Knight");
        if (!knight || knight->globalId != 26000000 || knight->rarity != "Common") {
            std::fprintf(stderr, "FAIL knight card mismatch\n"); return 1;
        }
        auto* fireball = csv.findCard("Fireball");
        if (!fireball || fireball->globalId != 28000000 || fireball->rarity != "Rare") {
            std::fprintf(stderr, "FAIL fireball card mismatch\n"); return 1;
        }

        // Check chest inheritance: Silver_Arena1 must inherit 3 hours (10800s) from Silver
        auto* silverA1 = csv.findChest("Silver_Arena1");
        if (!silverA1 || silverA1->unlockSeconds != 10800 || silverA1->minGold != 19 || silverA1->maxGold != 29) {
            std::fprintf(stderr, "FAIL Silver_Arena1 inheritance mismatch: %d secs\n", silverA1 ? silverA1->unlockSeconds : -1);
            return 1;
        }

        // Check Newbie1: 15s unlock, guarantees Knight
        auto* newbie1 = csv.findChest("Newbie1");
        if (!newbie1 || newbie1->unlockSeconds != 15 || newbie1->guaranteedSpells != std::vector<std::string>{"Knight"}) {
            std::fprintf(stderr, "FAIL Newbie1 mismatch\n"); return 1;
        }

        // Check chest cycle:
        // index 0 -> Newbie1, index 1 -> Newbie2
        // index 2 -> Silver_Arena1, index 3 -> Silver_Arena1, index 4 -> Gold_Arena1
        auto* c0 = csv.getChestForCycle(0, 1);
        auto* c1 = csv.getChestForCycle(1, 1);
        auto* c2 = csv.getChestForCycle(2, 1);
        auto* c3 = csv.getChestForCycle(3, 1);
        auto* c4 = csv.getChestForCycle(4, 1);
        if (!c0 || c0->name != "Newbie1" || !c1 || c1->name != "Newbie2" ||
            !c2 || c2->name != "Silver_Arena1" || !c3 || c3->name != "Silver_Arena1" ||
            !c4 || c4->name != "Gold_Arena1") {
            std::fprintf(stderr, "FAIL chest cycle mapping mismatch\n"); return 1;
        }

        // Check drop calculation: Newbie1 guarantees Knight and gives 16 gold
        auto drops = csv.calculateDrops(*newbie1, 0);
        if (drops.gold != 16 || drops.cards[knight->globalId] < 1) {
            std::fprintf(stderr, "FAIL Newbie1 drop mismatch gold=%d\n", drops.gold); return 1;
        }

        // Check Arena 2 Gold chest applies 135% multiplier
        auto* goldA2 = csv.findChest("Gold_Arena2");
        auto dropsG2 = csv.calculateDrops(*goldA2, 2);
        int32_t expectedMinGold = static_cast<int32_t>(64 * 1.35);
        int32_t expectedMaxGold = static_cast<int32_t>(96 * 1.35);
        if (dropsG2.gold < expectedMinGold || dropsG2.gold > expectedMaxGold) {
            std::fprintf(stderr, "FAIL Gold_Arena2 drop multiplier mismatch gold=%d (expected %d..%d)\n",
                         dropsG2.gold, expectedMinGold, expectedMaxGold); return 1;
        }

        std::puts("ok CSV tables, chest inheritance, cycle progression and reward drops");
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FAIL exception: %s\n", e.what());
        fs::remove_all(root);
        return 1;
    }

    fs::remove_all(root);

    if (argc >= 2) {
        std::string bundle = argv[1];
        ClashProfile profile;
        for (int npc = 0; npc < 7; ++npc) {
            ClashFixtures f(bundle, 18000000 + npc);
            auto h = f.home(profile);
            auto s = f.sector(profile);
            std::printf("Npc%d (%s): home=%zu sector=%zu npcDeck=%zu\n",
                        npc + 1, f.trainerName.c_str(), h.size(), s.size(), f.npcDeck.size());
        }
        ClashFixtures f8(bundle, 18000007, 1);
        auto s8 = f8.sector(profile);
        std::printf("Npc8 Arena 1 (%s): sector=%zu towers=%zu npcDeck=%zu\n",
                    f8.trainerName.c_str(), s8.size(), f8.towers.size(), f8.npcDeck.size());
        if (argc >= 3) {
            ClashFixtures f(bundle, 18000000);
            std::filesystem::create_directories(argv[2]);
            auto write = [&](const char* name, const Bytes& data) {
                std::ofstream file(std::filesystem::path(argv[2]) / name, std::ios::binary);
                file.write(reinterpret_cast<const char*>(data.data()), data.size());
            };
            write("home.bin", f.home(profile));
            write("sector.bin", f.sector(profile));
        }
    }

    return 0;
}
