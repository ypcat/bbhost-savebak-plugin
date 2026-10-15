/* Save backup with the Xbox controller: hold LT + RT + View + Menu. At the
 * title screen, then press LT to restore or RT to back up. In the game, it
 * exits to the title at once, as System > Exit Game > YES does. */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <linux/input.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "bbhost_plugin.h"

#define SAVE_DIR "saves/SPRJ0005"
#define BACKUP_DIR "saves/bak"
#define MENU_SECONDS 10.0f
#define DONE_SECONDS 3.0f
#define RESCAN_SECONDS 2.0
#define MAX_PADS 4
/* The YES action of System > Exit Game (sub_200c9f0 builds the question). */
#define EXIT_GAME_YES 0x200d050

BB_PLUGIN_EXPORT const BbPluginInfo bb_plugin_info = {
    BB_PLUGIN_API_VERSION, 0, "savebak", "Save Backup", "1.1.0", "claude",
    "Hold LT + RT + View + Menu: at the title, LT restores the save and RT backs it up; in the game, exit to the title."};

typedef struct Pad {
    int fd;
    char path[32];
    int lt_max, rt_max;
} Pad;

static const BbHostApi* host;
static Pad pads[MAX_PADS];
static int pad_count;
static double last_scan = -RESCAN_SECONDS;
static int lt, rt, view, menu;  /* held now, on any pad */
static enum { IDLE, RELEASE, ARMED } state;
static double armed_at;
static int used;  /* the combination did something; cleared when all four are up */

static double now_seconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static int test_bit(const unsigned long* bits, int bit) {
    return (bits[bit / (8 * sizeof(long))] >> (bit % (8 * sizeof(long)))) & 1;
}

static int abs_max(int fd, int axis) {
    struct input_absinfo info;
    return ioctl(fd, EVIOCGABS(axis), &info) == 0 && info.maximum > 0 ? info.maximum : 1023;
}

static void scan_pads(void) {
    glob_t found;
    if (glob("/dev/input/event*", 0, NULL, &found) != 0)
        return;
    for (size_t i = 0; i < found.gl_pathc && pad_count < MAX_PADS; i++) {
        int known = 0;
        for (int p = 0; p < pad_count; p++)
            known |= strcmp(pads[p].path, found.gl_pathv[i]) == 0;
        if (known)
            continue;
        int fd = open(found.gl_pathv[i], O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0)
            continue;
        unsigned long keys[KEY_MAX / (8 * sizeof(long)) + 1] = {0};
        if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof keys), keys) < 0 || !test_bit(keys, BTN_MODE)) {
            close(fd);
            continue;
        }
        Pad* pad = &pads[pad_count++];
        pad->fd = fd;
        snprintf(pad->path, sizeof pad->path, "%s", found.gl_pathv[i]);
        pad->lt_max = abs_max(fd, ABS_Z);
        pad->rt_max = abs_max(fd, ABS_RZ);
        host->log("savebak: reading %s", pad->path);
    }
    globfree(&found);
}

static int copy_file(const char* from, const char* to) {
    char tmp[4200];
    snprintf(tmp, sizeof tmp, "%s.tmp", to);
    int in = open(from, O_RDONLY | O_CLOEXEC);
    if (in < 0)
        return -1;
    int out = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0664);
    if (out < 0) {
        close(in);
        return -1;
    }
    char buf[65536];
    ssize_t n;
    int ok = 1;
    while ((n = read(in, buf, sizeof buf)) > 0)
        ok &= write(out, buf, n) == n;
    ok &= n == 0;
    close(in);
    ok &= close(out) == 0;
    if (ok && rename(tmp, to) == 0)
        return 0;
    unlink(tmp);
    return -1;
}

/* Copies every file in `from` (relative to the data folder) into `to`. The
 * number of files, 0 when `from` does not exist, or -1 on an error. */
static int copy_dir(const char* from, const char* to) {
    char src[2048], dst[2048];
    snprintf(src, sizeof src, "%s/%s", host->data_dir(), from);
    snprintf(dst, sizeof dst, "%s/%s", host->data_dir(), to);
    mkdir(dst, 0775);
    DIR* dir = opendir(src);
    if (!dir)
        return errno == ENOENT ? 0 : -1;
    int count = 0;
    struct dirent* e;
    while ((e = readdir(dir))) {
        char a[4096], b[4096];
        struct stat st;
        snprintf(a, sizeof a, "%s/%s", src, e->d_name);
        snprintf(b, sizeof b, "%s/%s", dst, e->d_name);
        if (stat(a, &st) != 0 || !S_ISREG(st.st_mode))
            continue;
        if (copy_file(a, b) != 0) {
            host->log("savebak: cannot copy %s to %s: %s", a, b, strerror(errno));
            count = -1;
            break;
        }
        count++;
    }
    closedir(dir);
    return count;
}

static void run(const char* from, const char* to, const char* done, const char* empty) {
    int n = copy_dir(from, to);
    host->log("savebak: %s -> %s: %d files", from, to, n);
    if (n > 0)
        host->show_message(done, DONE_SECONDS);
    else
        host->show_message(n == 0 ? empty : "Copy failed", DONE_SECONDS);
}

/* Something pressed while the choice shows: LT restores, RT backs up, any
 * other button or the d-pad cancels. */
static void press(int which) {
    if (state != ARMED)
        return;
    state = IDLE;
    if (which == ABS_Z)
        run(BACKUP_DIR, SAVE_DIR, "Save loaded", "No backup");
    else if (which == ABS_RZ)
        run(SAVE_DIR, BACKUP_DIR, "Save backed up", "No save");
    else
        host->show_message("Cancelled", 1.0f);
}

static void read_pad(int p) {
    Pad* pad = &pads[p];
    struct input_event ev[64];
    ssize_t n;
    while ((n = read(pad->fd, ev, sizeof ev)) > 0) {
        for (size_t i = 0; i < n / sizeof ev[0]; i++) {
            const struct input_event* e = &ev[i];
            if (e->type == EV_KEY) {
                if (e->code == BTN_SELECT)
                    view = e->value != 0;
                else if (e->code == BTN_START)
                    menu = e->value != 0;
                if (e->value == 1)
                    press(e->code);
            } else if (e->type == EV_ABS) {
                if (e->code == ABS_Z || e->code == ABS_RZ) {
                    int* held = e->code == ABS_Z ? &lt : &rt;
                    int down = e->value > (e->code == ABS_Z ? pad->lt_max : pad->rt_max) / 2;
                    if (down && !*held)
                        press(e->code);
                    *held = down;
                } else if ((e->code == ABS_HAT0X || e->code == ABS_HAT0Y) && e->value != 0) {
                    press(e->code);
                }
            }
        }
    }
    if (n < 0 && errno != EAGAIN) {  /* unplugged */
        host->log("savebak: lost %s", pad->path);
        close(pad->fd);
        pads[p] = pads[--pad_count];
        lt = rt = view = menu = 0;
    }
}

/* The title screen has no player in the world. player_stat_get does not tell:
 * the game loads the character's numbers at the title, for Continue. */
static int at_title(void) {
    return host->player_position(NULL, NULL) != 0;
}

static void exit_to_title(void) {
    if (!host->eboot_is_109())
        return;
    BbCallRegs regs;
    memset(&regs, 0, sizeof regs);
    host->log("savebak: exit to the title (call_guest %d)", host->call_guest(EXIT_GAME_YES, &regs));
}

static void frame(void* user) {
    (void)user;
    double now = now_seconds();
    if (now - last_scan >= RESCAN_SECONDS) {
        last_scan = now;
        scan_pads();
    }
    for (int p = pad_count - 1; p >= 0; p--)
        read_pad(p);

    if (state != IDLE && (now - armed_at > MENU_SECONDS || !at_title()))
        state = IDLE;
    if (!lt && !rt && !view && !menu)
        used = 0;
    if (state == IDLE && lt && rt && view && menu && !used) {
        used = 1;
        if (!at_title()) {
            exit_to_title();
            return;
        }
        state = RELEASE;
        armed_at = now;
        host->show_message("LT=load RT=save", MENU_SECONDS);
    } else if (state == RELEASE && !lt && !rt && !view && !menu) {
        state = ARMED;
    }
}

BB_PLUGIN_EXPORT int bb_plugin_init(const BbHostApi* api) {
    if (api->version < 8) {
        api->log("savebak: needs plugin API 8 or newer, host has %u", api->version);
        return 1;
    }
    host = api;
    return 0;
}

BB_PLUGIN_EXPORT int bb_plugin_image(const BbHostApi* api) {
    return api->on_frame(frame, NULL);
}
