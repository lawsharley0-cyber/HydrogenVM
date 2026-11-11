// Shared UIImagePickerController photo-library bridge. Camera/video are unavailable.
#include "UIKit.hpp"
#include <atomic>

namespace Hydrogen::Runtime::UIKit {
namespace {
struct PickerData : ObjC::HostObject { id delegate = 0, mediaTypes = 0; int32_t source = 0, cameraDevice = 0, videoQuality = 0; bool editing = false, showsCamera = true; id overlay = 0; };
std::atomic<uint64_t> nextRequest{1};
PickerData& picker(Ctx& x,id self){return rt(x).host<PickerData>(self);}
bool available(Ctx& x,id,SEL,int32_t source){
    std::lock_guard<std::mutex> lock(ui(x).inputMu);return source==0 && bool(ui(x).photoPickerObserver);
}
id mediaTypes(Ctx& x,id s,SEL sel,int32_t source){return available(x,s,sel,source)?Foundation::makeArray(x,{Foundation::makeString(x,"public.image")}):0;}
bool camera(Ctx&,id,SEL,int32_t){return false;}
id delegate(Ctx& x,id s,SEL){return picker(x,s).delegate;}
void setDelegate(Ctx& x,id s,SEL,id d){picker(x,s).delegate=d;}
void setSource(Ctx& x,id s,SEL,int32_t v){picker(x,s).source=v;}
int32_t source(Ctx& x,id s,SEL){return picker(x,s).source;}
void setEditing(Ctx& x,id s,SEL,bool v){picker(x,s).editing=v;}
bool editing(Ctx& x,id s,SEL){return picker(x,s).editing;}
void setMedia(Ctx& x,id s,SEL,id v){auto& d=picker(x,s);id old=d.mediaTypes;d.mediaTypes=v?rt(x).retain(x.cpu,v):0;if(old)rt(x).release(x.cpu,old);}
id getMedia(Ctx& x,id s,SEL){auto& d=picker(x,s);return d.mediaTypes?d.mediaTypes:Foundation::makeArray(x,{Foundation::makeString(x,"public.image")});}
void dealloc(Ctx& x,id s,SEL sel){auto& d=picker(x,s);if(d.mediaTypes)rt(x).release(x.cpu,d.mediaTypes);if(d.overlay)rt(x).release(x.cpu,d.overlay);rt(x).sendSuper(x.cpu,s,rt(x).getClass("UINavigationController"),sel);}
void publish(Ctx& x,bool active,uint64_t request){
    std::function<void(const PhotoPickerRequest&)> observer;
    {std::lock_guard<std::mutex> lock(ui(x).inputMu);observer=ui(x).photoPickerObserver;}
    if(observer)observer({active,request});
}
}
void setPhotoPickerObserver(Process& p,std::function<void(const PhotoPickerRequest&)> observer){
    std::lock_guard<std::mutex> lock(ui(p).inputMu);ui(p).photoPickerObserver=std::move(observer);
}
bool presentPhotoPicker(Ctx& x,id controller){
    if(!rt(x).isKindOf(controller,rt(x).getClass("UIImagePickerController")))return false;
    auto& u=ui(x);auto& d=picker(x,controller);
    if(u.activePhotoPicker){
        Process* p=&x.proc;rt(x).retain(x.cpu,controller);
        Foundation::mainRunLoop(x.proc).post([p,controller](CPU::Cpu& c){Ctx cx{*p,c,p->mem()};const id d=picker(cx,controller).delegate;
            if(d && rt(cx).respondsTo(d,rt(cx).sel("imagePickerControllerDidCancel:")))rt(cx).send(c,d,"imagePickerControllerDidCancel:",{controller});
            rt(cx).release(c,controller);
        });return true;
    }
    u.activePhotoPicker=rt(x).retain(x.cpu,controller);u.photoRequestId=nextRequest.fetch_add(1);u.photoResultDelivered=false;
    bool images=false;
    for(id t:Foundation::arrayItems(x,getMedia(x,controller,0)))if(Foundation::utf8(x,t)=="public.image")images=true;
    if(!available(x,controller,0,d.source) || !images)submitPhotoPickerResult(x.proc,u.photoRequestId,{});
    else publish(x,true,u.photoRequestId);
    return true;
}
void dismissPhotoPicker(Ctx& x,id controller){
    auto& u=ui(x);if(u.activePhotoPicker!=controller)return;
    const id old=u.activePhotoPicker;const auto request=u.photoRequestId;u.activePhotoPicker=0;u.photoResultDelivered=true;
    publish(x,false,request);rt(x).release(x.cpu,old);
}
void submitPhotoPickerResult(Process& p,uint64_t requestId,std::vector<uint8_t> data){
    if(data.size()>16u*1024*1024)data.clear();
    Process* process=&p;
    Foundation::mainRunLoop(p).post([process,requestId,data=std::move(data)](CPU::Cpu& cpu){
        Ctx x{*process,cpu,process->mem()};auto& u=ui(x);
        if(!u.activePhotoPicker || u.photoRequestId!=requestId || u.photoResultDelivered)return;
        u.photoResultDelivered=true;const id controller=u.activePhotoPicker,d=picker(x,controller).delegate;
        auto image=data.empty()?G::ImagePtr{}:G::decodeImage(data.data(),data.size());
        if(image && (image->width>4096 || image->height>4096))image.reset();
        if(image){
            const id original=makeUIImage(x,std::move(image),1);
            const id info=Foundation::makeDict(x,{{Foundation::makeString(x,"UIImagePickerControllerOriginalImage"),original},
                                                 {Foundation::makeString(x,"UIImagePickerControllerMediaType"),Foundation::makeString(x,"public.image")}});
            if(d && rt(x).respondsTo(d,rt(x).sel("imagePickerController:didFinishPickingMediaWithInfo:")))
                rt(x).send(cpu,d,"imagePickerController:didFinishPickingMediaWithInfo:",{controller,info});
            else if(d && rt(x).respondsTo(d,rt(x).sel("imagePickerController:didFinishPickingImage:editingInfo:")))
                rt(x).send(cpu,d,"imagePickerController:didFinishPickingImage:editingInfo:",{controller,original,0});
        }else if(d && rt(x).respondsTo(d,rt(x).sel("imagePickerControllerDidCancel:")))rt(x).send(cpu,d,"imagePickerControllerDidCancel:",{controller});
    });
}
void registerPhotoPicker(Process& p,ObjC::Runtime& r){
    ObjC::ClassBuilder ip=r.defineClass("UIImagePickerController","UINavigationController");
    ip.cm<&available>("isSourceTypeAvailable:","c@:i").cm<&mediaTypes>("availableMediaTypesForSourceType:","@@:i")
      .cm<&camera>("isCameraDeviceAvailable:","c@:i").cm<&camera>("isFlashAvailableForCameraDevice:","c@:i")
      .m<&delegate>("delegate").m<&setDelegate>("setDelegate:","v@:@")
      .m<&setSource>("setSourceType:","v@:i").m<&source>("sourceType","i@:")
      .m<&setMedia>("setMediaTypes:","v@:@").m<&getMedia>("mediaTypes")
      .m<&setEditing>("setAllowsEditing:","v@:c").m<&setEditing>("setAllowsImageEditing:","v@:c")
      .m<&editing>("allowsEditing","c@:").m<&editing>("allowsImageEditing","c@:");
    ip.m<&dealloc>("dealloc","v@:");
    ip.method("setCameraDevice:",[](Ctx& x){picker(x,x.cpu.r[0]).cameraDevice=int32_t(x.cpu.r[2]);},"v@:i");
    ip.method("cameraDevice",[](Ctx& x){x.cpu.r[0]=picker(x,x.cpu.r[0]).cameraDevice;},"i@:");
    ip.method("setVideoQuality:",[](Ctx& x){picker(x,x.cpu.r[0]).videoQuality=int32_t(x.cpu.r[2]);},"v@:i");
    ip.method("setShowsCameraControls:",[](Ctx& x){picker(x,x.cpu.r[0]).showsCamera=bool(x.cpu.r[2]);},"v@:c");
    ip.method("setCameraOverlayView:",[](Ctx& x){auto& d=picker(x,x.cpu.r[0]);id old=d.overlay;d.overlay=x.cpu.r[2]?rt(x).retain(x.cpu,x.cpu.r[2]):0;if(old)rt(x).release(x.cpu,old);},"v@:@");
    for(const char* name:{"UIImagePickerControllerOriginalImage","UIImagePickerControllerEditedImage","UIImagePickerControllerMediaType",
                         "UIImagePickerControllerCropRect","UIImagePickerControllerMediaURL","UIImagePickerControllerReferenceURL","UIImagePickerControllerMediaMetadata"})
        p.mem().write<uint32_t>(p.defineDataBlock(std::string("_")+name,4),Foundation::constString(p,name));
}
} // namespace Hydrogen::Runtime::UIKit
