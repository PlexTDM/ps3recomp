/*
 * Even presents: see rsx_present_pace.h.
 */
#include "rsx_present_pace.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#endif

static volatile int s_host_on = 0;          /* what the host asked for */
static volatile float s_host_period = 0.0f; /* the spacing it named, seconds; 0 unknown */
static volatile int s_hurry = 0;            /* show the frame that is being held now */
static int s_env = -1;                      /* RSX_PRESENT_PACE: -1 unread, 0 off, 1 on, 2 unset */
static int s_log = 0;

void rsx_present_pace_enable(int on) { s_host_on = on ? 1 : 0; }

void rsx_present_pace_period(double seconds)
{
    s_host_period = seconds > 0.0005 && seconds < 0.2 ? (float)seconds : 0.0f;
}

void rsx_present_pace_hurry(int on) { s_hurry = on ? 1 : 0; }

static void read_env(void)
{
    const char* e = getenv("RSX_PRESENT_PACE");
    const char* l = getenv("RSX_PRESENT_PACE_LOG");
    s_log = l ? atoi(l) : 0;
    s_env = !e || !*e ? 2 : (e[0] == '0' ? 0 : 1);
}

int rsx_present_pace_on(void)
{
    if (s_env < 0) read_env();
    return s_env == 1 || (s_env == 2 && s_host_on);
}

static double now_s(void)
{
#ifdef _WIN32
    static double inv = 0.0;
    LARGE_INTEGER q;
    if (inv == 0.0) { QueryPerformanceFrequency(&q); inv = 1.0 / (double)q.QuadPart; }
    QueryPerformanceCounter(&q);
    return (double)q.QuadPart * inv;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
#endif
}

/* A millisecond at most, and not exactly: the caller looks at the clock. */
static void doze(double seconds)
{
#ifdef _WIN32
    static HANDLE timer = NULL;
    if (!timer) {
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
        timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        if (!timer) timer = CreateWaitableTimerW(NULL, TRUE, NULL);
    }
    if (timer) {
        LARGE_INTEGER due;
        due.QuadPart = -(LONGLONG)(seconds * 1e7);   /* 100 ns units, relative */
        if (SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE)) { WaitForSingleObject(timer, 20); return; }
    }
    Sleep(1);
#else
    struct timespec ts;
    ts.tv_sec = 0;
    ts.tv_nsec = (long)(seconds * 1e9);
    nanosleep(&ts, NULL);
#endif
}

/* Until t, or until the frame is asked for: 1 when it was. Sleeps in slices
 * while there is more than a millisecond and a half to go and spins through
 * the rest: a timer alone comes back up to a millisecond late, which is the
 * unevenness this is here to remove. */
static int wait_until(double t)
{
    for (;;) {
        if (s_hurry) return 1;
        const double left = t - now_s();
        if (left <= 0.0) return 0;
        if (left > 0.0015) doze(left - 0.0008 < 0.001 ? left - 0.0008 : 0.001);
#ifdef _WIN32
        else YieldProcessor();
#endif
    }
}

void rsx_present_pace(void)
{
    enum { HIST = 8 };
    static double hist[HIST];          /* the last arrival intervals */
    static int hist_n, hist_at;
    static double last;                /* the last arrival */
    static double period, jitter;      /* the frames' spacing and its mean deviation */
    static double due;                 /* the schedule: when the last frame was due */
    static double wait_avg;
    static double shown;               /* when the last frame was let through */
    /* the report */
    static double rep_next, rep_wait, rep_in2, rep_out2, rep_in1, rep_out1, rep_max;
    static unsigned rep_n, rep_late, rep_cut, rep_free, rep_in_big, rep_out_big;
    static double prev_in, prev_out;

    if (!rsx_present_pace_on()) { last = 0.0; return; }

    const double a = now_s();
    const double d = a - last;
    const int fresh = last <= 0.0 || d <= 0.0 || d > 0.1;   /* the first frame, or the first after a gap */
    last = a;
    if (fresh) {
        hist_n = 0; period = 0.0; due = a; shown = a; wait_avg = 0.0; prev_in = prev_out = 0.0;
        return;
    }
    hist[hist_at] = d; hist_at = (hist_at + 1) % HIST;
    if (hist_n < HIST) hist_n++;
    if (hist_n < 4) { due = a; shown = a; return; }

    /* The spacing of the arrivals: the median of the last few, so that one
     * long frame does not move it and a change of frame rate does at once. */
    double m[HIST];
    memcpy(m, hist, sizeof(double) * (size_t)hist_n);
    for (int i = 1; i < hist_n; i++) {
        const double v = m[i];
        int j = i - 1;
        while (j >= 0 && m[j] > v) { m[j + 1] = m[j]; j--; }
        m[j + 1] = v;
    }
    const double med = (hist_n & 1) ? m[hist_n / 2] : 0.5 * (m[hist_n / 2 - 1] + m[hist_n / 2]);

    /* The spacing to schedule by. A host that caps the frame rate names it,
     * and it is taken while the frames bear it out. When they come slower
     * than that, the title is not making its frames in time and there is no
     * slack to pace them with: they are shown as they come. (When they come
     * faster, the name is out of date, and the arrivals are measured.) */
    const double told = (double)s_host_period;
    int free_run = 0;
    if (told > 0.0 && med >= 0.8 * told) {
        if (med > 1.2 * told) free_run = 1;
        else {
            if (period != told) { due = a; wait_avg = 0.0; if (jitter <= 0.0 || jitter > 0.3 * told) jitter = 0.1 * told; }
            period = told;
        }
    } else {
        if (period <= 0.0 || med < 0.6 * period || med > 1.6 * period) {   /* a different frame rate: from here */
            period = med; jitter = 0.1 * med; due = a; wait_avg = 0.0;
        }
        period += (med - period) * 0.2;
    }

    double w = 0.0, target = 0.0;
    int late = 0, cut = 0;
    if (free_run) {
        due = a; wait_avg = 0.0;
    } else {
        double dev = fabs(d - period);
        if (dev > 0.5 * period) dev = 0.5 * period;
        jitter += (dev - jitter) * 0.05;

        /* How far behind the frames the schedule runs: three times their
         * usual deviation, at most four tenths of a frame and 8 ms. */
        target = 3.0 * jitter;
        if (target < 0.0003) target = 0.0003;
        if (target > 0.4 * period) target = 0.4 * period;
        if (target > 0.008) target = 0.008;

        due += period;
        w = due - a;
        wait_avg += (w - wait_avg) * 0.1;
        due -= 0.2 * (wait_avg - target);   /* steer the usual wait to the target */
        w = due - a;
        if (w <= 0.0) { due = a; w = 0.0; late = 1; }
        else if (w > 2.0 * target + 0.001) { due = a + target; w = target; wait_avg = target; }   /* the schedule has fallen behind the frames */
        if (w > 0.0 && wait_until(due)) { cut = 1; due = now_s(); wait_avg = 0.0; }
    }
    const double out = w > 0.0 ? now_s() : a;

    if (s_log > 0) {
        const double in_iv = d, out_iv = out - shown;
        rep_n++; rep_late += (unsigned)late; rep_cut += (unsigned)cut; rep_free += (unsigned)free_run; rep_wait += out - a;
        if (out - a > rep_max) rep_max = out - a;
        rep_in1 += in_iv; rep_in2 += in_iv * in_iv; rep_out1 += out_iv; rep_out2 += out_iv * out_iv;
        if (prev_in > 0.0 && (in_iv > 1.15 * prev_in || in_iv < 0.87 * prev_in)) rep_in_big++;
        if (prev_out > 0.0 && (out_iv > 1.15 * prev_out || out_iv < 0.87 * prev_out)) rep_out_big++;
        prev_in = in_iv; prev_out = out_iv;
        if (s_log > 1)
            fprintf(stderr, "[pace] in %.3f out %.3f wait %.3f ms%s\n", in_iv * 1e3, out_iv * 1e3, (out - a) * 1e3,
                    late ? " late" : cut ? " cut" : free_run ? " free" : "");
        if (a >= rep_next && rep_n > 1) {
            const double n = (double)rep_n;
            const double mi = rep_in1 / n, mo = rep_out1 / n;
            const double si = sqrt(fabs(rep_in2 / n - mi * mi)), so = sqrt(fabs(rep_out2 / n - mo * mo));
            fprintf(stderr, "[pace] %u frames %.2f ms apart (scheduled by %.2f): arrivals vary %.2f ms (%u%% over 15%% off the one before), "
                            "presents %.2f ms (%u%%); wait %.2f ms mean, %.2f at most; %u late, %u cut short, %u unpaced\n",
                    rep_n, mo * 1e3, period * 1e3, si * 1e3, (unsigned)(100.0 * rep_in_big / n + 0.5), so * 1e3,
                    (unsigned)(100.0 * rep_out_big / n + 0.5), rep_wait / n * 1e3, rep_max * 1e3, rep_late, rep_cut, rep_free);
            rep_next = a + 2.0;
            rep_n = rep_late = rep_cut = rep_free = rep_in_big = rep_out_big = 0;
            rep_wait = rep_in1 = rep_in2 = rep_out1 = rep_out2 = rep_max = 0.0;
        }
    }
    shown = out;
}
