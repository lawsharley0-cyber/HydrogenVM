#include "../hvm_android.h"

__attribute__((visibility("default")))
void Java_test_Launcher_check(void* env, void* cls, int stage) {
    (void)env;
    (void)cls;
    static int expected = 1;
    static int mismatch = 0;
    if (stage != expected++) mismatch = 1;
    if (stage == 7) {
        static const char passed[] = "ok DEX activity constructs and runs the ordered lifecycle\ndone: DEX activity\n";
        static const char failed[] = "FAIL DEX activity constructs and runs the ordered lifecycle\ndone: DEX activity\n";
        if (mismatch) write(1, failed, sizeof(failed) - 1);
        else write(1, passed, sizeof(passed) - 1);
    }
}
