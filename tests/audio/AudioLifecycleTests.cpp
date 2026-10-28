#include "runtime/audio/Audio.hpp"
#include <iostream>
#include <memory>

using namespace Hydrogen::Runtime::Audio;

static void assert_eq(size_t a, size_t b, const char* msg) {
    if (a != b) {
        std::cerr << "FAIL: " << msg << " (" << a << " != " << b << ")" << std::endl;
        exit(1);
    }
}

static void assert_true(bool a, const char* msg) {
    if (!a) {
        std::cerr << "FAIL: " << msg << std::endl;
        exit(1);
    }
}

static Pcm make_dummy_pcm(size_t frames) {
    auto pcm = std::make_shared<std::vector<int16_t>>();
    pcm->resize(frames * 2, 0); // stereo
    return pcm;
}

int main() {
    Mixer mixer;
    float left[1024];
    float right[1024];

    // Test 1: autoRelease frees voices after playback
    {
        uint32_t voice = mixer.play(make_dummy_pcm(512), 44100, {}, 0, true);
        assert_true(mixer.playing(voice), "Voice should be playing");
        mixer.render(left, right, 512); // Finish playing
        assert_true(!mixer.playing(voice), "Voice should be stopped after playback");
        // Verify voice was erased (autoRelease)
        mixer.render(left, right, 1024);
        assert_true(!mixer.playing(voice), "Voice should be fully released");
    }

    // Test 2: explicit release
    {
        uint32_t voice = mixer.play(make_dummy_pcm(2048), 44100, {}, 0, false);
        mixer.render(left, right, 512);
        assert_true(mixer.playing(voice), "Voice should still be playing");
        mixer.release(voice);
        assert_true(!mixer.playing(voice), "Voice should be released");
    }

    // Test 3: stop clears queue
    {
        uint32_t voice = mixer.play(make_dummy_pcm(2048), 44100, {}, 0, false);
        mixer.stop(voice);
        assert_true(!mixer.playing(voice), "Voice should be stopped");
        mixer.release(voice);
    }

    // Test 4: empty buffers do not crash
    {
        uint32_t voice = mixer.play(make_dummy_pcm(0), 44100, {}, 0, true);
        mixer.render(left, right, 512); // Will process the empty buffer and release
        assert_true(!mixer.playing(voice), "Voice should be released");
    }

    std::cout << "All AudioLifecycleTests passed!" << std::endl;
    return 0;
}
