cat << 'DIFF' | patch -p1
--- a/src/runtime/Process.cpp
+++ b/src/runtime/Process.cpp
@@ -165,15 +165,19 @@
         liveness_->alive = false;
     }
     requestStop();
-    // Wait (bounded) for guest threads to notice and exit.
+    // Wait (bounded) for guest threads to notice and exit, enforcing cancellation on blocked workers
     for (int i = 0; i < 200; ++i) {
         std::shared_ptr<GuestThread> activeThread;
         {
             std::lock_guard<std::mutex> lk(threadsMu_);
             for (auto& [k, t] : threads_) {
                 if (!t->finished.load()) { activeThread = t; break; }
             }
         }
         if (!activeThread) break;
-        std::this_thread::sleep_for(std::chrono::milliseconds(10));
+        std::unique_lock<std::mutex> lk(activeThread->m);
+        if (!activeThread->cv.wait_for(lk, std::chrono::milliseconds(10), [&] { return activeThread->finished.load(); })) {
+            if (i == 199 && activeThread->hostNative) pthread_cancel(activeThread->hostNative);
+        }
     }
+    for (auto& [k, t] : threads_) { if (t->hostNative) pthread_detach(t->hostNative); }
     if (t_thread && &t_thread->proc == this) t_thread = nullptr;
 }
DIFF
