#include "runtime/offline/ClashOffline.hpp"
#include "runtime/offline/ClashState.hpp"
#include "runtime/Process.hpp"
#include "runtime/libc/HostCrypto.hpp"
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <filesystem>
#include <fstream>
#include <cstdio>
#include <algorithm>
#include <zlib.h>
using namespace Hydrogen::Runtime;
using namespace Hydrogen::Runtime::Offline;
namespace fs=std::filesystem;
namespace Plist=Hydrogen::Plist;
struct ClientCipher {
    HostCrypto::Cryptor* c=nullptr;
    ClientCipher(){std::string key="fhsd6f86f67rt8fw78fw789we78r9789wer6renonce";if(HostCrypto::create(0,4,0,(const uint8_t*)key.data(),key.size(),nullptr,&c))throw std::runtime_error("RC4 unavailable");apply(Bytes(key.begin(),key.end()));}
    ~ClientCipher(){HostCrypto::release(c);}
    Bytes apply(const Bytes& in){Bytes out(in.size()+32);size_t n=0;if(HostCrypto::update(c,in.data(),in.size(),out.data(),out.size(),&n))throw std::runtime_error("cipher update");out.resize(n);return out;}
};
struct SocketPeer {
    int fd=-1;ClientCipher outgoing,incoming;
    explicit SocketPeer(uint16_t port) {
        fd=::socket(AF_INET,SOCK_STREAM,0);timeval timeout{3,0};::setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof timeout);
        sockaddr_in address{};address.sin_family=AF_INET;address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);address.sin_port=htons(port);
#if defined(__APPLE__)
        address.sin_len=sizeof address;
#endif
        if(::connect(fd,(sockaddr*)&address,sizeof address))throw std::runtime_error("journal client connect failed");
    }
    ~SocketPeer(){if(fd>=0)::close(fd);}
    void send(uint16_t kind,const Bytes& body) {
        auto encrypted=outgoing.apply(body);Bytes packet{uint8_t(kind>>8),uint8_t(kind),uint8_t(body.size()>>16),uint8_t(body.size()>>8),uint8_t(body.size()),0,0};
        packet.insert(packet.end(),encrypted.begin(),encrypted.end());
        if(::send(fd,packet.data(),packet.size(),0)!=ssize_t(packet.size()))throw std::runtime_error("journal client send failed");
    }
    std::pair<uint16_t,Bytes> receive() {
        auto read=[&](uint8_t* data,size_t size){size_t n=0;while(n<size){auto got=::recv(fd,data+n,size-n,0);if(got<=0)throw std::runtime_error("journal client receive failed");n+=size_t(got);}};
        uint8_t h[7];read(h,7);Bytes body(size_t(h[2])<<16|size_t(h[3])<<8|h[4]);read(body.data(),body.size());
        return {uint16_t(uint16_t(h[0])<<8|h[1]),incoming.apply(body)};
    }
};
int main() {
    if(!HostCrypto::available()){std::puts("SKIP host crypto unavailable");return 77;}
    const fs::path root=fs::temp_directory_path()/("hvm-clash-offline-test-"+std::to_string(::getpid()));
    auto put=[&](const std::string& file,const std::string& data){auto path=root/"app/res"/file;fs::create_directories(path.parent_path());std::ofstream(path)<<data;};
    put("level/starting_home.json",R"({"decks":[[{"d":26000000,"t":0,"s":0,"l":0,"c":1},{"d":26000001,"t":0,"s":0,"l":0,"c":1},{"d":26000002,"t":0,"s":0,"l":0,"c":1},{"d":26000003,"t":0,"s":0,"l":0,"c":1},{"d":26000004,"t":0,"s":0,"l":0,"c":1},{"d":26000005,"t":0,"s":0,"l":0,"c":1}]],"selected_deck":0,"chest_slots":4,"ticks":288000})");
    put("csv_logic/npcs.csv","Name,Location,PredefinedDeck,Seed,StartingMana,WizardHpMultiplier,ExpLevel\nString,String,String,int,int,int,int\nNpc1,FirstMatch,BasicDeck1,2,1,40,1\n");
    put("csv_logic/locations.csv","Name,FileName\nString,String\nFirstMatch,locations/training_arena.csv\n");
    put("csv_logic/tutorials_npc.csv","Name,Location,NPC,WaitTimeMS\nString,String,String,int\nStart,FirstMatch,Npc1,3000\n");
    put("csv_client/texts.csv","TID,EN\nString,String\nunused,Trainer\n");
    put("csv_logic/spells_characters.csv","Name\nString\nUnit1\nUnit2\nUnit3\nUnit4\nUnit5\nUnit6\n");
    put("csv_logic/spells_buildings.csv","Name\nString\nBuilding1\n");put("csv_logic/spells_other.csv","Name\nString\nSpell1\n");
    put("csv_logic/predefined_decks.csv","Name,Spells,SpellLevel\nString,String,int\nBasicDeck1,Unit1,1\n");
    put("csv_logic/buildings.csv","Name,Hitpoints\nString,int\nKingTower,100\nPrincessTower,100\n");
    put("csv_logic/damage_types.csv","Name\nString\nA\nB\nC\nD\nE\n");
    put("locations/training_arena.csv","Objects,,,\n,KingTower,10,5\n,PrincessTower,5,5\n,,15,5\n,KingTower,10,50\n,PrincessTower,5,50\n,,15,50\n");
    put("csv_logic/decos.csv","Name,FileName\nString,String\nTestTree,sc/test.sc\n");
    {std::ofstream file(root/"app/res/locations/training_arena.csv",std::ios::app);file<<",TestTree,-1000,1200\n";}
    int failed=0;auto check=[&](bool pass,const char* text){std::printf("%s %s\n",pass?"ok":"FAIL",text);failed+=!pass;};
    const fs::path saved=root/"home/Library/Application Support/HydrogenVM/ClashOffline.plist";
    ProcessConfig cfg;cfg.bundlePath=(root/"app").string();cfg.homePath=(root/"home").string();
    {
        // CollectFreeChest::decode 0x9992c delegates to LogicCommand only;
        // there is no chest-ID payload after the account ID.
        Bytes turn;for(int n:{10,1234,1,518,-1,-1,0,1})clashVint(turn,n);
        clashInt(turn,-1);bool accepted=false;
        try{const auto commands=decodeClashHomeTurn(turn);accepted=commands.size()==1 && commands[0].type==518;}catch(const std::runtime_error&){}
        check(accepted,"free-chest turn command accepts its native metadata-only schema");
        turn.pop_back();bool rejected=false;try{decodeClashHomeTurn(turn);}catch(const std::runtime_error&){rejected=true;}
        check(rejected,"truncated free-chest turn is rejected before requesting a reward");
    }
    {
        // Native schemas: SwapSpells 0x77a88 (two VInts/two packed bools),
        // SpellPageOpened 0xace4c/0xdc688 (spell list), RefreshAchievements
        // 0xacdd8 (base only). A later requested chest must survive this batch.
        Bytes turn;for(int n:{10,1234,4,500,-1,-1,0,1,0,3})clashVint(turn,n);
        turn.push_back(2); // collection -> deck
        for(int n:{525,-1,-1,0,1,2,26,0,28,0,538,-1,-1,0,1,518,-1,-1,0,1})clashVint(turn,n);
        clashInt(turn,-1);
        bool accepted=false;try{const auto commands=decodeClashHomeTurn(turn);
            accepted=commands.size()==4 && commands[0].type==500 && commands[1].type==525 &&
                commands[2].type==538 && commands[3].type==518;}catch(const std::runtime_error&){}
        check(accepted,"native deck/page/achievement echoes preserve a later free-chest request in the same turn");
        turn.pop_back();bool rejected=false;try{decodeClashHomeTurn(turn);}catch(const std::runtime_error&){rejected=true;}
        check(rejected,"truncated mixed native turn rejects the whole batch");
    }
    {
        ClashFixtures fixture(cfg.bundlePath);
        ClashProfile paid;paid.chests[0]=19000000;auto paidHome=fixture.home(paid);
        auto chest=inspectClashHome(paidHome).states[0];
        // Native home encoder 0x124dd0..0x124e48: two free chests,
        // crown count + cooldown flag/timer, then the crown chest itself.
        Bytes timers;for(int n:{fixture.freeChestTicks,0,-1,0,0,-1})clashVint(timers,n);
        paidHome.at(inspectClashHome(paidHome).chestEnd+timers.size()+1)=7;
        const auto homeView=inspectClashHome(paidHome);
        check(homeView.crownCount==7 && !homeView.spareFreeChest.data && !homeView.crownChest.data,
            "crown count follows both free chest slots in the original home schema");
        check(chest.source==1,"new earned chest uses the original arena source");
        paidHome.at(chest.flagOffset)|=3; // current native unlock/claim completed
        auto expected=paidHome;expected.at(chest.flagOffset)&=uint8_t(~2u);
        check(unlockedClashRewardCheckpoint(paidHome,0,chest.value8)==expected,
            "paid reward checkpoint changes only the current claim bit and preserves post-cost avatar/timers");
        paidHome.at(chest.flagOffset)&=uint8_t(~1u);
        bool rejected=false;try{unlockedClashRewardCheckpoint(paidHome,0,chest.value8);}catch(const std::runtime_error&){rejected=true;}
        check(rejected,"paid reward checkpoint rejects an incomplete native unlock");
        const auto sector=fixture.sector({});
        auto countSequence=[&](const Bytes& sequence) {
            size_t count=0;auto at=sector.begin();
            while((at=std::search(at,sector.end(),sequence.begin(),sequence.end()))!=sector.end()){++count;++at;}
            return count;
        };
        // spawnObject 0x13fe8..0x13ffc: normalized Y direction +/-256,
        // followed by character +0x5c=0 and +0x6c=100. Signed VInt literals.
        check(countSequence({0,0x80,4,0,0xa4,1})==3 && countSequence({0,0xc0,0x7c,0,0xa4,1})==3,
            "three towers on each side retain original opposite facing vectors");
        // addDecos gives type-0 objects IDs 1,000,000+i, before type-5 towers.
        // The decoder's reference list must retain those existing objects.
        const Bytes ids{1,0,6,0,6,1,6,2,6,3,6,4,6,5};
        check(fixture.decoData==std::vector<int32_t>{42000000} &&
              std::search(sector.begin(),sector.end(),ids.begin(),ids.end())!=sector.end(),
              "full update retains native static decoration identities before tower IDs");
        for(const auto& tower:fixture.towers)
            check(fixture.towerStats(tower,1).second==(tower.owner==0?40:100),"NPC tower HP multiplier applies once to both tower types");
    }
    {
        // Native addChest (0x124bf0) increments home+0xbc and stores that ID
        // in chest+8. A slot number is not a chest identity.
        ClashProfile p;auto home=ClashFixtures(cfg.bundlePath).home(p);
        const std::string marker="Offline Player";
        auto avatar=std::search(home.begin(),home.end(),marker.begin(),marker.end());
        size_t offset=size_t(avatar-home.begin())-10;
        home=replaceClashHomeChests(home,{19000000,0,0,0},offset);
        home=replaceClashHomeChests(home,{19000000,19000001,0,0},offset);
        auto two=inspectClashHome(home);
        check(two.states[0].value8==1 && two.states[1].value8==2,"earned chests get distinct native monotonically increasing IDs");
        home=replaceClashHomeChests(home,{0,19000001,0,0},offset);
        home=replaceClashHomeChests(home,{19000002,19000001,0,0},offset);
        auto reused=inspectClashHome(home);
        check(reused.states[0].value8==3 && reused.states[1].value8==2,"reusing a slot preserves the other chest ID and advances the ID counter");
        // The counter is the first VInt after the fixed eight-byte home ID.
        Bytes enlarged(home.begin(),home.begin()+8);clashVint(enlarged,63);
        enlarged.insert(enlarged.end(),home.begin()+reused.chestCounterEnd,home.end());
        offset=offset-(reused.chestCounterEnd-8)+1;home=std::move(enlarged);
        home=replaceClashHomeChests(home,{19000002,19000001,19000003,0},offset);
        auto expanded=inspectClashHome(home);
        const std::string storedName(home.begin()+offset+10,home.begin()+offset+10+marker.size());
        check(expanded.lastChestId==64 && expanded.states[2].value8==64 && storedName==marker,
              "chest ID counter VInt expansion preserves the avatar boundary");
    }
    {
        Process process(cfg);ClashOffline service(process);
        int fd=::socket(AF_INET,SOCK_STREAM,0);timeval timeout{3,0};::setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof timeout);
        sockaddr_in address{};address.sin_family=AF_INET;address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);address.sin_port=htons(service.port());
#if defined(__APPLE__)
        address.sin_len=sizeof address;
#endif
        if(::connect(fd,(sockaddr*)&address,sizeof address))throw std::runtime_error("connect failed");
        ClientCipher outgoing,incoming;
        auto send=[&](uint16_t id,const Bytes& body){auto bytes=outgoing.apply(body);Bytes packet{uint8_t(id>>8),uint8_t(id),uint8_t(bytes.size()>>16),uint8_t(bytes.size()>>8),uint8_t(bytes.size()),0,0};packet.insert(packet.end(),bytes.begin(),bytes.end());if(::send(fd,packet.data(),packet.size(),0)!=ssize_t(packet.size()))throw std::runtime_error("send failed");};
        auto receive=[&](){uint8_t header[7];size_t n=0;while(n<7){auto r=::recv(fd,header+n,7-n,0);if(r<=0)throw std::runtime_error("receive header failed");n+=r;}Bytes body(size_t(header[2])<<16|size_t(header[3])<<8|header[4]);n=0;while(n<body.size()){auto r=::recv(fd,body.data()+n,body.size()-n,0);if(r<=0)throw std::runtime_error("receive body failed");n+=r;}return std::pair{uint16_t(uint16_t(header[0])<<8|header[1]),incoming.apply(body)};};
        Bytes login;clashInt(login,0);clashInt(login,0);clashInt(login,-1);for(int n:{2,0,1306})clashVint(login,n);
        send(10101,login);check(receive().first==20104 && receive().first==24101,"embedded encrypted login and home");
        Bytes name;clashString(name,"Solo");name.push_back(1);send(10212,name);
        // Independently decoded command 201: VInt c9=89 03, BE String length,
        // fixed BE name-change-state 0, user=true, server ID 0, ticks -1/-1,
        // account 0/1. Command decoder 0x1229d4 uses readInt, not readVInt.
        const Bytes expectedName{0x89,3,0,0,0,4,'S','o','l','o',0,0,0,0,1,0,0x7f,0x7f,0,1};
        check(receive()==std::pair<uint16_t,Bytes>{24111,expectedName},"initial name request emits the pinned native name command schema");
        send(14104,{18,0});check(receive().first==21903,"embedded first-NPC sector");
        // Literal native command envelope: a truncated 537 must not start a battle.
        send(14102,{10,0,1,0x99,8,0,0,0,1});send(10108,{});
        check(receive()==std::pair<uint16_t,Bytes>{20108,{}},"truncated matchmaking cannot start a battle or break heartbeat");
        Bytes mmTurn;clashVint(mmTurn,10);clashVint(mmTurn,1234);clashVint(mmTurn,1);clashVint(mmTurn,537);
        for(int n:{0,0,0,1,0})clashVint(mmTurn,n);mmTurn.push_back(0);clashInt(mmTurn,-1);
        send(14102,mmTurn);check(receive().first==21903,"matchmaking turn command supplies battle sector");
        send(14102,{0xff});send(10108,{});check(receive()==std::pair<uint16_t,Bytes>{20108,{}} ,"unknown turn does not break RC4 heartbeat");
        send(14101,{0});check(receive().first==24101 && Plist::parseFile(saved.string())->get("npcWins")->i==0,"GoHome does not fabricate a win");
        auto avatar=process.heap().allocZeroed(256);process.mem().write<int32_t>(avatar+0x10,-1);process.mem().write<int32_t>(avatar+0x14,0);
        Hydrogen::CPU::Cpu cpu;cpu.r[0]=avatar;cpu.r[1]=1;cpu.r[Hydrogen::CPU::kLR]=0x14c7d;
        process.engine().notifyEntry(cpu,0x9ae94);check(Plist::parseFile(saved.string())->get("npcWins")->i==0,"NPC avatar cannot grant player progression");
        process.mem().write<int32_t>(avatar+0x10,0);process.mem().write<int32_t>(avatar+0x14,1);
        process.engine().notifyEntry(cpu,0x9ae94);process.engine().notifyEntry(cpu,0x9ae94);
        check(cpu.exit==Hydrogen::CPU::Exit::None && Plist::parseFile(saved.string())->get("npcWins")->i==0,"native win intent waits for the reward transaction");
        process.engine().notifyEntry(cpu,0x14e74);
        auto reward=process.heap().allocZeroed(32);process.mem().write<int32_t>(reward+0x10,19000000);cpu.r[0]=reward;
        process.engine().notifyEntry(cpu,0x14e7c);process.engine().notifyEntry(cpu,0x14e7c);
        auto earned=Plist::parseFile(saved.string());
        check(earned->get("chests")->array[0]->i==19000000 && earned->get("chests")->array[1]->i==0 && earned->get("lastRewardedWin")->i==1 && earned->get("chestCycleIndex")->i==1 && earned->get("trophies")->i==0,"native reward captured once without duplicating the chest");

        // Advance to 7 wins (graduation from training camp)
        for (int w = 2; w <= 7; ++w) {
            cpu.r[0] = avatar; cpu.r[1] = w; cpu.exit = Hydrogen::CPU::Exit::None;
            process.engine().notifyEntry(cpu, 0x9ae94);
            process.engine().notifyEntry(cpu, 0x14e74);
            process.mem().write<int32_t>(reward + 0x10, 19000000); cpu.r[0] = reward;
            process.engine().notifyEntry(cpu, 0x14e7c);
        }
        auto postCamp = Plist::parseFile(saved.string());
        check(postCamp->get("npcWins")->i == 7 && postCamp->get("arenaLevel")->i == 1 && postCamp->get("trophies")->i == 0, "training camp graduation advances to Arena 1");

        // Transport/capture oracle only: a native-computed score is supplied
        // explicitly. The real client, not this observer test, must test ELO.
        process.mem().write<int32_t>(avatar+0x34,34);
        cpu.r[0] = avatar; cpu.r[1] = 8; cpu.exit = Hydrogen::CPU::Exit::None;
        process.engine().notifyEntry(cpu, 0x9ae94);
        process.engine().notifyEntry(cpu, 0x14e74);
        process.mem().write<int32_t>(reward + 0x10, 19000000); cpu.r[0] = reward;
        process.engine().notifyEntry(cpu, 0x14e7c);
        auto pvpWin = Plist::parseFile(saved.string());
        check(pvpWin->get("npcWins")->i == 8 && pvpWin->get("trophies")->i == 34 && pvpWin->get("arenaLevel")->i == 1, "battle metadata captures native score without a fixed win formula");

        process.mem().write<int32_t>(avatar+0x34,3);
        cpu.r[0] = avatar; cpu.r[1] = 1; cpu.r[Hydrogen::CPU::kLR] = 0x14ce7; cpu.exit = Hydrogen::CPU::Exit::None;
        process.engine().notifyEntry(cpu, 0x9aea4);
        auto pvpLoss = Plist::parseFile(saved.string());
        check(pvpLoss->get("npcLosses")->i == 1 && pvpLoss->get("trophies")->i == 3, "battle metadata captures native loss score without a fixed loss formula");

        // Native LogicClientHome::isClaimingReward (0x12580c) reads +0xb4.
        // A checkpoint during this transaction cannot safely be replayed on
        // relaunch: StartRewardClaim rejects a chest already marked claimed.
        auto ui=process.heap().allocZeroed(256),logic=process.heap().allocZeroed(256),home=process.heap().allocZeroed(256);
        process.mem().write<uint32_t>(ui+0x54,logic);
        process.mem().write<uint32_t>(logic+0x30,home);process.mem().write<uint32_t>(logic+0x34,avatar);
        process.mem().write<uint8_t>(home+0xb4,1);
        process.mem().write<int32_t>(avatar+0x9c,8);process.mem().write<int32_t>(avatar+0xa0,1);
        cpu.r[0]=ui;cpu.exit=Hydrogen::CPU::Exit::None;
        process.engine().notifyEntry(cpu,0x92f08);
        check(cpu.exit==Hydrogen::CPU::Exit::None && Plist::parseFile(saved.string())->get("chests")->array[0]->i==19000000,
              "pending native reward does not serialize or replace the last committed checkpoint");

        cpu.r[0]=avatar;
        cpu.r[1]=11;cpu.r[Hydrogen::CPU::kLR]=0x14c7d;cpu.exit=Hydrogen::CPU::Exit::None;
        process.engine().notifyEntry(cpu,0x9ae94);
        check(cpu.exit==Hydrogen::CPU::Exit::HostError && Plist::parseFile(saved.string())->get("npcWins")->i==8,"invalid progression jump rejected without changing save");
        ::close(fd);service.stop();
    }
    {
        Process process(cfg);ClashOffline service(process);
        check(Plist::parseFile(saved.string())->get("npcWins")->i==8,"native counter survives service relaunch");
        bool locked=false;try{Process other(cfg);ClashOffline duplicate(other);}catch(const std::exception&){locked=true;}
        check(locked,"duplicate save session rejected");service.stop();
    }
    {
        // A synthetic pre-claim checkpoint exercises journal recovery/transport;
        // the client reward contents are verified separately in real-client runs.
        ClashProfile p;p.npcWins=8;p.npcLosses=1;p.chests[0]=19000000;
        auto home=ClashFixtures(cfg.bundlePath).home(p);
        const std::string marker="Offline Player";
        auto name=std::search(home.begin(),home.end(),marker.begin(),marker.end());
        if(name==home.end())throw std::runtime_error("fixture avatar not found");
        auto stored=Plist::parseFile(saved.string());
        stored->set("nativeHome",Plist::Value::makeData(std::string(home.begin(),home.end())));
        stored->set("avatarOffset",Plist::Value::makeInt(name-home.begin()-10));
        const Bytes pending{0x95,3,0,1,1,0x7f,0x7f,0x7f,0,0};
        stored->set("pendingReward",Plist::Value::makeData(std::string(pending.begin(),pending.end())));
        stored->set("pendingRewardSlot",Plist::Value::makeInt(1));
        stored->set("pendingRewardKind",Plist::Value::makeInt(1));
        auto chests=Plist::Value::makeArray();for(auto chest:p.chests)chests->array.push_back(Plist::Value::makeInt(chest));stored->set("chests",chests);
        std::ofstream(saved)<<Plist::toXml(*stored);
        for(int launch=0;launch<2;++launch) {
            Process process(cfg);ClashOffline service(process);SocketPeer client(service.port());
            Bytes login;clashInt(login,0);clashInt(login,0);clashInt(login,-1);for(int n:{2,0,1306})clashVint(login,n);
            client.send(10101,login);
            check(client.receive().first==20104 && client.receive().first==24101 &&
                  client.receive()==std::pair<uint16_t,Bytes>{24111,pending},"relaunch delivers the identical journaled reward after home");
            client.send(10108,{});check(client.receive()==std::pair<uint16_t,Bytes>{20108,{}},"pending reward is delivered once per connection");
        }
        stored->set("pendingRewardSlot",Plist::Value::makeInt(0));std::ofstream(saved)<<Plist::toXml(*stored);
        bool rejected=false;try{Process process(cfg);ClashOffline service(process);}catch(const std::runtime_error&){rejected=true;}
        check(rejected,"incomplete reward journal is rejected without resetting progress");
    }
    {
        // Independent source-2 checkpoint layout: two timers follow slotted
        // chests, then free-chest presence + LogicChest fields, then crown.
        auto home=ClashFixtures(cfg.bundlePath).home({});
        Bytes timers;for(int n:{288000,0,-1,0,0,-1})clashVint(timers,n);
        const auto at=inspectClashHome(home).chestEnd+timers.size();
        if(home.at(at)!=0)throw std::runtime_error("unexpected synthetic extra chest flags");
        Bytes free{1,19,0,4,0,0,0x7f,0}; // present, ref, new/unclaimed flags, IDs, crown absent
        home.erase(home.begin()+at);home.insert(home.begin()+at,free.begin(),free.end());
        auto stored=Plist::parseFile(saved.string());
        stored->set("nativeHome",Plist::Value::makeData(std::string(home.begin(),home.end())));
        stored->set("nativeAvatar",Plist::Value::makeData(""));
        const std::string marker="Offline Player";auto name=std::search(home.begin(),home.end(),marker.begin(),marker.end());
        stored->set("avatarOffset",Plist::Value::makeInt(name-home.begin()-10));
        // This placeholder envelope tests journal persistence/transport only;
        // actual native randomization/execution is exercised in the real app.
        const Bytes pending{0x95,3,0,2,1,0x7f,0x7f,0x7f,0,0};
        stored->set("pendingReward",Plist::Value::makeData(std::string(pending.begin(),pending.end())));
        stored->set("pendingRewardSlot",Plist::Value::makeInt(0));stored->set("pendingRewardKind",Plist::Value::makeInt(2));
        std::ofstream(saved)<<Plist::toXml(*stored);
        for(int launch=0;launch<2;++launch) {
            Process process(cfg);ClashOffline service(process);SocketPeer client(service.port());
            Bytes login;clashInt(login,0);clashInt(login,0);clashInt(login,-1);for(int n:{2,0,1306})clashVint(login,n);
            client.send(10101,login);client.receive();client.receive();
            check(client.receive()==std::pair<uint16_t,Bytes>{24111,pending},"free reward journal replays immutable bytes from an unclaimed checkpoint");
            client.send(10108,{});check(client.receive()==std::pair<uint16_t,Bytes>{20108,{}},"free reward is sent once per connection");
        }
        home.at(at+3)|=2;
        stored->set("nativeHome",Plist::Value::makeData(std::string(home.begin(),home.end())));std::ofstream(saved)<<Plist::toXml(*stored);
        bool rejected=false;try{Process process(cfg);ClashOffline service(process);}catch(const std::runtime_error&){rejected=true;}
        check(rejected,"free journal rejects an already-claimed source checkpoint");
    }
    {
        // The real NPC battle can omit its log. Banking must follow its
        // qualified win, not depend on the optional log's chest getter.
        put("csv_logic/arenas.csv","Name,Arena,ChestRewardMultiplier\nString,int,int\nTrainingCamp,0,100\n");
        put("csv_logic/treasure_chests.csv","Name,MinGold,MaxGold\nString,int,int\nFixtureChest,16,16\n");
        put("csv_logic/chest_order.csv","Name,Chest\nString,String\nNewbie1,FixtureChest\n");
        cfg.homePath=(root/"csv-home").string();
        Process process(cfg);ClashOffline service(process);
        auto avatar=process.heap().allocZeroed(256);process.mem().write<int32_t>(avatar+0x14,1);
        Hydrogen::CPU::Cpu cpu;cpu.r[0]=avatar;cpu.r[1]=1;cpu.r[Hydrogen::CPU::kLR]=0x14c7d;
        process.engine().notifyEntry(cpu,0x9ae94);process.engine().notifyEntry(cpu,0x9ae94);
        const auto state=Plist::parseFile((fs::path(cfg.homePath)/"Library/Application Support/HydrogenVM/ClashOffline.plist").string());
        check(cpu.exit==Hydrogen::CPU::Exit::None && state->get("npcWins")->i==1 && state->get("lastRewardedWin")->i==1 &&
              state->get("chestCycleIndex")->i==1 && state->get("chests")->array[0]->i==19000000 && state->get("chests")->array[1]->i==0,
              "native win without battle log banks the CSV chest exactly once");
    }
    {
        std::string npcs="Name,Location,PredefinedDeck,Seed,StartingMana,WizardHpMultiplier,ExpLevel\nString,String,String,int,int,int,int\n";
        for(int i=1;i<=7;++i)npcs+="Npc"+std::to_string(i)+",FirstMatch,BasicDeck1,2,1,40,1\n";
        put("csv_logic/npcs.csv",npcs);cfg.homePath=(root/"seventh-mission").string();
        Process process(cfg);ClashOffline service(process);SocketPeer client(service.port());
        Bytes login;clashInt(login,0);clashInt(login,0);clashInt(login,-1);for(int n:{2,0,1306})clashVint(login,n);
        client.send(10101,login);client.receive();client.receive();client.send(14104,{18,6});
        auto packet=client.receive();const auto expected=ClashFixtures(cfg.bundlePath,18000006).sector(ClashProfile{});
        Bytes decoded(expected.size()+1024);uLongf size=decoded.size();
        bool valid=packet.first==21903 && packet.second.size()>5 &&
            ::uncompress(decoded.data(),&size,packet.second.data()+5,packet.second.size()-5)==Z_OK;
        decoded.resize(size);
        check(valid && decoded==expected,"seventh training mission keeps its bundled location and training arena");
    }
    {
        put("csv_logic/arenas.csv","Name,Arena,ChestRewardMultiplier,MatchCost,PvpLocation\nString,int,int,int,String\nTrainingCamp,0,100,0,FirstMatch\nArena1,1,100,1,PvP_goblin\n");
        put("csv_logic/locations.csv","Name,FileName\nString,String\nFirstMatch,locations/training_arena.csv\nPvP_goblin,locations/training_arena.csv\n");
        std::string npcs="Name,Location,PredefinedDeck,Seed,StartingMana,WizardHpMultiplier,ExpLevel,Trophies\nString,String,String,int,int,int,int,int\n";
        for(int i=1;i<=8;++i)npcs+="Npc"+std::to_string(i)+",FirstMatch,BasicDeck1,2,1,100,1,"+(i==8?std::string("50"):std::string("0"))+"\n";
        put("csv_logic/npcs.csv",npcs);cfg.homePath=(root/"match-cost").string();
        {Process process(cfg);ClashOffline service(process);}
        const auto path=fs::path(cfg.homePath)/"Library/Application Support/HydrogenVM/ClashOffline.plist";
        auto stored=Plist::parseFile(path.string());ClashProfile p;p.npcWins=p.lastRewardedWin=7;p.arenaLevel=1;
        const auto home=ClashFixtures(cfg.bundlePath).home(p);const std::string marker="Offline Player";
        const auto name=std::search(home.begin(),home.end(),marker.begin(),marker.end());
        const Bytes avatar(name-10,home.end()-1);
        stored->set("npcWins",Plist::Value::makeInt(7));stored->set("lastRewardedWin",Plist::Value::makeInt(7));
        stored->set("arenaLevel",Plist::Value::makeInt(1));stored->set("nativeAvatar",Plist::Value::makeData(std::string(avatar.begin(),avatar.end())));
        std::ofstream(path)<<Plist::toXml(*stored);
        auto gold=[&](){const auto saved=Plist::parseFile(path.string());const auto& data=saved->get("nativeAvatar")->s;return inspectClashAvatar(Bytes(data.begin(),data.end())).gold;};
        for(int launch=0;launch<2;++launch) {
            Process process(cfg);ClashOffline service(process);SocketPeer client(service.port());
            Bytes login;clashInt(login,0);clashInt(login,0);clashInt(login,-1);for(int n:{2,0,1306})clashVint(login,n);
            client.send(10101,login);client.receive();client.receive();
            check(gold()==100-launch,"login/relaunch does not repeat an arena entry charge");
            Bytes turn;for(int n:{10,0,1,537,-1,-1,0,1,0})clashVint(turn,n);turn.push_back(0);clashInt(turn,-1);
            client.send(14102,turn);check(client.receive().first==21903 && gold()==99-launch,"validated arena sector commits its CSV entry fee before delivery");
            client.send(14102,turn);client.send(10108,{});
            check(client.receive().first==20108 && gold()==99-launch,"duplicate pending matchmaking neither sends another sector nor charges again");
            client.send(14101,{0});check(client.receive().first==24101 && gold()==99-launch,"returning home retains the committed entry fee");
        }
    }
    fs::remove_all(root);return failed?1:0;
}
