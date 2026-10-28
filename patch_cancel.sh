cat << 'DIFF' | patch -p1
--- a/src/runtime/Process.hpp
+++ b/src/runtime/Process.hpp
@@ -73,6 +73,7 @@
     uint32_t stackBase = 0, stackSize = 0;
     uint32_t errnoAddr = 0;
     std::unordered_map<uint32_t, uint32_t> tsd;   // pthread keys
+    pthread_t hostNative = 0;
     std::thread host;
     std::atomic<bool> finished{false};
     bool detached = false;
--- a/src/runtime/Process.cpp
+++ b/src/runtime/Process.cpp
@@ -83,18 +83,19 @@

 // Host threads for guest threads get a generous stack: nested host↔guest
 // calls (ObjC dispatch from host code, callbacks) recurse on it.
-void spawnHostThread(std::function<void()> fn) {
+pthread_t spawnHostThread(std::function<void()> fn) {
     pthread_attr_t attr;
     pthread_attr_init(&attr);
     pthread_attr_setstacksize(&attr, 8u << 20);
-    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
+    // Let the destructor explicitly cancel or detach if needed
     auto* heapFn = new std::function<void()>(std::move(fn));
     pthread_t th;
     const int r = pthread_create(&th, &attr, [](void* p) -> void* {
         std::unique_ptr<std::function<void()>> f(static_cast<std::function<void()>*>(p));
         (*f)();
         return nullptr;
     }, heapFn);
     pthread_attr_destroy(&attr);
     if (r != 0) { delete heapFn; throw std::runtime_error("couldn't create host thread"); }
+    return th;
 }

 } // namespace
@@ -174,7 +175,12 @@
         }
         if (!activeThread) break;
-        std::this_thread::sleep_for(std::chrono::milliseconds(10));
+        std::unique_lock<std::mutex> lk(activeThread->m);
+        if (!activeThread->cv.wait_for(lk, std::chrono::milliseconds(10), [&] { return activeThread->finished.load(); })) {
+            if (i == 199 && activeThread->hostNative) {
+                pthread_cancel(activeThread->hostNative);
+            }
+        }
     }
+    for (auto& [k, t] : threads_) { if (t->hostNative) pthread_detach(t->hostNative); }
     if (t_thread && &t_thread->proc == this) t_thread = nullptr;
 }
@@ -698,7 +704,7 @@
         std::lock_guard<std::mutex> lk(threadsMu_);
         threads_[raw->self] = t;
     }
-    spawnHostThread([this, raw, fn, arg, alive = liveness_, threadRef = std::move(t)] {
+    raw->hostNative = spawnHostThread([this, raw, fn, arg, alive = liveness_, threadRef = t] {
         {
             std::lock_guard<std::mutex> lk(alive->mu);
             if (!alive->alive) {
                 std::lock_guard<std::mutex> tlk(threadRef->m);
DIFF
