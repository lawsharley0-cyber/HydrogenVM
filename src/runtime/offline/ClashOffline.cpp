#include "ClashOffline.hpp"
#include "ClashState.hpp"
#include "ClashCsv.hpp"
#include "runtime/Process.hpp"
#include "runtime/libc/HostCrypto.hpp"
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/file.h>
#include <poll.h>
#include <fcntl.h>
#include <unistd.h>
#include <atomic>
#include <algorithm>
#include <array>
#include <ctime>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <random>
#include <stdexcept>
#include <thread>
#include <zlib.h>

namespace Hydrogen::Runtime::Offline {
namespace {
constexpr size_t limit=1<<20;
constexpr char key[]="fhsd6f86f67rt8fw78fw789we78r9789wer6renonce";
struct Cipher {
    HostCrypto::Cryptor* handle=nullptr;
    Cipher() {
        if(HostCrypto::create(HostCrypto::kEncrypt,HostCrypto::kRC4,0,
            reinterpret_cast<const uint8_t*>(key),sizeof(key)-1,nullptr,&handle)!=0)throw std::runtime_error("Offline Clash: host RC4 unavailable");
        try {apply(Bytes(key,key+sizeof(key)-1));}
        catch(...) {HostCrypto::release(handle);handle=nullptr;throw;}
    }
    ~Cipher(){HostCrypto::release(handle);}
    Bytes apply(const Bytes& input) {
        Bytes out(input.size()+32);size_t moved=0;
        if(HostCrypto::update(handle,input.data(),input.size(),out.data(),out.size(),&moved)!=0 || moved!=input.size())throw std::runtime_error("Offline Clash: cipher stream failed");
        out.resize(moved);return out;
    }
};
struct Reader {
    const Bytes& bytes;size_t pos=0;
    uint8_t byte(){if(pos>=bytes.size())throw std::runtime_error("Offline Clash: truncated message");return bytes[pos++];}
    int32_t integer(){uint32_t n=0;for(int i=0;i<4;++i)n=(n<<8)|byte();return int32_t(n);}
    int32_t v() {
        auto first=byte();int64_t n=first&63;int shift=6;auto next=first;
        for(int i=1;next&128;++i){if(i>4)throw std::runtime_error("Offline Clash: long VInt");next=byte();if(i==4 && next>15)throw std::runtime_error("Offline Clash: bad fifth VInt byte");n|=int64_t(next&127)<<shift;shift+=7;}
        if(first&64)n|=-(int64_t(1)<<std::min(shift,31));
        if(n<INT32_MIN || n>INT32_MAX)throw std::runtime_error("Offline Clash: VInt range");return int32_t(n);
    }
    std::string string(){int32_t n=integer();if(n==-1)return {};if(n<0 || size_t(n)>bytes.size()-pos)throw std::runtime_error("Offline Clash: bad string");std::string s(bytes.begin()+pos,bytes.begin()+pos+n);pos+=n;return s;}
};
Bytes initialNameCommand(const std::string& name) {
    Bytes command;clashVint(command,201);clashString(command,name);
    // Command decoder slot 0x48 is readInt; avatar snapshots use VInt for
    // this field, but LogicChangeAvatarNameCommand uses a fixed BE int.
    clashInt(command,0);command.push_back(1);
    for(int n:{0,-1,-1,0,1})clashVint(command,n);
    return command;
}
Bytes loginReply(const ClashProfile& p) {
    Bytes w;for(int i=0;i<2;++i){clashInt(w,0);clashInt(w,1);}
    for(auto& s:{p.token,std::string(),std::string()})clashString(w,s);
    for(auto n:{2,0,1306,0})clashVint(w,n);clashString(w,"dev");
    for(auto n:{1,0,0})clashVint(w,n);clashString(w,"");
    clashString(w,std::to_string(std::time(nullptr)));clashString(w,std::to_string(p.created));clashVint(w,0);return w;
}
void save(const std::filesystem::path& path,const ClashProfile& p) {
    auto value=Plist::Value::makeDict();
    value->set("format",Plist::Value::makeInt(4));value->set("token",Plist::Value::makeString(p.token));
    value->set("created",Plist::Value::makeInt(p.created));value->set("npcWins",Plist::Value::makeInt(p.npcWins));value->set("npcLosses",Plist::Value::makeInt(p.npcLosses));
    value->set("lastRewardedWin",Plist::Value::makeInt(p.lastRewardedWin));
    value->set("chestCycleIndex",Plist::Value::makeInt(p.chestCycleIndex));
    value->set("trophies",Plist::Value::makeInt(p.trophies));
    value->set("arenaLevel",Plist::Value::makeInt(p.arenaLevel));
    auto chests=Plist::Value::makeArray();
    for(int32_t chest:p.chests)chests->array.push_back(Plist::Value::makeInt(chest));value->set("chests",chests);
    value->set("nativeHome",Plist::Value::makeData(std::string(p.nativeHome.begin(),p.nativeHome.end())));
    value->set("nativeAvatar",Plist::Value::makeData(std::string(p.nativeAvatar.begin(),p.nativeAvatar.end())));
    value->set("avatarOffset",Plist::Value::makeInt(p.avatarOffset));
    value->set("pendingReward",Plist::Value::makeData(std::string(p.pendingReward.begin(),p.pendingReward.end())));
    value->set("pendingRewardSlot",Plist::Value::makeInt(p.pendingRewardSlot+1));
    value->set("pendingRewardKind",Plist::Value::makeInt(p.pendingRewardKind));
    value->set("rewardSerial",Plist::Value::makeInt(p.rewardSerial));
    value->set("clientExecutableSHA256",Plist::Value::makeString("3519d7954cb3dc16b9d7746cc5dbd2521d0209eea2bbc163f6ab7cdfa5e52397"));
    auto text=Plist::toXml(*value);std::filesystem::create_directories(path.parent_path());
    auto temp=path.string()+".tmp";int fd=::open(temp.c_str(),O_WRONLY|O_CREAT|O_EXCL,0600);
    if(fd<0)throw std::runtime_error("Offline Clash: cannot create atomic save");
    size_t at=0;bool ok=true;
    while(at<text.size()){auto n=::write(fd,text.data()+at,text.size()-at);if(n<0 && errno==EINTR)continue;if(n<=0){ok=false;break;}at+=size_t(n);}
    if(::fsync(fd)!=0)ok=false;if(::close(fd)!=0)ok=false;
    if(!ok || ::rename(temp.c_str(),path.c_str())!=0){::unlink(temp.c_str());throw std::runtime_error("Offline Clash: atomic save failed");}
}
ClashProfile load(const std::filesystem::path& path) {
    ClashProfile p;
    if(std::filesystem::exists(path)) {
        if(std::filesystem::file_size(path)>3*limit)throw std::runtime_error("Offline Clash: oversized save");
        auto value=Plist::parseFile(path.string());
        auto integer=[&](const char* key,int64_t max){auto* n=value->get(key);if(!n || n->type!=Plist::Value::Type::Integer || n->i<0 || n->i>max)throw std::runtime_error("Offline Clash: invalid save field");return int32_t(n->i);};
        const auto format=integer("format",4);
        if(format<1 || value->getString("clientExecutableSHA256")!="3519d7954cb3dc16b9d7746cc5dbd2521d0209eea2bbc163f6ab7cdfa5e52397")throw std::runtime_error("Offline Clash: save/client mismatch");
        p.token=value->getString("token");if(p.token.size()!=48 || p.token.find_first_not_of("0123456789abcdef")!=std::string::npos)throw std::runtime_error("Offline Clash: invalid local identity");
        p.created=integer("created",INT32_MAX);p.npcWins=integer("npcWins",1000);p.npcLosses=integer("npcLosses",1000);
        if(format>=2) {
            p.lastRewardedWin=integer("lastRewardedWin",p.npcWins);
            auto* cycle=value->get("chestCycleIndex");
            if(cycle && cycle->type==Plist::Value::Type::Integer) p.chestCycleIndex=integer("chestCycleIndex",100000);
            else p.chestCycleIndex=p.lastRewardedWin;
            auto* troph=value->get("trophies");
            if(troph && troph->type==Plist::Value::Type::Integer) p.trophies=integer("trophies",100000);
            else p.trophies=0;
            auto* ar=value->get("arenaLevel");
            if(ar && ar->type==Plist::Value::Type::Integer) p.arenaLevel=integer("arenaLevel",20);
            else p.arenaLevel=p.npcWins>=7?1:0;
            auto* chests=value->get("chests");
            if(!chests || chests->type!=Plist::Value::Type::Array || chests->array.size()!=4)throw std::runtime_error("Offline Clash: invalid chest slots");
            for(size_t i=0;i<4;++i){auto& chest=*chests->array[i];if(chest.type!=Plist::Value::Type::Integer || (chest.i!=0 && (chest.i<19000000 || chest.i>=20000000)))throw std::runtime_error("Offline Clash: invalid chest data");p.chests[i]=int32_t(chest.i);}
        } else if(p.npcWins==1) {
            // One-time migration of the original private first-win prototype:
            // it had no chest-collection support and could not consume Newbie1.
            p.chests[0]=19000000;p.lastRewardedWin=1;
        }
        if(format>=3) {
            auto data=[&](const char* name){auto* valueData=value->get(name);if(!valueData || valueData->type!=Plist::Value::Type::Data || valueData->s.size()>limit)throw std::runtime_error("Offline Clash: invalid native snapshot");return Bytes(valueData->s.begin(),valueData->s.end());};
            p.nativeHome=data("nativeHome");p.nativeAvatar=data("nativeAvatar");p.avatarOffset=integer("avatarOffset",p.nativeHome.size());
            if(!p.nativeHome.empty() && (p.avatarOffset==0 || p.avatarOffset>=p.nativeHome.size()))throw std::runtime_error("Offline Clash: invalid avatar offset");
        }
        if(format>=4) {
            auto* pending=value->get("pendingReward");
            if(!pending || pending->type!=Plist::Value::Type::Data || pending->s.size()>65536)
                throw std::runtime_error("Offline Clash: invalid reward journal");
            p.pendingReward=Bytes(pending->s.begin(),pending->s.end());p.pendingRewardSlot=integer("pendingRewardSlot",4)-1;
            p.pendingRewardKind=value->get("pendingRewardKind")?integer("pendingRewardKind",2):(p.pendingReward.empty()?0:1);
            if((p.pendingReward.empty() && (p.pendingRewardSlot!=-1 || p.pendingRewardKind!=0)) ||
               (!p.pendingReward.empty() && (p.nativeHome.empty() || p.pendingRewardKind==0 ||
                (p.pendingRewardKind==1 && p.pendingRewardSlot<0) || (p.pendingRewardKind==2 && p.pendingRewardSlot!=-1))))
                throw std::runtime_error("Offline Clash: incomplete reward journal");
            if(value->get("rewardSerial"))p.rewardSerial=integer("rewardSerial",1000000);
            if(!p.pendingReward.empty()) {
                Reader command{p.pendingReward};
                const auto before=inspectClashHome(p.nativeHome);
                const auto source=p.pendingRewardKind==1?before.states[p.pendingRewardSlot]:before.freeChest;
                if(command.v()!=213 || !source.data || source.claimed)
                    throw std::runtime_error("Offline Clash: reward journal has no complete source chest");
            }
        }
        if(format<4)save(path,p);
    } else {
        std::random_device random;constexpr char hex[]="0123456789abcdef";
        for(int i=0;i<48;++i)p.token+=hex[random()&15];p.created=int32_t(std::time(nullptr));save(path,p);
    }
    return p;
}
Bytes compressed(const Bytes& data) {
    uLongf size=compressBound(data.size());Bytes result(5+size);result[0]=1;
    for(int i=0;i<4;++i)result[1+i]=uint32_t(data.size())>>(i*8);
    if(compress2(result.data()+5,&size,data.data(),data.size(),Z_DEFAULT_COMPRESSION)!=Z_OK)throw std::runtime_error("Offline Clash: sector compression failed");
    result.resize(5+size);return result;
}
}
struct ClashOffline::Impl {
    Process& process;ClashFixtures fixtures;std::filesystem::path path;ClashProfile profile;
    std::unique_ptr<ClashCsvEngine> csvEngine;
    std::mutex mutex,connectionMutex;std::atomic<bool> stopping{false};int active=-1;
    int listener=-1,lockFile=-1;uint16_t boundPort=0;std::thread worker;bool rewardForPlayer=false;
    uint32_t rewardAvatar=0,pendingBattleAvatar=0,scratch=0,stream=0;CPU::Cpu snapshotCpu;
    uint32_t endingBattle=0;
    bool pendingArenaBattle=false;
    int32_t requestedChestId=-1;
    bool requestedPaidClaim=false;
    uint32_t observedRewardCommand=0;
    bool pendingRewardApplied=false;
    std::chrono::steady_clock::time_point lastSnapshot{};
    explicit Impl(Process& p):process(p),fixtures(p.config().bundlePath),
        path(std::filesystem::path(p.config().homePath)/"Library/Application Support/HydrogenVM/ClashOffline.plist") {
        struct Setup {
            Impl& self;bool ready=false;
            ~Setup(){if(!ready){if(self.listener>=0)::close(self.listener);if(self.lockFile>=0)::close(self.lockFile);}}
        } setup{*this};
        std::filesystem::create_directories(path.parent_path());
        lockFile=::open((path.string()+".lock").c_str(),O_RDWR|O_CREAT,0600);
        if(lockFile<0 || ::flock(lockFile,LOCK_EX|LOCK_NB)!=0){if(lockFile>=0)::close(lockFile);lockFile=-1;throw std::runtime_error("Offline Clash: another session owns this save");}
        // The exclusive lock makes an interrupted same-directory temporary ours
        // to remove. The previous committed save is never replaced on failure.
        ::unlink((path.string()+".tmp").c_str());
        profile=load(path);
        process.logf(LogLevel::Info,"Offline Clash: loaded saved progress wins=%d losses=%d",profile.npcWins,profile.npcLosses);
        if(!profile.nativeAvatar.empty()) {
            const auto native=inspectClashAvatar(profile.nativeAvatar);
            if(profile.trophies!=native.trophies || profile.arenaLevel!=native.arena) {
                auto next=profile;next.trophies=native.trophies;next.arenaLevel=native.arena;
                save(path,next);profile=std::move(next);
                process.log(LogLevel::Info,"Offline Clash: reconciled metadata with the saved native score/arena");
            }
        }
        if(profile.pendingReward.empty() && !profile.nativeAvatar.empty()) {
            const auto corrected=withClashBattleProgress(profile.nativeAvatar,profile.chestCycleIndex);
            if(corrected!=profile.nativeAvatar) {
                auto next=profile;next.nativeAvatar=corrected;save(path,next);profile=std::move(next);
                process.log(LogLevel::Info,"Offline Clash: reconciled native chest index with earned cycle; no historical crowns inferred");
            }
        }
        // Confirm crypto availability before starting any guest connection.
        Cipher check;
        const auto resDir = std::filesystem::path(p.config().bundlePath) / "res";
        if (std::filesystem::exists(resDir / "csv_logic/treasure_chests.csv")) {
            csvEngine = std::make_unique<ClashCsvEngine>(resDir);
            // Older checkpoints could persist a qualified native win without
            // banking its reward when no battle log was generated. Complete
            // that one outstanding transaction using the bundled chest order.
            if(profile.npcWins==profile.lastRewardedWin+1) {
                auto next=profile;bankNpcChest(next);save(path,next);profile=std::move(next);
            }
        }
        listener=::socket(AF_INET,SOCK_STREAM,0);if(listener<0)throw std::runtime_error("Offline Clash: socket creation failed");
        sockaddr_in address{};address.sin_family=AF_INET;address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
#if defined(__APPLE__)
        address.sin_len=sizeof address;
#endif
        socklen_t length=sizeof address;
        if(::bind(listener,reinterpret_cast<sockaddr*>(&address),length)!=0 || ::listen(listener,1)!=0 || ::getsockname(listener,reinterpret_cast<sockaddr*>(&address),&length)!=0){::close(listener);listener=-1;throw std::runtime_error("Offline Clash: localhost listener failed");}
        boundPort=ntohs(address.sin_port);
        process.engine().observeEntry(0x1499c,[this](uint32_t,const CPU::Cpu& cpu){
            std::lock_guard lock(mutex);endingBattle=cpu.r[0];
        });
        process.engine().observeEntry(0x9ae94,[this](uint32_t,const CPU::Cpu& cpu){observeCount(cpu,true);});
        process.engine().observeEntry(0x9aea4,[this](uint32_t,const CPU::Cpu& cpu){observeCount(cpu,false);});
        // Native post-setter continuations: win after refreshArena, loss after
        // the count store. Preserve the new avatar before a GoHome reply can
        // replace the battle objects with the previous home snapshot.
        process.engine().observeEntry(0x14c82,[this](uint32_t,const CPU::Cpu&){commitBattleAvatar();});
        process.engine().observeEntry(0x14ce6,[this](uint32_t,const CPU::Cpu&){commitBattleAvatar();});
        process.engine().observeEntry(0x14e74,[this](uint32_t,const CPU::Cpu& cpu){
            const auto avatar=cpu.r[0];std::lock_guard lock(mutex);
            rewardForPlayer=uint64_t(avatar)+0x18<=UINT32_MAX && process.mem().read<int32_t>(avatar+0x10)==0 && process.mem().read<int32_t>(avatar+0x14)==1;
            rewardAvatar=rewardForPlayer?avatar:0;
        });
        process.engine().observeEntry(0x14e7c,[this](uint32_t,const CPU::Cpu& cpu){
            std::lock_guard lock(mutex);
            if(!rewardForPlayer || profile.lastRewardedWin==profile.npcWins)return;
            rewardForPlayer=false;auto next=profile;next.lastRewardedWin=next.npcWins;
            int32_t data = 0;
            if(cpu.r[0]) {
                data = process.mem().read<int32_t>(cpu.r[0]+0x10);
            }
            if(data/1000000!=19 && csvEngine) {
                const auto* chest = csvEngine->getChestForCycle(next.chestCycleIndex, next.arenaLevel > 0 ? next.arenaLevel : 1);
                if(chest) data = chest->globalId;
            }
            if(data/1000000==19) {
                auto slot=std::find(next.chests.begin(),next.chests.end(),0);
                if(slot!=next.chests.end()) {
                    *slot=data;
                    next.chestCycleIndex++;
                }
            }
            if(rewardAvatar && process.image().entry) {
                next.nativeAvatar=encodeNative(0,rewardAvatar).first;
            }
            save(path,next);profile=next;process.logf(LogLevel::Info,"Offline Clash: native win and chest reward committed (trophies=%d, arena=%d)",next.trophies,next.arenaLevel);
        });
        process.engine().observeEntry(0x92f08,[this](uint32_t,const CPU::Cpu& cpu){captureHome(cpu);});
        process.engine().observeEntry(0x8203c,[this](uint32_t,const CPU::Cpu& cpu){
            std::lock_guard lock(mutex);
            observedRewardCommand=profile.pendingRewardKind==2 && !profile.pendingReward.empty() &&
                process.mem().read<int32_t>(cpu.r[0]+0x24)==2?cpu.r[0]:0;
        });
        process.engine().observeEntry(0x8275e,[this](uint32_t,const CPU::Cpu& cpu){
            std::lock_guard lock(mutex);
            if(observedRewardCommand && cpu.r[4]==0 && profile.pendingRewardKind==2) {
                pendingRewardApplied=true;
                process.log(LogLevel::Info,"Offline Clash: native free-chest reward executed successfully");
            }
            observedRewardCommand=0;
        });
        process.log(LogLevel::Info,"Offline Clash: phone-local first-NPC service started; earned rewards/chests remain experimental");
        worker=std::thread([this]{serve();});
        setup.ready=true;
    }
    ~Impl(){stop();if(lockFile>=0)::close(lockFile);}
    void stop(){if(stopping.exchange(true))return;::shutdown(listener,SHUT_RDWR);{std::lock_guard lock(connectionMutex);if(active>=0)::shutdown(active,SHUT_RDWR);}if(worker.joinable())worker.join();if(listener>=0)::close(listener);listener=-1;}
    ClashProfile snapshot(){std::lock_guard lock(mutex);return profile;}
    void bankNpcChest(ClashProfile& next) {
        if(!csvEngine || next.npcWins==next.lastRewardedWin)return;
        if(next.npcWins!=next.lastRewardedWin+1)throw std::runtime_error("Offline Clash: non-contiguous NPC reward");
        auto slot=std::find(next.chests.begin(),next.chests.end(),0);
        if(slot!=next.chests.end()) {
            const auto* chest=csvEngine->getChestForCycle(next.chestCycleIndex,std::max(next.arenaLevel,1));
            if(!chest)throw std::runtime_error("Offline Clash: missing bundled chest cycle reward");
            *slot=chest->globalId;++next.chestCycleIndex;
        }
        next.lastRewardedWin=next.npcWins;
        if(!next.nativeAvatar.empty())next.nativeAvatar=withClashBattleProgress(next.nativeAvatar,next.chestCycleIndex);
        process.log(LogLevel::Info,"Offline Clash: earned NPC chest banked from bundled chest order");
    }
    void observeCount(const CPU::Cpu& cpu,bool wins) {
        // Read the original account stored inline at avatar+0x10. Reject NPCs
        // and only observe the native battleEnded setter call sites.
        const uint32_t expected=wins?0x14c7d:0x14ce7;
        if(cpu.r[CPU::kLR]!=expected)return;
        const auto avatar=cpu.r[0];auto& mem=process.mem();
        if(uint64_t(avatar)+0x18>UINT32_MAX || mem.read<int32_t>(avatar+0x10)!=0 || mem.read<int32_t>(avatar+0x14)!=1)return;
        const int32_t count=int32_t(cpu.r[1]);std::lock_guard lock(mutex);
        const int32_t previous=wins?profile.npcWins:profile.npcLosses;
        if(count==previous)return;
        if(count!=previous+1 || count>1000)throw std::runtime_error("Offline Clash: unexpected native NPC counter transition");
        auto next=profile;(wins?next.npcWins:next.npcLosses)=count;
        // Original battleEnded calls updateScores before either counter setter.
        // Persist that result; do not invent rating deltas or overwrite avatars.
        next.trophies=mem.read<int32_t>(avatar+0x34);
        if(next.trophies<0 || next.trophies>100000)throw std::runtime_error("Offline Clash: invalid native battle score");
        if(profile.npcWins >= 7) {
            if(csvEngine)next.arenaLevel=csvEngine->getArenaForTrophies(next.trophies,next.arenaLevel);
        } else if(next.npcWins >= 7 && next.arenaLevel == 0) {
            next.arenaLevel = 1;
        }
        if(wins && csvEngine)bankNpcChest(next);
        // On a real client the post-setter continuation commits the updated
        // native avatar and counters/reward together. Saving just the counters
        // here could leave an older avatar after interruption.
        if(!process.image().entry && (!wins || next.lastRewardedWin==next.npcWins))save(path,next);
        pendingArenaBattle=profile.npcWins>=7;
        profile=next;pendingBattleAvatar=avatar;
        process.logf(LogLevel::Info,"Offline Clash: native NPC %s observed (%d, trophies=%d, arena=%d)%s",
            wins?"win":"loss",count,next.trophies,next.arenaLevel,process.image().entry?"; awaiting native avatar commit":"; metadata test transaction");
    }
    std::pair<Bytes,size_t> encodeNative(uint32_t home,uint32_t avatar) {
        if(!scratch){scratch=process.heap().allocZeroed(256*1024);stream=process.heap().allocZeroed(64);}
        snapshotCpu.r[CPU::kSP]=scratch+256*1024;
        process.callGuest(snapshotCpu,0x192a71,{stream,65536});
        if(home)process.callGuest(snapshotCpu,0x124cfd,{home,stream});
        const size_t offset=uint32_t(process.callGuest(snapshotCpu,0x192b2d,{stream}));
        process.callGuest(snapshotCpu,0x99e51,{avatar,stream});
        const auto size=uint32_t(process.callGuest(snapshotCpu,0x192b2d,{stream}));
        const auto data=uint32_t(process.callGuest(snapshotCpu,0x1936fd,{stream}));
        if(size>limit || size<offset)throw std::runtime_error("Offline Clash: native serializer exceeded snapshot bounds");
        Bytes bytes(size);process.mem().copyOut(bytes.data(),data,size);
        process.callGuest(snapshotCpu,0x19370d,{stream});
        if(home)bytes.push_back(0);return {bytes,offset};
    }
    void commitBattleAvatar() {
        if(!process.image().entry)return; // synthetic counter/transport tests have no native executable
        std::lock_guard lock(mutex);const auto avatar=pendingBattleAvatar;if(!avatar)return;
        auto& mem=process.mem();
        if(mem.read<int32_t>(avatar+0x10)!=0 || mem.read<int32_t>(avatar+0x14)!=1 ||
           mem.read<int32_t>(avatar+0x9c)!=profile.npcWins || mem.read<int32_t>(avatar+0xa0)!=profile.npcLosses)
            throw std::runtime_error("Offline Clash: native battle avatar did not complete its counter transition");
        auto next=profile;next.nativeAvatar=encodeNative(0,avatar).first;
        int32_t crowns=0;
        if(pendingArenaBattle) {
            if(!endingBattle)throw std::runtime_error("Offline Clash: arena result has no native battle");
            const auto account=mem.read<uint32_t>(endingBattle+0x10);
            if(!account || mem.read<int32_t>(account)!=0 || mem.read<int32_t>(account+4)!=1)
                throw std::runtime_error("Offline Clash: arena crown owner mismatch");
            // Original LogicSummoner::getStars, also used by battleEnded.
            // NPC mode skips transferring them; the offline arena ledger must
            // carry the actual result into home.updateStarsFromAvatar.
            crowns=int32_t(process.callGuest(snapshotCpu,0x10f3fd,
                {mem.read<uint32_t>(endingBattle+0x38),mem.read<uint32_t>(endingBattle+0x34)}));
            if(crowns<0 || crowns>3)throw std::runtime_error("Offline Clash: invalid native arena crowns");
        }
        next.nativeAvatar=withClashBattleProgress(next.nativeAvatar,next.chestCycleIndex,crowns);
        save(path,next);profile=std::move(next);pendingBattleAvatar=0;
        if(pendingArenaBattle)process.logf(LogLevel::Info,"Offline Clash: %d actual native arena crowns committed for home transfer",crowns);
        pendingArenaBattle=false;endingBattle=0;
        process.log(LogLevel::Info,"Offline Clash: native battle avatar and earned progress committed together");
    }
    Bytes encodeNativeReward(uint32_t home,uint32_t chest,int32_t chestId,int32_t source=1) {
        if(!scratch){scratch=process.heap().allocZeroed(256*1024);stream=process.heap().allocZeroed(64);}
        snapshotCpu.r[CPU::kSP]=scratch+256*1024;
        // Mirror LogicProgressSimulator::openChest (0x8f2b4..0x8f318), using
        // the original randomizer and command setters. These are temporary
        // objects owned by the adapter; no live guest state is overwritten.
        const auto randomizer=process.heap().allocZeroed(8),command=process.heap().allocZeroed(44);
        process.callGuest(snapshotCpu,0x132f75,{randomizer});
        const auto reward=uint32_t(process.callGuest(snapshotCpu,0x132f99,
            {randomizer,chest,uint32_t(profile.created)^uint32_t(profile.chestCycleIndex),home}));
        process.callGuest(snapshotCpu,0x81fed,{command});
        process.callGuest(snapshotCpu,0x829c1,{command,uint32_t(chestId)});
        process.callGuest(snapshotCpu,0x829c5,{command,uint32_t(source)});
        process.callGuest(snapshotCpu,0x82841,{command,reward});
        process.callGuest(snapshotCpu,0x192a71,{stream,65536});
        process.callGuest(snapshotCpu,0x6d8c5,{stream,command});
        const auto size=uint32_t(process.callGuest(snapshotCpu,0x192b2d,{stream}));
        const auto data=uint32_t(process.callGuest(snapshotCpu,0x1936fd,{stream}));
        if(size<3 || size>65536)throw std::runtime_error("Offline Clash: native reward exceeded command bounds");
        Bytes packet(size);process.mem().copyOut(packet.data(),data,size);
        process.callGuest(snapshotCpu,0x19370d,{stream});
        process.callGuest(snapshotCpu,0x82011,{command});
        process.callGuest(snapshotCpu,0x132f79,{randomizer});
        process.heap().free(command);process.heap().free(randomizer);
        return packet;
    }
    void captureHome(const CPU::Cpu& cpu) {
        const auto now=std::chrono::steady_clock::now();
        if(now-lastSnapshot<std::chrono::milliseconds(1000) || process.stopping())return;
        lastSnapshot=now;auto& mem=process.mem();const uint32_t uiMode=cpu.r[0];
        if(mem.read<int32_t>(uiMode+0x28)!=0)return;
        const auto logic=mem.read<uint32_t>(uiMode+0x54);if(!logic)return;
        const auto home=mem.read<uint32_t>(logic+0x30),avatar=mem.read<uint32_t>(logic+0x34);if(!home || !avatar)return;
        std::lock_guard lock(mutex);
        if(mem.read<int32_t>(avatar+0x10)!=0 || mem.read<int32_t>(avatar+0x14)!=1 ||
            mem.read<int32_t>(avatar+0x9c)!=profile.npcWins || mem.read<int32_t>(avatar+0xa0)!=profile.npcLosses)return;
        // Only a newly received, validated claim can generate a reward. Older
        // half-claimed saves without a journal remain a recovery task.
        if(mem.read<uint8_t>(home+0xb4)) {
            if(requestedChestId==-2 && profile.pendingReward.empty() && !profile.nativeHome.empty() && process.image().entry) {
                const auto before=inspectClashHome(profile.nativeHome);
                const auto chest=mem.read<uint32_t>(home+0x78); // getFreeChest 0x126434
                if(!chest || !before.freeChest.data || before.freeChest.claimed || !mem.read<uint8_t>(chest+0x18))
                    throw std::runtime_error("Offline Clash: free reward has no complete pre-claim checkpoint");
                auto next=profile;
                if(next.rewardSerial>=1000000)throw std::runtime_error("Offline Clash: reward serial exhausted");
                ++next.rewardSerial;next.pendingReward=encodeNativeReward(home,chest,0,2);
                next.pendingRewardSlot=-1;next.pendingRewardKind=2;pendingRewardApplied=false;
                save(path,next);profile=std::move(next);requestedChestId=-1;
                process.log(LogLevel::Info,"Offline Clash: native free-chest reward journaled before delivery");
            }
            if(requestedChestId>=0 && profile.pendingReward.empty() && !profile.nativeHome.empty() && process.image().entry) {
                const auto count=mem.read<int32_t>(home+0x58);
                const auto list=mem.read<uint32_t>(home+0x50);
                if(count!=4 || !list)throw std::runtime_error("Offline Clash: invalid native chest list");
                const auto before=inspectClashHome(profile.nativeHome);
                for(int i=0;i<count;++i) {
                    const auto chest=mem.read<uint32_t>(list+i*4);
                    if(!chest || mem.read<int32_t>(chest+8)!=requestedChestId || !mem.read<uint8_t>(chest+0x18))continue;
                    if(!before.chests[i] || before.states[i].claimed || before.states[i].value8!=requestedChestId)
                        throw std::runtime_error("Offline Clash: reward has no complete pre-claim checkpoint");
                    auto next=profile;
                    if(next.rewardSerial>=1000000)throw std::runtime_error("Offline Clash: reward serial exhausted");
                    if(requestedPaidClaim) {
                        auto [checkpoint,offset]=encodeNative(home,avatar);
                        next.nativeHome=unlockedClashRewardCheckpoint(checkpoint,i,requestedChestId);
                        next.avatarOffset=offset;
                        next.nativeAvatar=Bytes(checkpoint.begin()+offset,checkpoint.end()-1);
                    }
                    ++next.rewardSerial;next.pendingReward=encodeNativeReward(home,chest,requestedChestId);next.pendingRewardSlot=i;next.pendingRewardKind=1;
                    save(path,next);profile=std::move(next);requestedChestId=-1;requestedPaidClaim=false;
                    process.log(LogLevel::Info,"Offline Clash: native chest reward journaled before delivery");break;
                }
            }
            return;
        }
        if(profile.pendingRewardKind==2 && !pendingRewardApplied)return;
        auto [bytes,offset]=encodeNative(home,avatar);
        if(bytes==profile.nativeHome)return;
        const auto view=inspectClashHome(bytes);
        if(profile.pendingRewardKind==1 && view.chests[profile.pendingRewardSlot])return;
        auto next=profile;next.nativeHome=bytes;next.avatarOffset=offset;
        if(!next.pendingReward.empty()) {
            next.pendingReward.clear();next.pendingRewardSlot=-1;next.pendingRewardKind=0;pendingRewardApplied=false;
            process.log(LogLevel::Info,"Offline Clash: native chest collection completed; checkpoint committed");
        }
        next.nativeAvatar=withClashBattleProgress(Bytes(bytes.begin()+offset,bytes.end()-1),next.chestCycleIndex);next.chests=view.chests;
        save(path,next);profile=next;
    }
    bool transfer(int fd,uint8_t* data,size_t size,bool write) {
        size_t at=0;
        while(at<size && !stopping) {
            pollfd wait{fd,short(write?POLLOUT:POLLIN),0};const int ready=::poll(&wait,1,100);
            if(ready<0 && errno==EINTR)continue;if(ready<0)return false;if(!ready)continue;
#if defined(__APPLE__)
            constexpr int flags=0;
#else
            constexpr int flags=MSG_NOSIGNAL;
#endif
            ssize_t n=write ? ::send(fd,data+at,size-at,flags) : ::recv(fd,data+at,size-at,0);
            if(n<0 && (errno==EINTR || errno==EAGAIN || errno==EWOULDBLOCK))continue;
            if(n<=0) {
                process.logf(LogLevel::Info, "Offline Clash transfer %s n=%zd errno=%d at=%zu size=%zu", write?"write":"read", n, errno, at, size);
                return false;
            }
            at+=size_t(n);
        }
        return at==size;
    }
    void session(int fd) {
        Cipher incoming,outgoing;bool loggedIn=false;int32_t deliveredReward=-1;
        auto send=[&](uint16_t kind,uint16_t version,const Bytes& body) {
            auto encrypted=outgoing.apply(body);Bytes packet{uint8_t(kind>>8),uint8_t(kind),uint8_t(body.size()>>16),uint8_t(body.size()>>8),uint8_t(body.size()),uint8_t(version>>8),uint8_t(version)};
            packet.insert(packet.end(),encrypted.begin(),encrypted.end());if(!transfer(fd,packet.data(),packet.size(),true))throw std::runtime_error("Offline Clash: disconnected while sending");
        };
        process.logf(LogLevel::Info, "Offline Clash session started fd=%d", fd);
        while(!stopping) {
            std::array<uint8_t,7> header{};
            if(!transfer(fd,header.data(),header.size(),false)) {
                process.log(LogLevel::Info, "Offline Clash transfer failed reading header");
                return;
            }
            const uint16_t kind=uint16_t(header[0])<<8|header[1];const size_t size=size_t(header[2])<<16|size_t(header[3])<<8|header[4];
            process.logf(LogLevel::Info, "Offline Clash received packet kind=%d size=%zu", kind, size);
            if(size>limit)throw std::runtime_error("Offline Clash: oversized packet");
            Bytes body(size);if(size && !transfer(fd,body.data(),size,false))return;body=incoming.apply(body);Reader r{body};
            if(kind==10101) {
                const int32_t high=r.integer(),low=r.integer();auto token=r.string();
                if(r.v()!=2 || r.v()!=0 || r.v()!=1306)throw std::runtime_error("Offline Clash: unsupported wire version");
                auto state=snapshot();if(!(high==0 && low==0) && !(high==0 && low==1 && token==state.token))throw std::runtime_error("Offline Clash: identity mismatch");
                {std::lock_guard lock(mutex);requestedChestId=-1;requestedPaidClaim=false;}
                loggedIn=true;send(20104,1,loginReply(state));send(24101,0,fixtures.home(state));
                if(!state.nativeAvatar.empty()) {
                    Reader avatar{state.nativeAvatar};for(int i=0;i<6;++i)avatar.v();const auto name=avatar.string();
                    const bool named=avatar.byte()&1;const auto changeState=avatar.v();
                    // Repair only the exact state produced by checkpoint 33d82ad's
                    // short integer: 00 01 00 7f. Reapply the SAME initial name
                    // through the original native command, without changing saves
                    // or allowing a paid rename. A snapshot ends this replay.
                    if(named && changeState==0x1007f) {
                        send(24111,0,initialNameCommand(name));
                        process.log(LogLevel::Info,"Offline Clash: correcting initial-name command integer width");
                    }
                }
            } else if(kind==10108 && body.empty()) {
                send(20108,0,{});
            } else if(!loggedIn)throw std::runtime_error("Offline Clash: login required");
            else if(kind==10212) {
                // ChangeAvatarNameMessage::decode 0x7ce7c: bounded String, bool.
                const auto name=r.string();const auto byUser=r.byte();
                if(name.empty() || name.size()>60 || byUser!=1 || r.pos!=body.size() ||
                   std::any_of(name.begin(),name.end(),[](unsigned char ch){return ch<32 || ch==127;}))
                    throw std::runtime_error("Offline Clash: invalid initial name request");
                const auto state=snapshot();
                if(!state.nativeAvatar.empty()) {
                    Reader avatar{state.nativeAvatar};for(int i=0;i<6;++i)avatar.v();avatar.string();
                    if(avatar.byte()&1)throw std::runtime_error("Offline Clash: paid name changes are not implemented");
                }
                // Native LogicChangeAvatarNameCommand 201 (0x122958): String,
                // change state, user flag, then server ID and command metadata.
                // The native command sets the name/flag and notifies the UI;
                // normal native home snapshots persist the completed change.
                // Until captured, relaunch offers the initial dialog again.
                send(24111,0,initialNameCommand(name));
                process.log(LogLevel::Info,"Offline Clash: initial name command supplied");
            }
            else if(kind==14104) {
                const int32_t table=r.v(),id=r.v();auto state=snapshot();
                if(table!=18 || id<0 || id>6)throw std::runtime_error("Offline Clash: mission does not match the supported training progress");
                // All seven bundled training NPCs use their own Location.
                // NPC7 is still TrainingCamp; Arena 1 begins after graduation.
                auto mission=id==0?fixtures:ClashFixtures(process.config().bundlePath,18000000+id);
                send(21903,1,compressed(mission.sector(state)));process.logf(LogLevel::Info,"Offline Clash: native Npc%d sector supplied",id+1);
            } else if(kind==14101) {
                if(body.size()!=1 || body[0]>1)throw std::runtime_error("Offline Clash: unsupported GoHome schema");
                send(24101,0,fixtures.home(snapshot()));process.log(LogLevel::Info,"Offline Clash: returned home with native NPC counters");
            } else if(kind==14102) {
                // Client turn commands: handle matchmaking (cmd 537) to supply arena battle
                std::vector<ClashHomeCommand> commands;
                try {commands=decodeClashHomeTurn(body);}
                catch(const std::runtime_error& error) {
                    process.log(LogLevel::Debug,error.what());
                    continue; // no reply, replay or mutation for diagnostic-only turns
                }
                    for(const auto& command:commands) {
                        const int32_t cmdType=command.type;
                        if(cmdType==537) {
                            auto state=snapshot();
                            int32_t arenaLevel = state.npcWins >= 7 ? (state.arenaLevel > 0 ? state.arenaLevel : 1) : 0;
                            int32_t missionId = 0;
                            if(arenaLevel == 0) {
                                missionId = 18000000 + std::clamp(state.npcWins, 0, 6);
                            } else {
                                missionId = csvEngine ? csvEngine->findBestNpcForTrophies(state.trophies) : 18000007;
                            }
                            ClashFixtures pvpMatch(process.config().bundlePath, missionId, arenaLevel);
                            send(21903, 1, compressed(pvpMatch.sector(state)));
                            process.logf(LogLevel::Info,"Offline Clash: matchmaking sector supplied for battle (Arena %d, bot %d)", arenaLevel, missionId);
                            break;
                        } else if(cmdType==502) {
                            process.logf(LogLevel::Info,"Offline Clash: StartExploring chest %d", command.argument);
                        } else if(cmdType==503) {
                            // StartRewardClaim only marks the native chest claimed.
                            // It is not proof that the server's 213 reward executed.
                            {std::lock_guard lock(mutex);requestedChestId=command.argument;requestedPaidClaim=false;}
                            process.logf(LogLevel::Info,"Offline Clash: StartRewardClaim chest %d received", command.argument);
                        } else if(cmdType==504) {
                            process.logf(LogLevel::Info,"Offline Clash: FuseSpells spell %d", command.argument);
                        } else if(cmdType==518) {
                            {std::lock_guard lock(mutex);requestedChestId=-2;requestedPaidClaim=false;}
                            process.log(LogLevel::Info,"Offline Clash: native free-chest collection requested");
                        } else if(cmdType==500 || cmdType==525 || cmdType==538) {
                            // Do not replay locally executed commands or send an
                            // invented checksum acknowledgement. captureHome
                            // preserves their original native results.
                            process.logf(LogLevel::Info,"Offline Clash: native home echo %d decoded; awaiting native checkpoint",cmdType);
                        } else if(cmdType==506) {
                            if(command.flag){std::lock_guard lock(mutex);requestedChestId=command.argument;requestedPaidClaim=true;}
                            process.logf(LogLevel::Info,"Offline Clash: SpeedUpExploring chest %d (claim=%d)", command.argument, command.flag);
                        }
                    }
            }
            if(loggedIn) {
                auto state=snapshot();
                if(!state.pendingReward.empty() && state.rewardSerial!=deliveredReward) {
                    send(24111,0,state.pendingReward);deliveredReward=state.rewardSerial;
                    process.log(LogLevel::Info,"Offline Clash: journaled native reward command delivered");
                }
            }
        }
    }
    void serve() noexcept {
        process.logf(LogLevel::Info, "Offline Clash listener running on port %d", boundPort);
        while(!stopping) {
            pollfd wait{listener,POLLIN,0};if(::poll(&wait,1,100)<=0)continue;
            int fd=::accept(listener,nullptr,nullptr);if(fd<0)continue;
            process.logf(LogLevel::Info, "Offline Clash accepted connection fd=%d", fd);
#if defined(__APPLE__)
            int enabled=1;if(::setsockopt(fd,SOL_SOCKET,SO_NOSIGPIPE,&enabled,sizeof enabled)!=0){::close(fd);continue;}
#endif
            {std::lock_guard lock(connectionMutex);active=fd;}
            try{session(fd);}catch(const std::exception& error){if(!stopping)process.log(LogLevel::Error,error.what());}
            {std::lock_guard lock(connectionMutex);active=-1;::close(fd);}
        }
    }
};
ClashOffline::ClashOffline(Process& p):impl_(std::make_unique<Impl>(p)){}
ClashOffline::~ClashOffline()=default;
uint16_t ClashOffline::port()const{return impl_->boundPort;}
void ClashOffline::stop(){impl_->stop();}
}
