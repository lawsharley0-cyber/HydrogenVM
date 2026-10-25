#include "runtime/offline/ClashState.hpp"
#include <algorithm>
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
    if(clashElapsedHomeSeconds(100,160)!=60 || clashElapsedHomeSeconds(0,160)!=0 ||
       clashElapsedHomeSeconds(160,100)!=0 || clashElapsedHomeSeconds(2147483700LL,2147483760LL)!=60 ||
       clashElapsedHomeSeconds(1,INT64_MAX)!=2592000) {
        std::fprintf(stderr,"FAIL elapsed home clock bounds/migration\n");return 1;
    }
    std::puts("ok measured elapsed seconds, old-save migration, backward clock and native tick bounds");

    {
        // Native resource table IDs: Gold=5:1, FreeGold=5:5,
        // ChestOrderIndex=5:2, StarCount=5:4. Preserve the entire other tail.
        Bytes head;for(int n:{0,1,0,1,0,1})clashVint(head,n);clashString(head,"Ledger");
        for(int n:{0,0,54,1,65,0,6})clashVint(head,n);
        const Bytes initialResources{2,5,1,0xa4,1,5,5,0xa4,1};
        Bytes tail{0,0,0,0,0,0xa4,1,0xa4,1,0,1,0,0,0x12,0x34};
        Bytes initial=head;initial.insert(initial.end(),initialResources.begin(),initialResources.end());
        initial.insert(initial.end(),tail.begin(),tail.end());
        const Bytes expectedResources{4,5,1,0xa4,1,5,5,0xa4,1,5,2,10,5,4,3};
        Bytes expected=head;expected.insert(expected.end(),expectedResources.begin(),expectedResources.end());
        expected.insert(expected.end(),tail.begin(),tail.end());
        const auto actual=withClashBattleProgress(initial,10,3);
        Bytes charged=head;const Bytes chargedResources{2,5,1,0xa3,1,5,5,0xa3,1};
        charged.insert(charged.end(),chargedResources.begin(),chargedResources.end());charged.insert(charged.end(),tail.begin(),tail.end());
        if(withClashMatchCost(initial,1)!=charged || withClashMatchCost(initial,0)!=initial ||
           inspectClashAvatar(charged).gold!=99 || inspectClashAvatar(charged).freeGold!=99) {
            std::fprintf(stderr,"FAIL native arena entry cost literal\n");return 1;
        }
        bool insufficient=false;try{withClashMatchCost(initial,101);}catch(const std::runtime_error&){insufficient=true;}
        Bytes partlyPaid=head;const Bytes mixedResources{2,5,1,0xa4,1,5,5,40};
        partlyPaid.insert(partlyPaid.end(),mixedResources.begin(),mixedResources.end());partlyPaid.insert(partlyPaid.end(),tail.begin(),tail.end());
        const auto mixed=inspectClashAvatar(withClashMatchCost(partlyPaid,1));
        if(!insufficient || mixed.gold!=99 || mixed.freeGold!=40) {
            std::fprintf(stderr,"FAIL insufficient/mixed native match gold policy\n");return 1;
        }
        std::puts("ok native Gold/FreeGold match cost, mixed balances and insufficient balance rejection");
        if(actual!=expected || withClashBattleProgress(actual,10)!=actual ||
           inspectClashAvatar(actual).chestCycleIndex!=10 || inspectClashAvatar(actual).pendingCrowns!=3) {
            std::fprintf(stderr,"FAIL earned avatar ledger literal/preservation vector\n");return 1;
        }
        bool rewind=false,duplicate=false;
        try{withClashBattleProgress(actual,9);}catch(const std::runtime_error&){rewind=true;}
        try{withClashBattleProgress(actual,10,1);}catch(const std::runtime_error&){duplicate=true;}
        auto oversized=expected;const auto starAt=head.size()+expectedResources.size()-1;
        oversized.erase(oversized.begin()+starAt);Bytes maximum;clashVint(maximum,INT32_MAX);
        oversized.insert(oversized.begin()+starAt,maximum.begin(),maximum.end());
        bool overflow=false;try{withClashBattleProgress(oversized,10,1);}catch(const std::runtime_error&){overflow=true;}
        if(!rewind || !duplicate || !overflow){std::fprintf(stderr,"FAIL inconsistent avatar ledger accepted\n");return 1;}
        std::puts("ok earned cycle/native crown resource transaction, untouched tail and invalid-transition rejection");
    }

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
        "Name,Arena,ChestRewardMultiplier,TrophyLimit,DemoteTrophyLimit,PvpLocation,MatchCost\n"
        "String,int,int,int,int,String,int\n"
        "TrainingCamp,0,100,0,0,TrainingCamp,0\n"
        "Arena1,1,100,0,0,PvP_goblin,1\n"
        "Arena2,2,135,400,350,PvP_bone,2\n"
        "Arena3,3,160,800,750,PvP_barbarian,3\n");

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
        if(csv.findArenaByLevel(0)->matchCost!=0 || csv.findArenaByLevel(1)->matchCost!=1 ||
           csv.findArenaByLevel(2)->matchCost!=2 || csv.findArenaByLevel(3)->matchCost!=3) {
            std::fprintf(stderr,"FAIL bundled arena match cost selection\n");return 1;
        }

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
        // Original buff constructor at 0xf164 sets damage and size to 100%,
        // not zero. VInt 100 is a4 01. One packed zero flag byte precedes
        // the six fields. All six tower components must have neutral values.
        const Bytes neutralBuffTail{0,0xa4,1,0,0,0,0xa4,1,0};
        int neutralCount=0;auto scan=s8.begin();
        while((scan=std::search(scan,s8.end(),neutralBuffTail.begin(),neutralBuffTail.end()))!=s8.end()){++neutralCount;scan+=neutralBuffTail.size();}
        if(neutralCount!=6){std::fprintf(stderr,"FAIL six towers require neutral damage/size buff percentages\n");return 1;}
        for(const auto& tower:f8.towers) {
            const auto [level,hp]=f8.towerStats(tower,2);
            // Pinned globals: King +8%, Princess +9%. exp_levels row 2
            // selects level index 1. Original bases are 2400 and 1400.
            if(level!=1 || hp!=(tower.king?2592:1526)) {
                std::fprintf(stderr,"FAIL original level-2 tower stats\n");return 1;
            }
        }
        const auto npcName=std::search(s8.begin(),s8.end(),f8.trainerName.begin(),f8.trainerName.end());
        if(npcName==s8.end() || npcName-s8.begin()<10) return 1;
        const auto bot=inspectClashAvatar(Bytes(npcName-10,s8.end()));
        if(bot.arena!=1 || bot.trophies!=50 || f8.npcExpLevel!=2) {
            std::fprintf(stderr,"FAIL Arena bot must use bundled NPC8 trophies, experience and active arena\n");return 1;
        }
        // LogicGameMode::decode 0xd7a80 requires delimiter 12 after avatars,
        // independently of whether tutorial data follows. Removing the tutorial
        // must clear its reference, not the stream delimiter.
        const Bytes arenaTrailer{12,0,0,0,0,0};
        if(s8.size()<arenaTrailer.size() ||
           !std::equal(arenaTrailer.begin(),arenaTrailer.end(),s8.end()-arenaTrailer.size())) {
            std::fprintf(stderr,"FAIL Arena sector must retain native full-update delimiter 12\n");return 1;
        }
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
