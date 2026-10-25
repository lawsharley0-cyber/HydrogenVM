#include "ClashState.hpp"
#include <algorithm>
#include <array>
#include <charconv>
#include <filesystem>
#include <fstream>
#include <map>
#include <regex>
#include <stdexcept>

namespace Hydrogen::Runtime::Offline {
namespace {
using Row=std::vector<std::string>;
using Record=std::map<std::string,std::string>;
std::string read(const std::filesystem::path& path) {
    std::ifstream file(path,std::ios::binary|std::ios::ate);
    if(!file || file.tellg()<0 || file.tellg()>4*1024*1024) throw std::runtime_error("Offline Clash: missing/oversized resource "+path.filename().string());
    std::string text(size_t(file.tellg()),'\0'); file.seekg(0);file.read(text.data(),text.size());
    if(!file) throw std::runtime_error("Offline Clash: resource read failed");
    if(text.starts_with("\xef\xbb\xbf")) text.erase(0,3);
    return text;
}
int32_t number(const std::string& s) {
    int32_t value=0;auto [end,error]=std::from_chars(s.data(),s.data()+s.size(),value);
    if(error!=std::errc{} || end!=s.data()+s.size()) throw std::runtime_error("Offline Clash: invalid numeric field");
    return value;
}
std::vector<Row> csv(const std::string& text) {
    std::vector<Row> rows;Row row;std::string field;bool quoted=false;
    for(size_t i=0;i<text.size();++i) {
        char c=text[i];
        if(c=='"') {if(quoted && i+1<text.size() && text[i+1]=='"') {field+='"';++i;} else quoted=!quoted;}
        else if(!quoted && (c==',' || c=='\n')) {row.push_back(field);field.clear();if(c=='\n'){rows.push_back(row);row.clear();}}
        else if(c!='\r' || quoted) field+=c;
    }
    if(quoted) throw std::runtime_error("Offline Clash: unterminated CSV quote");
    if(!field.empty() || !row.empty()) {row.push_back(field);rows.push_back(row);}
    return rows;
}
std::vector<Record> table(const std::filesystem::path& res,const std::string& name) {
    auto rows=csv(read(res/"csv_logic"/(name+".csv")));
    if(rows.size()<3) throw std::runtime_error("Offline Clash: empty table");
    std::vector<Record> result;
    for(size_t i=2;i<rows.size();++i) {Record r;for(size_t j=0;j<rows[0].size();++j)r[rows[0][j]]=j<rows[i].size()?rows[i][j]:"";result.push_back(r);}
    return result;
}
int32_t jsonNumber(const std::string& object,const std::string& key) {
    std::smatch match;
    if(!std::regex_search(object,match,std::regex("\""+key+"\"\\s*:\\s*(-?[0-9]+)"))) throw std::runtime_error("Offline Clash: missing starter field "+key);
    return number(match[1]);
}
struct Writer {
    Bytes data;int booleanIndex=-1;uint8_t bit=0;
    void v(int32_t n) {booleanIndex=-1;clashVint(data,n);}
    void i(int32_t n) {booleanIndex=-1;clashInt(data,n);}
    void s(const std::string& s) {booleanIndex=-1;clashString(data,s);}
    void b(bool value) {if(booleanIndex<0 || bit==8){booleanIndex=int(data.size());data.push_back(0);bit=0;}if(value)data[booleanIndex]|=1u<<bit;++bit;}
    void ref(int32_t n) {v(n/1000000);if(n)v(n%1000000);}
    void timer(int ticks=0) {v(ticks);v(0);v(-1);}
    void zeros(int count) {while(count--)v(0);}
};
struct Cursor {
    const Bytes& data;size_t at=0;int bits=0;uint8_t packed=0;
    uint8_t byte(){bits=0;if(at>=data.size())throw std::runtime_error("Offline Clash: truncated native snapshot");return data[at++];}
    int32_t v(){uint8_t first=byte(),b=first;int shift=6;int64_t n=first&63;for(int i=1;b&128;++i){b=byte();if(i>4 || (i==4 && b>15))throw std::runtime_error("Offline Clash: invalid snapshot VInt");n|=int64_t(b&127)<<shift;shift+=7;}if(first&64)n|=-(int64_t(1)<<std::min(shift,31));return int32_t(n);}
    bool b(){if(!bits){packed=byte();bits=8;}bool value=packed&1;packed>>=1;--bits;return value;}
    int32_t ref(){int type=v();if(!type)return 0;int index=v();if(type<1 || type>999 || index<0 || index>=1000000)throw std::runtime_error("Offline Clash: invalid snapshot reference");return type*1000000+index;}
    ClashFixtures::Card card(){ClashFixtures::Card c{ref(),v(),v(),v(),v()};v();b();b();return c;}
};
void deck(Writer& w,const std::vector<ClashFixtures::Card>& cards) {
    for(int i=0;i<8;++i)w.b(size_t(i)<cards.size());
    for(auto& c:cards){w.ref(c.data);w.v(c.level);w.v(c.serial);w.v(c.count);w.v(c.timer);w.v(0);w.b(false);w.b(false);}
}
}
std::vector<ClashHomeCommand> decodeClashHomeTurn(const Bytes& bytes) {
    Cursor r{bytes};
    const int32_t tick=r.v();r.v(); // checksum is signed
    const int32_t count=r.v();
    if(tick<0 || count<0 || count>64)throw std::runtime_error("Offline Clash: invalid home turn tick/count");
    std::vector<ClashHomeCommand> commands;
    for(int32_t i=0;i<count;++i) {
        ClashHomeCommand c;c.type=r.v();
        // Native getCommandType: StartExploring 0x10f7c8 and
        // SpeedUpExploring 0x17a40. Payloads follow their native decode methods.
        if(c.type!=500 && c.type!=502 && c.type!=503 && c.type!=504 && c.type!=506 && c.type!=518 && c.type!=525 && c.type!=537 && c.type!=538)
            throw std::runtime_error("Offline Clash: unsupported turn command "+std::to_string(c.type));
        r.v();r.v(); // LogicCommand::decode (0x639a0): given tick, sequence
        const int32_t high=r.v(),low=r.v();
        if(high!=0 || low!=1)throw std::runtime_error("Offline Clash: home command executor mismatch");
        if(c.type==500) {
            // LogicSwapSpellsCommand::decode 0x77a88: native execution has
            // already changed the deck; only qualify this echo's boundaries.
            r.v();r.v();r.b();r.b();
        } else if(c.type==525) {
            // decodeSpellList 0xdc688 asserts count < 200.
            const auto spells=r.v();
            if(spells<0 || spells>=200)throw std::runtime_error("Offline Clash: invalid opened spell list");
            for(int j=0;j<spells;++j)r.ref();
        } else if(c.type!=518 && c.type!=538) {
            c.argument=c.type==504?r.ref():r.v();
            if(c.type==506 || c.type==537)c.flag=r.b();
        } // CollectFreeChest/RefreshAchievements have only base metadata.
        if((c.type==502 || c.type==503 || c.type==506) && c.argument<0)
            throw std::runtime_error("Offline Clash: invalid chest ID");
        commands.push_back(c);
    }
    // ClientEndTurn carries a fixed big-endian -1 when debug data is absent.
    for(int i=0;i<4;++i)if(r.byte()!=255)throw std::runtime_error("Offline Clash: unsupported turn debug trailer");
    if(r.at!=bytes.size())throw std::runtime_error("Offline Clash: trailing turn data");
    return commands;
}
ClashHomeView inspectClashHome(const Bytes& bytes) {
    Cursor r{bytes};for(int i=0;i<8;++i)r.byte();
    ClashHomeView view;view.lastChestId=r.v();view.chestCounterEnd=r.at;
    if(view.lastChestId<0)throw std::runtime_error("Offline Clash: negative native chest counter");
    for(int i=0;i<5;++i)r.v();
    const int decks=r.v();if(decks<1 || decks>3)throw std::runtime_error("Offline Clash: invalid native decks");
    std::vector<std::vector<ClashFixtures::Card>> all;
    for(int i=0;i<decks;++i){std::array<bool,8> present{};for(auto&& p:present)p=r.b();std::vector<ClashFixtures::Card> cards;for(bool p:present)if(p)cards.push_back(r.card());all.push_back(cards);}
    const int count=r.v();if(count<0 || count>10000)throw std::runtime_error("Offline Clash: invalid native collection");
    for(int i=0;i<count;++i)r.card();r.v();const int selected=r.v();const int slots=r.v();
    if(selected<0 || selected>=decks || slots!=4)throw std::runtime_error("Offline Clash: invalid native home selection");
    view.deck=all[selected];view.chestOffset=r.at;
    for(int i=0;i<4;++i)if(r.b()) {auto& c=view.states[i];c.data=r.ref();view.chests[i]=c.data;c.flagOffset=r.at;c.unlocked=r.b();c.claimed=r.b();c.isNew=r.b();c.hasTimer=r.b();if(c.hasTimer)for(auto& n:c.timer)n=r.v();c.value8=r.v();c.source=r.v();c.valueC=r.v();}
    view.chestEnd=r.at;
    for(int i=0;i<6;++i)r.v(); // timers at home+0x60/+0x98
    auto extraChest=[&](ClashHomeView::Chest& c) {
        if(!r.b())return;
        c.data=r.ref();c.unlocked=r.b();c.claimed=r.b();c.isNew=r.b();c.hasTimer=r.b();
        if(c.hasTimer)for(auto& n:c.timer)n=r.v();c.value8=r.v();c.source=r.v();c.valueC=r.v();
    };
    extraChest(view.freeChest);extraChest(view.spareFreeChest); // +0x78/+0x7c
    view.crownCount=r.v();view.crownCooldown=r.b(); // +0x8c/+0x94
    for(int i=0;i<3;++i)r.v(); // crown cooldown at +0x80
    extraChest(view.crownChest);return view; // +0x90
}
Bytes unlockedClashRewardCheckpoint(const Bytes& bytes,int32_t slot,int32_t chestId) {
    const auto view=inspectClashHome(bytes);
    if(slot<0 || slot>=4)throw std::runtime_error("Offline Clash: invalid paid reward slot");
    const auto& chest=view.states[slot];
    if(!chest.data || chest.value8!=chestId || !chest.unlocked || !chest.claimed)
        throw std::runtime_error("Offline Clash: paid claim did not complete native unlock");
    // Only the current, validated native speed-up transaction reaches here.
    // Persist its actual cost/timers and unlocked chest, immediately before the
    // still-undelivered grant. This is not recovery of arbitrary old claim flags.
    auto result=bytes;result.at(chest.flagOffset)&=uint8_t(~2u);return result;
}
Bytes replaceClashHomeChests(const Bytes& bytes,const std::array<int32_t,4>& chests,size_t& avatarOffset) {
    auto view=inspectClashHome(bytes);if(view.chests==chests)return bytes;
    if(avatarOffset<view.chestEnd)throw std::runtime_error("Offline Clash: avatar overlaps the chest list");
    // Preserve native timer/unlock state when the slot is unchanged. This path
    // only appends newly earned rewards to empty slots, never rewrites a chest.
    int32_t lastId=view.lastChestId;
    for(const auto& c:view.states)if(c.data)lastId=std::max(lastId,c.value8);
    Writer w;
    for(size_t i=0;i<4;++i) {
        auto c=view.states[i];const auto chest=chests[i];w.b(chest!=0);
        if(!chest)continue;
        if(c.data!=chest) {
            if(c.data)throw std::runtime_error("Offline Clash: cannot replace an occupied native chest slot");
            if(lastId==INT32_MAX)throw std::runtime_error("Offline Clash: native chest ID exhausted");
            c=ClashHomeView::Chest{};c.data=chest;c.value8=++lastId;c.source=1;
        }
        w.ref(c.data);w.b(c.unlocked);w.b(c.claimed);w.b(c.isNew);w.b(c.hasTimer);
        if(c.hasTimer)for(auto n:c.timer)w.v(n);w.v(c.value8);w.v(c.source);w.v(c.valueC);
    }
    Bytes counter;clashVint(counter,lastId);
    Bytes result(bytes.begin(),bytes.begin()+8);result.insert(result.end(),counter.begin(),counter.end());
    result.insert(result.end(),bytes.begin()+view.chestCounterEnd,bytes.begin()+view.chestOffset);
    result.insert(result.end(),w.data.begin(),w.data.end());result.insert(result.end(),bytes.begin()+view.chestEnd,bytes.end());
    avatarOffset=avatarOffset-view.chestEnd+view.chestOffset+w.data.size()-(view.chestCounterEnd-8)+counter.size();return result;
}
namespace {
void avatar(Writer& w,const ClashProfile& profile,bool npc,const std::string& trainer="Trainer Earl",int npcArena=0,int npcScore=0,int npcLevel=1) {
    for(int i=0;i<3;++i){w.v(npc?-1:0);w.v(npc?0:1);}
    w.s(npc?trainer:"Offline Player");w.b(false);w.v(0);
    w.ref(54000000 + (npc ? npcArena : profile.arenaLevel));
    w.v(npc ? npcScore : profile.trophies);
    w.b(false);
    w.v(6);w.v(2);w.ref(5000001);w.v(100);w.ref(5000005);w.v(100);w.zeros(5);
    w.v(100);w.v(100);w.v(0);w.v(npc?npcLevel:1);w.v(0);w.b(false);
    w.zeros(4);w.v(npc?0:profile.npcWins);w.v(npc?0:profile.npcLosses);w.b(false);
}
}
ClashAvatarView inspectClashAvatar(const Bytes& bytes) {
    Cursor r{bytes};for(int i=0;i<6;++i)r.v();
    uint32_t length=0;for(int i=0;i<4;++i)length=(length<<8)|r.byte();
    if(length>bytes.size()-r.at)throw std::runtime_error("Offline Clash: invalid native avatar name");
    r.at+=length;r.b();r.v();const auto arena=r.ref(),score=r.v();
    if(arena<54000000 || arena>54000020 || score<0 || score>100000)
        throw std::runtime_error("Offline Clash: invalid native avatar score/arena");
    r.b();const auto slots=r.v();
    if(slots<0 || slots>16)throw std::runtime_error("Offline Clash: invalid avatar resource slots");
    int32_t chestCycle=0,pendingCrowns=0;
    for(int i=0;i<slots;++i) {
        const auto count=r.v();if(count<0 || count>128)throw std::runtime_error("Offline Clash: invalid avatar resource count");
        for(int j=0;j<count;++j){const auto data=r.ref(),value=r.v();
            if(i==0 && data==5000002)chestCycle=value;
            if(i==0 && data==5000004)pendingCrowns=value;
        }
    }
    r.v();r.v();r.v();const auto level=r.v();
    if(level<1 || level>13)throw std::runtime_error("Offline Clash: invalid avatar experience level");
    return {arena-54000000,score,level,chestCycle,pendingCrowns};
}
int32_t clashElapsedHomeSeconds(int64_t savedAt,int64_t now) {
    if(savedAt<=0 || now<=savedAt)return 0;
    // All bundled home cooldowns fit within 30 days. Bound the original
    // seconds-to-ticks multiplication and don't invent pre-migration elapsed time.
    return int32_t(std::min<int64_t>(now-savedAt,30*24*60*60));
}
Bytes withClashBattleProgress(const Bytes& bytes,int32_t chestCycle,int32_t crowns) {
    const auto view=inspectClashAvatar(bytes);
    if(view.chestCycleIndex<0 || chestCycle<0 || chestCycle<view.chestCycleIndex || chestCycle>1000000 || crowns<0 || crowns>3 ||
       view.pendingCrowns<0 || view.pendingCrowns>3 || view.pendingCrowns>3-crowns)
        throw std::runtime_error("Offline Clash: inconsistent earned avatar ledger");
    Cursor r{bytes};for(int i=0;i<6;++i)r.v();
    uint32_t length=0;for(int i=0;i<4;++i)length=(length<<8)|r.byte();r.at+=length;
    r.b();r.v();r.ref();r.v();r.b();
    if(r.v()<1)throw std::runtime_error("Offline Clash: avatar has no resource slot");
    const auto begin=r.at;const auto count=r.v();std::vector<std::pair<int32_t,int32_t>> resources;
    for(int i=0;i<count;++i) {
        const auto data=r.ref(),value=r.v();
        if(std::any_of(resources.begin(),resources.end(),[&](auto entry){return entry.first==data;}))
            throw std::runtime_error("Offline Clash: duplicate native resource");
        resources.emplace_back(data,value);
    }
    auto set=[&](int32_t data,int32_t value) {
        for(auto& resource:resources)if(resource.first==data){resource.second=value;return;}
        if(value)resources.emplace_back(data,value);
    };
    set(5000002,chestCycle);if(crowns)set(5000004,view.pendingCrowns+crowns);
    Writer w;w.v(int(resources.size()));for(auto [data,value]:resources){w.ref(data);w.v(value);}
    Bytes result(bytes.begin(),bytes.begin()+begin);result.insert(result.end(),w.data.begin(),w.data.end());
    result.insert(result.end(),bytes.begin()+r.at,bytes.end());return result;
}
void clashVint(Bytes& out,int32_t value) {
    const uint64_t magnitude=value<0?uint64_t(-int64_t(value)):uint64_t(value);
    int size=1;for(int bits:{6,13,20,27}){if(magnitude<(uint64_t(1)<<bits))break;++size;}
    const uint32_t encoded=uint32_t(value)&0x7fffffff;
    out.push_back((encoded&63)|(value<0?64:0)|(size>1?128:0));
    for(int i=1;i<size;++i)out.push_back(((encoded>>(6+(i-1)*7))&(i==4?15:127))|(i<size-1?128:0));
}
void clashInt(Bytes& out,int32_t value){for(int shift:{24,16,8,0})out.push_back(uint32_t(value)>>shift);}
void clashString(Bytes& out,const std::string& value){if(value.size()>1048576)throw std::runtime_error("Offline Clash: oversized string");clashInt(out,int32_t(value.size()));out.insert(out.end(),value.begin(),value.end());}

ClashFixtures::ClashFixtures(const std::string& bundle,int32_t npcId,int32_t arenaLevel):mission(npcId),arena(54000000+arenaLevel) {
    if(npcId<18000000 || npcId>18000019)throw std::runtime_error("Offline Clash: only the twenty original training/PvP missions are implemented");
    const auto res=std::filesystem::path(bundle)/"res";
    const auto initial=read(res/"level"/"starting_home.json");
    // The version pin limits this to the shipped, flat starter-card objects.
    // This is not a general JSON parser or a save-file reader.
    const std::regex object("\\{[^{}]*\"d\"\\s*:[^{}]*\\}");
    for(std::sregex_iterator it(initial.begin(),initial.end(),object),end;it!=end;++it) {
        auto text=it->str();Card card{jsonNumber(text,"d"),jsonNumber(text,"l"),jsonNumber(text,"s"),jsonNumber(text,"c"),jsonNumber(text,"t")};
        if(card.data/1000000<26 || card.data/1000000>28 || card.level!=0 || card.serial!=0 || card.timer!=0 || card.count!=1)throw std::runtime_error("Offline Clash: unsupported starter deck");
        playerDeck.push_back(card);
    }
    if(playerDeck.size()!=6 || jsonNumber(initial,"selected_deck")!=0 || jsonNumber(initial,"chest_slots")!=4)throw std::runtime_error("Offline Clash: unsupported initial home");
    freeChestTicks=jsonNumber(initial,"ticks");
    auto npcs=table(res,"npcs");
    auto npc=npcs.end();int npcIndex=0;
    for(auto it=npcs.begin();it!=npcs.end();++it)if(!(*it)["Name"].empty() && npcIndex++==npcId-18000000){npc=it;break;}
    if(npc==npcs.end())throw std::runtime_error("Offline Clash: unsupported training mission");
    const int32_t expLvl = number((*npc)["ExpLevel"]);
    if(expLvl<1 || expLvl>13)throw std::runtime_error("Offline Clash: unsupported npc exp level");
    npcExpLevel=expLvl;
    npcTrophies=(*npc)["Trophies"].empty()?0:number((*npc)["Trophies"]);
    if(!(*npc)["WizardHpMultiplier"].empty())npcTowerHpPercent=number((*npc)["WizardHpMultiplier"]);
    if(!npcTowerHpPercent)npcTowerHpPercent=100; // LogicNpcData::createReferences 0x100416
    if(std::filesystem::exists(res/"csv_logic/exp_levels.csv")) {
        for(auto& row:table(res,"exp_levels"))if(!row["Name"].empty()) {
            const auto level=number(row["Name"]);
            if(level>=1 && level<=13){kingLevels[level-1]=number(row["SummonerLevel"])-1;towerLevels[level-1]=number(row["TowerLevel"])-1;}
        }
        for(auto& row:table(res,"globals")) {
            if(row["Name"]=="HITPOINT_INCREASE_PERCENT_PER_KING_LEVEL")kingHpGrowth=number(row["NumberValue"]);
            if(row["Name"]=="HITPOINT_INCREASE_PERCENT_PER_TOWER_LEVEL")towerHpGrowth=number(row["NumberValue"]);
        }
    }
    seed=(*npc)["Seed"].empty()?2:number((*npc)["Seed"]);
    npcMana=(*npc)["StartingMana"].empty()?5:number((*npc)["StartingMana"]);
    int locationIndex=0;bool foundLocation=false;std::string locationFile;
    std::string targetLocation = (*npc)["Location"];
    if(arenaLevel > 0) {
        int arenaIndex = 0;
        for(auto& r:table(res,"arenas")) if(!r["Name"].empty()) {
            int curArena = r["Arena"].empty()?arenaIndex:number(r["Arena"]);
            if(curArena == arenaLevel && !r["PvpLocation"].empty()) {
                targetLocation = r["PvpLocation"];
                break;
            }
            ++arenaIndex;
        }
    }
    for(auto& r:table(res,"locations"))if(!r["Name"].empty()) {
        if(r["Name"]==targetLocation){locationFile=r["FileName"];location=15000000+locationIndex;foundLocation=true;}
        ++locationIndex;
    }
    if(!foundLocation || locationFile.empty())throw std::runtime_error("Offline Clash: missing training location");
    tutorial=0;tutorialWait=0;int tutorialIndex=0;
    if(arenaLevel == 0) {
        for(auto& r:table(res,"tutorials_npc"))if(!r["Name"].empty()) {
            if(!tutorial && r["NPC"]==(*npc)["Name"] && r["Location"]==(*npc)["Location"]){tutorial=48000000+tutorialIndex;tutorialWait=r["WaitTimeMS"].empty()?0:number(r["WaitTimeMS"]);}
            ++tutorialIndex;
        }
    }
    for(auto& r:csv(read(res/"csv_client"/"texts.csv")))if(r.size()>1 && r[0]==(*npc)["TID"])trainerName=r[1];
    std::map<std::string,int32_t> spells;
    for(auto [id,name]:{std::pair{26,"spells_characters"},std::pair{27,"spells_buildings"},std::pair{28,"spells_other"}}) {
        int index=0;for(auto& r:table(res,name))if(!r["Name"].empty())spells[r["Name"]]=id*1000000+index++;
    }
    bool selected=false;
    for(auto& r:table(res,"predefined_decks")) {
        if(!r["Name"].empty())selected=r["Name"]==(*npc)["PredefinedDeck"];
        if(selected && !r["Spells"].empty())npcDeck.push_back({spells.at(r["Spells"]),number(r["SpellLevel"])-1,0,0,0});
    }
    std::map<std::string,std::pair<int32_t,int32_t>> buildings;int index=0;
    for(auto& r:table(res,"buildings"))if(!r["Name"].empty()) {
        const auto id=35000000+index++;
        if(r["Name"]=="KingTower" || r["Name"]=="PrincessTower")buildings[r["Name"]]={id,number(r["Hitpoints"])};
    }
    std::map<std::string,int32_t> decos;index=0;
    if(std::filesystem::exists(res/"csv_logic/decos.csv"))
        for(auto& row:table(res,"decos"))if(!row["Name"].empty())decos[row["Name"]]=42000000+index++;
    bool objects=false;std::string group;
    for(auto& r:csv(read(res/locationFile))) {
        if(!r.empty() && r[0]=="Objects"){objects=true;continue;}
        if(!objects || r.size()<4)continue;
        if(!r[1].empty())group=r[1];
        if(r[2].empty() || r[2].find_first_not_of("-0123456789")!=std::string::npos)continue;
        if(auto deco=decos.find(group);deco!=decos.end()){decoData.push_back(deco->second);continue;}
        if(group!="KingTower" && group!="PrincessTower")continue;
        int32_t x=number(r[2]),y=number(r[3]);auto [data,hp]=buildings.at(group);const int owner=y<32?0:1;
        towers.push_back({data,owner,x*500,y*500,hp,group=="KingTower"});
    }
    for(auto& r:table(res,"damage_types"))if(!r["Name"].empty())++damageTypes;
    if(towers.size()!=6 || npcDeck.empty() || npcDeck.size()>8 || damageTypes!=5)throw std::runtime_error("Offline Clash: unsupported initial topology");
}
Bytes ClashFixtures::home(const ClashProfile& p) const {
    if(!p.nativeHome.empty()) {
        auto offset=p.avatarOffset;auto result=replaceClashHomeChests(p.nativeHome,p.chests,offset);
        if(!p.nativeAvatar.empty()){if(offset>=result.size())throw std::runtime_error("Offline Clash: invalid native avatar offset");result.resize(offset);result.insert(result.end(),p.nativeAvatar.begin(),p.nativeAvatar.end());result.push_back(0);}
        return result;
    }
    int32_t lastChestId=0;for(int i=0;i<4;++i)if(p.chests[i])lastChestId=i+1;
    Writer w;w.i(0);w.i(1);w.v(lastChestId);w.v(0);w.timer();w.v(0);w.v(1);deck(w,playerDeck);
    w.v(0);w.v(1);w.v(0);w.v(4);
    for(int i=0;i<4;++i) {
        const bool present=p.chests[i]!=0;w.b(present);
        if(present){w.ref(p.chests[i]);w.b(false);w.b(false);w.b(true);w.b(false);w.v(i+1);w.v(1);w.v(-1);}
    }
    w.timer(freeChestTicks);w.timer();w.b(false);w.b(false);w.v(0);w.b(true);w.timer();w.b(false);w.timer();
    w.zeros(9);w.v(1);w.ref(0);w.v(-1);w.v(-1);w.v(-1);w.timer();w.v(0);w.timer();w.timer();w.zeros(6);w.v(0);w.s("");w.v(0);
    avatar(w,p,false);w.v(0);return w.data;
}
std::pair<int32_t,int32_t> ClashFixtures::towerStats(const Tower& t,int32_t expLevel) const {
    if(expLevel<1 || expLevel>13)throw std::runtime_error("Offline Clash: tower experience level outside supported range");
    const auto level=(t.king?kingLevels:towerLevels)[expLevel-1],growth=t.king?kingHpGrowth:towerHpGrowth;
    if(level<0 || level>12 || (expLevel>1 && (!level || growth<=0 || growth>100)))
        throw std::runtime_error("Offline Clash: missing tower scaling data");
    // Native getScaledValue 0x74ee8: accumulate an integer percentage at
    // each level, then scale the original design value. NPC multiplier uses
    // ceil division in LogicCharacter::setLevelIndex, 0x12d330..0x12d35e.
    int64_t percent=100;for(int i=0;i<level;++i)percent=percent*(100+growth)/100;
    int64_t hp=int64_t(t.hp)*percent/100;
    if(t.owner==0)hp=(hp*npcTowerHpPercent+99)/100;
    if(hp<1 || hp>INT32_MAX)throw std::runtime_error("Offline Clash: invalid scaled tower hitpoints");
    return {level,int32_t(hp)};
}
Bytes ClashFixtures::sector(const ClashProfile& p) const {
    const auto player=p.nativeHome.empty()?playerDeck:inspectClashHome(p.nativeHome).deck;
    const auto playerLevel=p.nativeAvatar.empty()?1:inspectClashAvatar(p.nativeAvatar).expLevel;
    Writer w;w.v(0);w.v(0);w.v(11);w.v(0);w.i(seed);w.v(0);
    w.ref(location);w.ref(mission);w.ref(arena);w.v(-1);w.v(0);w.v(0);w.v(1);
    w.zeros(10);w.v(1);w.zeros(2);for(int i=0;i<6;++i)w.b(false);
    // setLocation/addDecos already created these static objects from the CSV.
    // A full update's ID list must retain them, or Manager::decode deletes them.
    // LogicDeco::decode is empty; their original row/positions remain native.
    w.v(int(decoData.size()));w.zeros(4);w.v(6);w.v(int(decoData.size())+6);
    for(auto data:decoData)w.ref(data);for(auto& t:towers)w.ref(t.data);
    for(int i=0;i<int(decoData.size());++i)w.ref(1000000+i);
    for(int i=0;i<6;++i)w.ref(6000000+i);
    for(auto& t:towers) {
        w.v(towerStats(t,t.owner==0?npcExpLevel:playerLevel).first);w.b(false);w.b(false);w.v(t.owner);w.v(13);w.v(t.x);w.v(t.y);w.v(0);
        // Original spawnObject 0x13fe8..0x13ffc initializes opposing normalized
        // directions. A zero vector makes both sides render the same angle.
        w.v(0);w.v(t.owner==0?256:-256);
        w.v(0);w.v(100);w.zeros(10);
        if(t.king){const auto& cards=t.owner==0?npcDeck:player;const int hand=std::min<int>(4,cards.size());
            w.b(true);w.v(hand);for(int i=0;i<4;++i)w.v(i<hand?i:-1);
            w.v(int(cards.size())-hand);for(int i=hand;i<int(cards.size());++i)w.v(i);
            w.v(0);w.v(-1);w.zeros(2);w.v(0);w.v(t.owner==1?5:npcMana);w.zeros(2);}
    }
    for(auto& t:towers){(void)t;w.b(false);w.b(false);w.zeros(7);w.ref(0);}
    for(auto& t:towers){const auto hp=towerStats(t,t.owner==0?npcExpLevel:playerLevel).second;w.v(hp);w.v(hp);w.v(0);}
    for(auto& t:towers){
        (void)t;w.zeros(5+damageTypes);for(int i=0;i<damageTypes;++i)w.b(false);
        // Original LogicCharacterBuffComponent constructor: +0x34 damage and
        // +0x44 size are neutral 100%, not zero. Zero size clamps to 1% and
        // corrupts max-HP ratios and tower occupants; zero damage disables fire.
        w.v(100);w.zeros(3);w.v(100);w.v(0);
    }
    w.b(true);deck(w,npcDeck);w.b(true);deck(w,player);
    for(int owner=0;owner<2;++owner)for(int i=0;i<6;++i)if(towers[i].king && towers[i].owner==owner)w.ref(6000000+i);
    for(int owner=0;owner<2;++owner){w.v(2);for(int i=0;i<6;++i)if(!towers[i].king && towers[i].owner==owner)w.ref(6000000+i);}
    w.zeros(6);avatar(w,p,true,trainerName,arena-54000000,npcTrophies,npcExpLevel);
    if(p.nativeAvatar.empty())avatar(w,p,false);else {w.booleanIndex=-1;w.data.insert(w.data.end(),p.nativeAvatar.begin(),p.nativeAvatar.end());}
    if(arena-54000000 > 0 || tutorial == 0) {
        // 12 is the LogicGameMode full-update delimiter (decoder 0xd7a80),
        // not a tutorial flag. Only the following tutorial reference is null.
        w.v(12);w.ref(0);w.v(0);w.ref(0);w.zeros(2);
    } else {
        w.v(12);w.ref(tutorial);w.v(tutorialWait);w.ref(0);w.zeros(2);
    }
    return w.data;
}
}
