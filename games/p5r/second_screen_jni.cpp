// JNI bridge between the game's second_screen C++ state and SecondScreenService (Java).
// SecondScreenService calls nativeGetSnapshot() on its 10 Hz polling thread.
// The returned int array layout:
//   [0]  valid (1/0)
//   [1]  in_battle (1/0)
//   per slot 0..3 (8 ints each, starting at index 2 + slot*8):
//   [+0] active (1/0)
//   [+1] member id
//   [+2] hp
//   [+3] hp_max
//   [+4] sp
//   [+5] sp_max
//   [+6] level
//   [+7] reserved (0)
#if defined(__ANDROID__)
#include "second_screen.h"
#include <jni.h>

extern "C" JNIEXPORT jintArray JNICALL
Java_io_github_p5rrecomp_SecondScreenService_nativeGetSnapshot(JNIEnv* env, jobject /*thiz*/) {
    p5r::second_screen::GameStateSnapshot snap = p5r::second_screen::consume_snapshot();

    constexpr int kSize = 2 + 4 * 8;
    jint buf[kSize] = {};
    buf[0] = snap.valid    ? 1 : 0;
    buf[1] = snap.in_battle ? 1 : 0;
    for (int s = 0; s < 4; s++) {
        const auto& m = snap.party[s];
        int* b = buf + 2 + s * 8;
        b[0] = m.active ? 1 : 0;
        b[1] = m.id;
        b[2] = m.hp;
        b[3] = m.hp_max;
        b[4] = m.sp;
        b[5] = m.sp_max;
        b[6] = m.level;
        b[7] = 0;
    }

    jintArray arr = env->NewIntArray(kSize);
    if (arr) env->SetIntArrayRegion(arr, 0, kSize, buf);
    return arr;
}
#endif
