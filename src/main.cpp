#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <fmod.hpp>
#include <thread>
#include <atomic>
#include <cmath>
#include <chrono>

using namespace geode::prelude;

static std::atomic<bool> g_running(false);
static std::atomic<bool> g_inLevel(false);
static std::atomic<bool> g_pressing(false);
static std::atomic<bool> g_threadReady(false);
static std::thread g_micThread;
static bool g_debug = false; // set to true if u need to see what the mic is picking up

static FMOD::System* g_fmodSystem = nullptr;
static FMOD::Sound* g_recordSound = nullptr;
static int g_recordDevice = 0;
static const int RECORD_LEN = 1024 * 2;

// ok uhm i saw a lot of tiktoks of someone making an external program for this so i made it for geode .-. lol
// axiom was here
// mic detection is literally just comparing db levels, if peak is above threshold = jump. thats it

static void press() {
    if (!g_inLevel.load()) return;
    PlayLayer* pl = PlayLayer::get();
    if (!pl) return;
    if (!Mod::get()->getSettingValue<bool>("enabled")) return;
    pl->handleButton(true, (int)PlayerButton::Jump, true);
}

static void release() {
    if (!g_inLevel.load()) return;
    PlayLayer* pl = PlayLayer::get();
    if (!pl) return;
    pl->handleButton(false, (int)PlayerButton::Jump, true);
}

static float get_threshold() {
    return (float)Mod::get()->getSettingValue<double>("threshold-db");

static int find_mic_with_audio() {
    // fmod is stupid so we gotta do this manually
    int numDevices = 0;
    int numConnected = 0;
    g_fmodSystem->getRecordNumDrivers(&numDevices, &numConnected);

    for (int i = 0; i < numDevices; i++) {
        FMOD_CREATESOUNDEXINFO exparams;
        memset(&exparams, 0, sizeof(FMOD_CREATESOUNDEXINFO));
        exparams.cbsize = sizeof(FMOD_CREATESOUNDEXINFO);
        exparams.numchannels = 1;
        exparams.format = FMOD_SOUND_FORMAT_PCM16;
        exparams.defaultfrequency = 44100;
        exparams.length = RECORD_LEN * sizeof(short);

        FMOD::Sound* test_sound = nullptr;
        FMOD_RESULT res = g_fmodSystem->createSound(nullptr, FMOD_2D | FMOD_OPENUSER | FMOD_LOOP_NORMAL, &exparams, &test_sound);
        if (res != FMOD_OK) continue;

        res = g_fmodSystem->recordStart(i, test_sound, true);
        if (res != FMOD_OK) {
            test_sound->release();
            continue;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        unsigned int curr;
        g_fmodSystem->getRecordPosition(i, &curr);
        g_fmodSystem->recordStop(i);

        test_sound->release();
        if (curr > 0) {
            return i;
        }
    }

    return 0;// fallbackkkkkkkkkkkkkkkkkkk
}

static void mic_thread_func() {
    FMOD::System_Create(&g_fmodSystem);
    g_fmodSystem->init(1, FMOD_INIT_NORMAL, nullptr);

    g_recordDevice = find_mic_with_audio();

    FMOD_CREATESOUNDEXINFO exparams;
    memset(&exparams, 0, sizeof(FMOD_CREATESOUNDEXINFO));
    exparams.cbsize = sizeof(FMOD_CREATESOUNDEXINFO);
    exparams.numchannels = 1; // mono only, stereo was overkill
    exparams.format = FMOD_SOUND_FORMAT_PCM16;
    exparams.defaultfrequency = 44100; // tried 48k, didnt matter
    exparams.length = RECORD_LEN * sizeof(short);

    g_fmodSystem->createSound(nullptr, FMOD_2D | FMOD_OPENUSER | FMOD_LOOP_NORMAL, &exparams, &g_recordSound);
    g_fmodSystem->recordStart(g_recordDevice, g_recordSound, true);

    g_threadReady.store(true);

    unsigned int pos = 0;
    bool was_above = false;
    int release_counter = 0;
    const int RELEASE_FRAMES = 1;

    while (g_running.load()) {
        g_fmodSystem->update();

        unsigned int curr;
        g_fmodSystem->getRecordPosition(g_recordDevice, &curr);

        if (curr != pos) {
            void* p1 = nullptr;
            void* p2 = nullptr;
            unsigned int l1 = 0;
            unsigned int l2 = 0;
            int samples = (static_cast<int>(curr) - static_cast<int>(pos) + RECORD_LEN) % RECORD_LEN;

            if (samples > 0) {
                g_recordSound->lock(pos * sizeof(short), samples * sizeof(short), &p1, &p2, &l1, &l2);

                float peak = 0.0f;
                float sumsq = 0.0f;
                int cnt = 0;

                if (p1) {
                    short* buf = (short*)p1;
                    int n = l1 / sizeof(short);
                    for (int i = 0; i < n; i++) {
                        float s = buf[i] / 32768.0f;
                        float abss = s < 0.0f ? -s : s;
                        if (abss > peak) peak = abss;
                        sumsq += s * s;
                        cnt++;
                    }
                }
                if (p2) {
                    short* buf = (short*)p2;
                    int n = l2 / sizeof(short);
                    for (int i = 0; i < n; i++) {
                        float s = buf[i] / 32768.0f;
                        float abss = s < 0.0f ? -s : s;
                        if (abss > peak) peak = abss;
                        sumsq += s * s;
                        cnt++;
                    }
                }

                g_recordSound->unlock(p1, p2, l1, l2);

                float peak_db = (peak <= 0.0f) ? -100.0f : 20.0f * log10f(peak);
                float rms = (cnt > 0) ? sqrtf(sumsq / (float)cnt) : 0.0f;
                float rms_db = (rms <= 0.0f) ? -100.0f : 20.0f * log10f(rms);

                if (g_inLevel.load() && Mod::get()->getSettingValue<bool>("enabled")) {
                    float thresh = get_threshold();
                    float release_thresh = thresh - 12.0f;

                    bool peak_above = peak_db >= thresh;
                    bool rms_above = rms_db >= release_thresh;

                    if (peak_above && !was_above) {
                        was_above = true;
                        release_counter = 0;
                        g_pressing.store(true);
                        Loader::get()->queueInMainThread([]() {
                            if (g_inLevel.load()) press();
                        });

                    } else if (was_above && !peak_above && !rms_above) {
                        release_counter++;
                        if (release_counter >= RELEASE_FRAMES) {
                            was_above = false;
                            release_counter = 0;
                            g_pressing.store(false);
                            Loader::get()->queueInMainThread([]() {
                                if (g_inLevel.load()) release();
                            });
                        }

                    // still loud, keep holding
                    } else if (peak_above && was_above) {
                        release_counter = 0;
                    }
                } else if (!g_inLevel.load() && was_above) {
                    was_above = false;
                    release_counter = 0;
                    g_pressing.store(false);
                }
            }
            pos = curr;
        }
        // -sleep so were not burning cpu. 2ms seems like a good balance, talking from experience-, nvm going to 1ms
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    g_fmodSystem->recordStop(g_recordDevice);
    if (g_recordSound) {
        g_recordSound->release();
        g_recordSound = nullptr;
    }
    g_fmodSystem->release();
    g_fmodSystem = nullptr;
}

static void start_mic() {
    if (g_running.load()) return;
    g_running.store(true);
    g_threadReady.store(false);
    g_micThread = std::thread(mic_thread_func);
}

static void do_release() {
    if (g_pressing.load()) {
        g_pressing.store(false);
        release();
    }
}

class $modify(PlayLayer) {
    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;
        g_inLevel.store(true);
        return true;
    }

    void onExit() {
        g_inLevel.store(false);
        g_pressing.store(false);
        PlayLayer::onExit();
    }

    void resetLevel() {
        do_release();
        PlayLayer::resetLevel();
    }

    void destroyPlayer(PlayerObject* player, GameObject* object) {
        do_release();
        PlayLayer::destroyPlayer(player, object);
    }
};

$execute {
    start_mic();
}