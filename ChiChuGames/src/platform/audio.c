/* 扬声器 — 常驻 aplay 播放器 + PCM 音效表 (Kenney CC0, sfx_data.c)
 * 首次播放时 fork 一次 aplay, stdin 管道常开, O_NONBLOCK 写;
 * 短音效 chunk 即写即走, 游戏循环零停顿
 * 48kHz 双声道(单声样本复制 L/R), -D hw:0,0 直连硬件绕开 dmix 队列;
 * 零 malloc; host 下 stub */
#include "audio.h"
#include <stdbool.h>
#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <time.h>

/* ---- PCM 音效表 (tools/gen_sfx.py 生成, Kenney Interface Sounds CC0) ---- */
extern const int16_t g_sfx_tick[];   extern const uint32_t g_sfx_tick_len;
extern const int16_t g_sfx_move[];   extern const uint32_t g_sfx_move_len;
extern const int16_t g_sfx_select[]; extern const uint32_t g_sfx_select_len;
extern const int16_t g_sfx_clear[];  extern const uint32_t g_sfx_clear_len;
extern const int16_t g_sfx_error[];  extern const uint32_t g_sfx_error_len;
extern const int16_t g_sfx_win[];    extern const uint32_t g_sfx_win_len;
extern const int16_t g_sfx_lose[];   extern const uint32_t g_sfx_lose_len;

static int s_pipe = -1;
static uint32_t s_played, s_dropped;   /* 统计: 写入成功/丢弃 */

#ifndef CHICHU_HOST
static pid_t s_child = -1;
#endif

#ifdef CHICHU_HOST
/* /dev/null 播放器 — host 下只验写循环 */
static void player_start(void) {
    if (s_pipe < 0) s_pipe = open("/dev/null", O_WRONLY | O_NONBLOCK);
}
static void player_stop(void) {
    if (s_pipe >= 0) { close(s_pipe); s_pipe = -1; }
}
#else
static void player_start(void) {
    int fds[2];
    if (pipe(fds) != 0) return;
    pid_t pid = fork();
    if (pid < 0) { close(fds[0]); close(fds[1]); return; }
    if (pid == 0) {
        close(fds[1]);
        dup2(fds[0], STDIN_FILENO);
        close(fds[0]);
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }
        /* hw 直连(48k stereo), 绕开 default/dmix 混音队列(其缓冲可导致
         * 音效延迟数秒); -B 20ms/立即 start/500ms 停止延后: 单音效即时
         * 回放且不频繁启停(启停有 depop 瞬态=嘶哑) */
        execl("/usr/bin/aplay", "aplay", "-q", "-D", "hw:0,0",
              "-B", "20000", "-R", "0", "-T", "500000",
              "-f", "S16_LE", "-r", "48000", "-c", "2", "-t", "raw", NULL);
        _exit(127);
    }
    close(fds[0]);
    fcntl(fds[1], F_SETFL, O_NONBLOCK);   /* 满缓冲即放弃, 绝不阻塞 */
    /* 管道增容 64KB→1MB (48k stereo 96KB/s): 快速连键雨 5s 缓冲,
     * 避免 EAGAIN 丢音效(摇杆高速拨动"跳音频"根因) */
    fcntl(fds[1], F_SETPIPE_SZ, 1048576);
    s_pipe = fds[1];
    s_child = pid;
}

static bool reap_child_until(pid_t child, uint32_t timeout_ms) {
    struct timespec started;

    if (clock_gettime(CLOCK_MONOTONIC, &started) != 0) return false;
    for (;;) {
        int status = 0;
        pid_t result = waitpid(child, &status, WNOHANG);
        struct timespec now;
        uint64_t elapsed;

        if (result == child || (result < 0 && errno == ECHILD)) return true;
        if (result < 0 && errno != EINTR) return false;
        if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return false;
        elapsed = (uint64_t)(now.tv_sec - started.tv_sec) * 1000u;
        if (now.tv_nsec >= started.tv_nsec)
            elapsed += (uint64_t)(now.tv_nsec - started.tv_nsec) / 1000000u;
        else
            elapsed -= (uint64_t)(started.tv_nsec - now.tv_nsec) / 1000000u;
        if (elapsed >= timeout_ms) return false;
        usleep(2000);
    }
}

static void player_stop(void) {
    if (s_pipe >= 0) { close(s_pipe); s_pipe = -1; }
    if (s_child > 0) {
        if (!reap_child_until(s_child, 1500)) {
            (void)kill(s_child, SIGTERM);
            if (!reap_child_until(s_child, 250)) {
                int status = 0;
                (void)kill(s_child, SIGKILL);
                while (waitpid(s_child, &status, 0) < 0 && errno == EINTR) {
                }
            }
        }
        s_child = -1;
    }
}
#endif

/* PCM 播放: 单声样本复制为 stereo(48k), 整段写管道 */
static void play_pcm(const int16_t *pcm, uint32_t n) {
    if (n == 0) return;
    if (s_pipe < 0) player_start();
    if (s_pipe < 0) return;
#ifdef CCG_TIMELINE
    {
        extern void tm_mark(const char *);
        const char *nm = "?";
        if (pcm == g_sfx_tick) nm = "tick"; else if (pcm == g_sfx_move) nm = "move";
        else if (pcm == g_sfx_select) nm = "select"; else if (pcm == g_sfx_clear) nm = "clear";
        else if (pcm == g_sfx_error) nm = "error"; else if (pcm == g_sfx_win) nm = "win";
        else if (pcm == g_sfx_lose) nm = "lose";
        tm_mark(nm);
    }
#endif
    static int16_t sbuf[1024 * 2];      /* stereo chunk */
    uint32_t done = 0;
    while (done < n) {
        uint32_t m = n - done;
        if (m > 1024) m = 1024;
        for (uint32_t i = 0; i < m; i++) {
            sbuf[i * 2] = pcm[done + i];
            sbuf[i * 2 + 1] = pcm[done + i];
        }
        ssize_t w = write(s_pipe, sbuf, m * 4);
        if (w < 0) {
            s_dropped++;
            if (errno != EAGAIN && errno != EWOULDBLOCK) player_stop();  /* aplay 已退出 */
            return;
        }
        done += (uint32_t)w / 4;
    }
    s_played++;
}

/* ---- 对外 API (语义不变, 音色=真素材) ---- */
void audio_beep(uint16_t freq_hz, uint16_t dur_ms) {
    (void)freq_hz; (void)dur_ms;   /* 接口保留: 无人使用, 静默 */
}

void audio_tick(void)   { play_pcm(g_sfx_tick,   g_sfx_tick_len); }
void audio_move(void)   { play_pcm(g_sfx_move,   g_sfx_move_len); }
void audio_select(void) { play_pcm(g_sfx_select, g_sfx_select_len); }
void audio_clear(void)  { play_pcm(g_sfx_clear,  g_sfx_clear_len); }
void audio_error(void)  { play_pcm(g_sfx_error,  g_sfx_error_len); }
void audio_win(void)    { play_pcm(g_sfx_win,    g_sfx_win_len); }
void audio_lose(void)   { play_pcm(g_sfx_lose,   g_sfx_lose_len); }

void audio_cleanup(void) {
#ifndef CHICHU_HOST
    player_stop();
#endif
}

void audio_get_stats(audio_stats_t *st) {
    st->played = s_played;
    st->dropped = s_dropped;
}
