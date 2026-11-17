/* Unit test for the engine's speaker output stage (DC blocker + soft-knee gain).
 * Build: see tests/run_audio_stage_test.sh */
#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include "audio.h"

int main(void)
{
    enum { N = 223721 };                     /* one second at the engine's mix rate */
    static int16_t buf[N * 2];
    audio_set_output_stage(20, 200);
    /* 440 Hz tone at amplitude 6000 riding on a DC offset that steps 0 -> 3000 at 0.5 s */
    for (int i = 0; i < N; i++) {
        double dc = i < N / 2 ? 0 : 3000;
        int16_t v = (int16_t)(dc + 6000 * sin(2 * M_PI * 440 * i / (double)N));
        buf[i * 2] = buf[i * 2 + 1] = v;
    }
    audio_output_stage_process(buf, N, N);
    double mean = 0, peak = 0;
    for (int i = N * 3 / 4; i < N; i++) {     /* last quarter: well after the step */
        mean += buf[i * 2];
        if (fabs((double)buf[i * 2]) > peak) peak = fabs((double)buf[i * 2]);
    }
    mean /= N / 4;
    /* loud input must approach but never exceed full scale */
    int16_t loud[4] = {30000, -30000, 32767, -32768};
    audio_set_output_stage(0, 200);
    audio_output_stage_process(loud, 2, N);
    int ok = fabs(mean) < 60 && peak > 11000 && peak < 12500 &&
             loud[0] > 30000 && loud[0] <= 32767 && loud[1] < -30000 && loud[2] <= 32767;
    printf("dc after step %.1f, tone peak %.0f (in 6000, gain 2x), loud 30000 -> %d, 32767 -> %d: %s\n",
           mean, peak, loud[0], loud[2], ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
