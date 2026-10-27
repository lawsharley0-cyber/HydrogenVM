// Build-time only. Generate pinned initial protocol fixtures from user assets;
// no proprietary bytes or player saves belong in the repository.
#include "runtime/offline/ClashState.hpp"
#include <cstdio>
#include <ctime>
#include <algorithm>
#include <stdexcept>
#include <fstream>
#include <zlib.h>
using namespace Hydrogen::Runtime::Offline;
static void emit(const char* name,const Bytes& data) {
    std::printf("static const unsigned char %s[]={",name);
    for(auto b:data)std::printf("%u,",unsigned(b));
    std::printf("};\n");
}
static size_t unique(const Bytes& bytes,const Bytes& pattern) {
    auto at=std::search(bytes.begin(),bytes.end(),pattern.begin(),pattern.end());
    if(pattern.empty() || at==bytes.end() || std::search(at+1,bytes.end(),pattern.begin(),pattern.end())!=bytes.end())
        throw std::runtime_error("Guest fixture field is not unique");
    return size_t(at-bytes.begin());
}
static Bytes cardBytes(const std::vector<ClashFixtures::Card>& cards) {
    Bytes out;
    for(auto c:cards) {
        for(int v:{c.data/1000000,c.data%1000000,c.level,c.serial,c.count,c.timer,0})clashVint(out,v);
        out.push_back(0); // two packed native booleans
    }
    return out;
}
static Bytes handBytes(int count) {
    Bytes out{1};int hand=std::min(4,count);
    clashVint(out,hand);for(int i=0;i<4;++i)clashVint(out,i<hand?i:-1);
    clashVint(out,count-hand);for(int i=hand;i<count;++i)clashVint(out,i);
    for(int v:{0,-1,0,0,0,5,0,0})clashVint(out,v);
    return out;
}
static void sectorTemplate(const std::string& name,const ClashFixtures& f,const ClashProfile& p,const Bytes& avatar,bool opponent=false) {
    const auto sector=f.sector(p),cards=cardBytes(f.playerDeck),hand=handBytes(int(f.playerDeck.size()));
    const size_t avatarAt=unique(sector,avatar),handAt=unique(sector,hand);
    // The NPC card's two final booleans, player-deck-present bit, and eight
    // player slot flags share two bytes. Card payload itself is byte aligned.
    auto cardsAt=std::find_end(sector.begin(),sector.begin()+avatarAt,cards.begin(),cards.end());
    if(cardsAt==sector.begin()+avatarAt || cardsAt-sector.begin()<2)throw std::runtime_error("Missing player deck");
    const size_t deckAt=size_t(cardsAt-sector.begin())-2,deckEnd=deckAt+2+cards.size();
    const unsigned mask=(1u<<f.playerDeck.size())-1;
    if((sector[deckAt]&0xfc)!=((mask<<3|4)&0xfc) || sector[deckAt+1]!=(mask>>5) || handAt+hand.size()>deckAt || deckEnd>avatarAt)
        throw std::runtime_error("Unsupported player deck bit layout");
    emit((name+"_a").c_str(),Bytes(sector.begin(),sector.begin()+handAt));
    emit((name+"_b").c_str(),Bytes(sector.begin()+handAt+hand.size(),sector.begin()+deckAt));
    emit((name+"_c").c_str(),Bytes(sector.begin()+deckEnd,sector.begin()+avatarAt));
    emit((name+"_d").c_str(),Bytes(sector.begin()+avatarAt+avatar.size(),sector.end()));
    struct Patch {size_t offset;std::vector<int> values;};std::vector<Patch> patches;
    Bytes hpPattern;std::vector<size_t> hpOffsets;
    for(const auto& t:f.towers) {
        const int hp=f.towerStats(t,t.owner==0?f.npcExpLevel:1).second;
        hpOffsets.push_back(hpPattern.size());clashVint(hpPattern,hp);hpOffsets.push_back(hpPattern.size());clashVint(hpPattern,hp);clashVint(hpPattern,0);
        if(t.owner!=1)continue;
        Bytes pattern;clashVint(pattern,f.towerStats(t,1).first);pattern.push_back(0);
        for(int n:{t.owner,13,t.x,t.y,0,0,-256,0,100})clashVint(pattern,n);
        pattern.insert(pattern.end(),10,0);
        Patch patch{unique(sector,pattern),{}};
        for(int level=1;level<=12;++level)patch.values.push_back(f.towerStats(t,level).first);
        patches.push_back(patch);
    }
    const auto hpAt=unique(sector,hpPattern);
    for(size_t i=0;i<f.towers.size();++i)if(f.towers[i].owner==1)for(int field=0;field<2;++field) {
        Patch patch{hpAt+hpOffsets[2*i+field],{}};
        for(int level=1;level<=12;++level)patch.values.push_back(f.towerStats(f.towers[i],level).second);
        patches.push_back(patch);
    }
    std::printf("static const struct GuestTowerPatch %s_patches[]={\n",name.c_str());
    for(const auto& patch:patches) {
        Bytes initial;clashVint(initial,patch.values.front());
        int part=patch.offset<handAt?0:1;
        if((part==0 && patch.offset+initial.size()>handAt) || (part==1 && (patch.offset<handAt+hand.size() || patch.offset+initial.size()>deckAt)))
            throw std::runtime_error("Tower patch crosses a variable field");
        if(!std::equal(initial.begin(),initial.end(),sector.begin()+patch.offset))throw std::runtime_error("Tower patch mismatch");
        std::printf("{%d,%zu,%zu,{",part,part?patch.offset-handAt-hand.size():patch.offset,initial.size());
        for(int value:patch.values) {
            Bytes bytes;clashVint(bytes,value);
            if(bytes.size()!=initial.size())throw std::runtime_error("Tower VInt width changed");
            std::printf("%d,",value);
        }
        std::puts("}},");
    }
    std::puts("};");
    std::printf("#define %s_LOW_BITS %uu\n",name.c_str(),sector[deckAt]&7);
    size_t nameOffset=0,nameSize=0;
    if(opponent) {
        Bytes pattern;clashString(pattern,f.trainerName);const auto nameAt=unique(sector,pattern);
        if(nameAt<deckEnd || nameAt+pattern.size()>avatarAt || sector.size()<8 ||
           !std::equal(sector.begin(),sector.begin()+4,Bytes{0,0,11,0}.begin()))
            throw std::runtime_error("Unsupported NPC name/seed layout");
        nameOffset=nameAt-deckEnd;nameSize=pattern.size();
    }
    std::printf("#define %s_NAME_OFFSET %zuu\n#define %s_NAME_SIZE %zuu\n",name.c_str(),nameOffset,name.c_str(),nameSize);
}
#ifdef GUEST_BOT_POLICY_GENERATED
static ClashFixtures opponentFixture(const char *bundle,int variant) {
    if(variant<0 || variant>=3*GUEST_BOT_DECK_COUNT)throw std::runtime_error("Invalid arena opponent variant");
    ClashFixtures bot(bundle,18000007+variant/GUEST_BOT_DECK_COUNT,1);
    if(bot.npcExpLevel!=2)throw std::runtime_error("Unexpected early-arena native tower level");
    bot.npcDeck.clear();int deck=variant%GUEST_BOT_DECK_COUNT;
    for(int i=0;i<8;++i)bot.npcDeck.push_back({guest_bot_ids[deck][i],guest_bot_levels[deck][i],0,0,0});
    bot.trainerName="Guest Opponent";bot.npcMana=5;return bot;
}
#endif
int main(int argc,char** argv) {
#ifdef GUEST_BOT_POLICY_GENERATED
    if(argc==7) {
        std::ifstream input(argv[2],std::ios::binary);uint32_t h[4]{};
        if(!input.read(reinterpret_cast<char*>(h),sizeof h) || h[0]!=0x31475243 || !h[1] || h[1]>65536 || h[2]>=h[1]-1)return 6;
        Bytes body(h[1]);if(!input.read(reinterpret_cast<char*>(body.data()),body.size()))return 6;
        ClashProfile p;p.nativeHome=Bytes(body.begin(),body.begin()+h[2]);p.nativeAvatar=Bytes(body.begin()+h[2],body.end()-1);
        auto bot=opponentFixture(argv[1],std::stoi(argv[3]));bot.seed=std::stoi(argv[4]);bot.trainerName=argv[5];
        auto bytes=bot.sector(p);std::ofstream output(argv[6],std::ios::binary);
        return output.write(reinterpret_cast<const char*>(bytes.data()),bytes.size())?0:6;
    }
#endif
    if(argc==5) {
        // Independent C++ sector oracle for a private guest checkpoint. This
        // mode emits no fixture/player data to stdout or the repository.
        std::ifstream input(argv[2],std::ios::binary);uint32_t header[4]{};
        if(!input.read(reinterpret_cast<char*>(header),sizeof header) || header[0]!=0x31475243 ||
           !header[1] || header[1]>65536 || header[2]>=header[1]-1)return 6;
        Bytes body(header[1]);if(!input.read(reinterpret_cast<char*>(body.data()),body.size()))return 6;
        ClashProfile p;p.nativeHome=Bytes(body.begin(),body.begin()+header[2]);
        p.nativeAvatar=Bytes(body.begin()+header[2],body.end()-1);
        const int mission=std::stoi(argv[3]);if(mission<0 || mission>=20)return 6;
        const auto expected=ClashFixtures(argv[1],18000000+mission,mission>=7?1:0).sector(p);
        std::ofstream output(argv[4],std::ios::binary);
        return output.write(reinterpret_cast<const char*>(expected.data()),expected.size())?0:6;
    }
    if(argc!=2)return 2;
#ifndef GUEST_BOT_POLICY_GENERATED
    throw std::runtime_error("Generate validated opponent policy before exporting game data");
#endif
    ClashProfile p;p.created=int(std::time(nullptr));p.token="guest-offline-development-identity";
    ClashFixtures f(argv[1]);const auto home=f.home(p);emit("initial_home",home);
    const std::string name="Offline Player";
    const auto at=std::search(home.begin(),home.end(),name.begin(),name.end());
    if(at==home.end() || at-home.begin()<10)return 4;
    std::printf("#define INITIAL_AVATAR_OFFSET %zuu\n",size_t(at-home.begin()-10));
    Bytes reply;for(int i=0;i<2;++i){clashInt(reply,0);clashInt(reply,1);}
    for(auto s:{p.token,std::string(),std::string()})clashString(reply,s);
    for(int n:{2,0,1306,0})clashVint(reply,n);clashString(reply,"dev");
    for(int n:{1,0,0})clashVint(reply,n);clashString(reply,"");
    clashString(reply,std::to_string(p.created));clashString(reply,std::to_string(p.created));clashVint(reply,0);
    emit("initial_login",reply);
    const Bytes avatar(home.begin()+(at-home.begin()-10),home.end()-1);
    // The pinned 2016 exp_levels.csv contains levels 1..12, not modern level 13.
    std::puts("#define GUEST_MAX_TOWER_LEVEL 12\nstruct GuestTowerPatch {unsigned part,offset,width;int values[12];};");
    for(int i=0;i<7;++i)sectorTemplate("npc_"+std::to_string(i),ClashFixtures(argv[1],18000000+i),p,avatar);
    std::vector<int> arenaScores;
#ifdef GUEST_BOT_POLICY_GENERATED
    for(int i=0;i<3*GUEST_BOT_DECK_COUNT;++i) {
        auto bot=opponentFixture(argv[1],i);
        sectorTemplate("arena1_bot_"+std::to_string(i),bot,p,avatar,true);
        arenaScores.push_back(bot.npcTrophies);
    }
    std::printf("#define GUEST_ARENA_PROFILE_COUNT %d\n",3*GUEST_BOT_DECK_COUNT);
    std::printf("static const int arena_mana_rates[3]={%d,%d,%d};\n",guest_bot_mana_rates[0],guest_bot_mana_rates[1],guest_bot_mana_rates[2]);
#endif
    std::puts("struct GuestSector {const unsigned char *parts[4];unsigned sizes[4];const struct GuestTowerPatch *patches;unsigned patchCount,lowBits,nameOffset,nameSize;};");
    std::puts("static const struct GuestSector npc_sectors[]={");
    for(int i=0;i<7;++i)std::printf("{{npc_%d_a,npc_%d_b,npc_%d_c,npc_%d_d},{sizeof npc_%d_a,sizeof npc_%d_b,sizeof npc_%d_c,sizeof npc_%d_d},npc_%d_patches,sizeof npc_%d_patches/sizeof *npc_%d_patches,npc_%d_LOW_BITS,0,0},\n",i,i,i,i,i,i,i,i,i,i,i,i);
    std::puts("};");
    std::puts("static const struct GuestSector arena1_sectors[]={");
#ifdef GUEST_BOT_POLICY_GENERATED
    for(int i=0;i<3*GUEST_BOT_DECK_COUNT;++i)std::printf("{{arena1_bot_%d_a,arena1_bot_%d_b,arena1_bot_%d_c,arena1_bot_%d_d},{sizeof arena1_bot_%d_a,sizeof arena1_bot_%d_b,sizeof arena1_bot_%d_c,sizeof arena1_bot_%d_d},arena1_bot_%d_patches,sizeof arena1_bot_%d_patches/sizeof *arena1_bot_%d_patches,arena1_bot_%d_LOW_BITS,arena1_bot_%d_NAME_OFFSET,arena1_bot_%d_NAME_SIZE},\n",i,i,i,i,i,i,i,i,i,i,i,i,i,i);
#endif
    std::puts("};\nstatic const int arena1_npc_trophies[]={");
    for(int score:arenaScores)std::printf("%d,",score);
    std::puts("};");
#ifdef GUEST_BOT_POLICY_GENERATED
    std::puts("static const unsigned arena1_deck_ids[]={");
    for(int i=0;i<3*GUEST_BOT_DECK_COUNT;++i)std::printf("%d,",i%GUEST_BOT_DECK_COUNT);
    std::puts("};\nstatic const int arena1_missions[]={");
    for(int i=0;i<3*GUEST_BOT_DECK_COUNT;++i)std::printf("%d,",18000007+i/GUEST_BOT_DECK_COUNT);
    std::puts("};");
#endif
}
