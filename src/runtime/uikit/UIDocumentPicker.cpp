#include "UIKit.hpp"
#include <atomic>
#include <filesystem>
#include <fstream>

namespace Hydrogen::Runtime::UIKit {
namespace {
struct DocumentData : ObjC::HostObject {
    id delegate = 0;
    std::vector<std::string> types;
    int32_t mode = 0;
};
std::atomic<uint64_t> nextDocumentRequest{1};
DocumentData& document(Ctx& x, id self) { return rt(x).host<DocumentData>(self); }
id initialize(Ctx& x, id self, SEL, id types, int32_t mode) {
    auto& d = document(x, self);
    d.mode = mode;
    for (id type : Foundation::arrayItems(x, types)) d.types.push_back(Foundation::utf8(x, type));
    return self;
}
void setDelegate(Ctx& x, id self, SEL, id value) { document(x, self).delegate = value; }
id getDelegate(Ctx& x, id self, SEL) { return document(x, self).delegate; }
int32_t getMode(Ctx& x, id self, SEL) { return document(x, self).mode; }
void publish(Ctx& x, bool active) {
    std::function<void(const DocumentPickerRequest&)> observer;
    { std::lock_guard<std::mutex> lock(ui(x).inputMu); observer = ui(x).documentPickerObserver; }
    if (observer) observer({active, ui(x).documentRequestId, active ? document(x, ui(x).activeDocumentPicker).types : std::vector<std::string>{}});
}
}
void setDocumentPickerObserver(Process& p, std::function<void(const DocumentPickerRequest&)> observer) {
    std::lock_guard<std::mutex> lock(ui(p).inputMu);
    ui(p).documentPickerObserver = std::move(observer);
}
bool presentDocumentPicker(Ctx& x, id controller) {
    if (!rt(x).isKindOf(controller, rt(x).getClass("UIDocumentPickerViewController"))) return false;
    auto& u = ui(x);
    if (u.activeDocumentPicker) return true;
    u.activeDocumentPicker = rt(x).retain(x.cpu, controller);
    u.documentRequestId = nextDocumentRequest.fetch_add(1);
    u.documentResultDelivered = false;
    bool supported;
    { std::lock_guard<std::mutex> lock(u.inputMu); supported = bool(u.documentPickerObserver); }
    if (supported && document(x, controller).mode == 0) publish(x, true);
    else submitDocumentPickerResult(x.proc, u.documentRequestId, {});
    return true;
}
void dismissDocumentPicker(Ctx& x, id controller) {
    auto& u = ui(x);
    if (u.activeDocumentPicker != controller) return;
    publish(x, false);
    u.activeDocumentPicker = 0;
    u.documentResultDelivered = true;
    rt(x).release(x.cpu, controller);
}
void submitDocumentPickerResult(Process& p, uint64_t requestId, std::string sourcePath) {
    Process* process = &p;
    Foundation::mainRunLoop(p).post([process, requestId, sourcePath = std::move(sourcePath)](CPU::Cpu& cpu) {
        Ctx x{*process, cpu, process->mem()};
        auto& u = ui(x);
        if (!u.activeDocumentPicker || requestId != u.documentRequestId || u.documentResultDelivered) return;
        u.documentResultDelivered = true;
        const id controller = u.activeDocumentPicker, delegate = document(x, controller).delegate;
        namespace fs = std::filesystem;
        std::error_code ec;
        fs::path source(sourcePath), destination;
        const auto size = sourcePath.empty() ? 0 : fs::file_size(source, ec);
        bool imported = !ec && size > 0 && size <= 16u * 1024 * 1024 && fs::is_regular_file(source, ec);
        if (imported) {
            const fs::path parent = fs::path(process->homePath()) / "tmp";
            fs::create_directories(parent, ec);
            fs::path directory;
            imported = false;
            if (!ec) for (unsigned attempt = 0; attempt < 4096; ++attempt) {
                directory = parent / ("document-" + std::to_string(requestId) + "-" + std::to_string(attempt));
                if (fs::create_directory(directory, ec)) { imported = true; break; }
                if (ec) break;
            }
            if (imported) {
                destination = directory / source.filename();
                imported = fs::copy_file(source, destination, fs::copy_options::none, ec) && !ec;
            }
        }
        if (imported) {
            const id url = Foundation::makeFileURL(x, destination.string());
            if (delegate && rt(x).respondsTo(delegate, rt(x).sel("documentPicker:didPickDocumentsAtURLs:"))) {
                const id urls = Foundation::makeArray(x, {url});
                rt(x).send(cpu, delegate, "documentPicker:didPickDocumentsAtURLs:", {controller, urls});
            } else if (delegate && rt(x).respondsTo(delegate, rt(x).sel("documentPicker:didPickDocumentAtURL:"))) {
                rt(x).send(cpu, delegate, "documentPicker:didPickDocumentAtURL:", {controller, url});
            }
        } else if (delegate && rt(x).respondsTo(delegate, rt(x).sel("documentPickerWasCancelled:"))) {
            rt(x).send(cpu, delegate, "documentPickerWasCancelled:", {controller});
        }
    });
}
void registerDocumentPicker(Process&, ObjC::Runtime& r) {
    auto picker = r.defineClass("UIDocumentPickerViewController", "UIViewController");
    picker.m<&initialize>("initWithDocumentTypes:inMode:", "@@:@i")
          .m<&getDelegate>("delegate").m<&setDelegate>("setDelegate:", "v@:@")
          .m<&getMode>("documentPickerMode", "i@:");
}
}
