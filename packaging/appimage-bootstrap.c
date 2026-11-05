// SPDX-License-Identifier: GPL-2.0-or-later
// Run the host's NVIDIA helper BEFORE nix-appimage enters its user namespace.
// The host helper retains its installed privileges; this program needs none.
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef NVIDIA_PROC
#define NVIDIA_PROC "/proc/driver/nvidia"
#endif
#ifndef NVIDIA_DEV
#define NVIDIA_DEV "/dev"
#endif
#ifndef NVIDIA_HELPER
#define NVIDIA_HELPER "/usr/bin/nvidia-modprobe"
#endif

static int device_exists(const char* name) {
    char path[PATH_MAX];
    struct stat st;
    snprintf(path, sizeof path, "%s/%s", NVIDIA_DEV, name);
    return stat(path, &st) == 0 && S_ISCHR(st.st_mode);
}

static int run_helper(const char* helper, const char* option, const char* value) {
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        // Do not inject Steam's or an enclosing application's libraries into
        // this host tool. The later packaged entry point cleans its own env.
        unsetenv("LD_LIBRARY_PATH");
        unsetenv("LD_PRELOAD");
        unsetenv("LD_AUDIT");
        alarm(15);
        execl(helper, helper, option, value, (char*)NULL);
        _exit(127);
    }
    int status;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) return -1;
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static void initialize_nvidia(void) {
    char path[PATH_MAX];
    snprintf(path, sizeof path, "%s/version", NVIDIA_PROC);
    if (access(path, R_OK) != 0) return; // no loaded proprietary NVIDIA driver
    const char* helper = NVIDIA_HELPER;
    if (access(helper, X_OK) != 0) {
        helper = "/run/wrappers/bin/nvidia-modprobe";
        if (access(helper, X_OK) != 0) {
            helper = "/run/current-system/sw/bin/nvidia-modprobe";
            if (access(helper, X_OK) != 0) {
                if (!device_exists("nvidiactl"))
                    fprintf(stderr, "NVIDIA: device nodes missing and the host nvidia-modprobe helper is unavailable\n");
                return;
            }
        }
    }
    int attempted = 0, complete = 1;
    if (!device_exists("nvidiactl")) {
        attempted = 1;
        run_helper(helper, "-c", "255");
    }
    // Use the driver's minor numbers rather than assuming that NVIDIA is GPU 0.
    snprintf(path, sizeof path, "%s/gpus", NVIDIA_PROC);
    DIR* directory = opendir(path);
    struct dirent* entry;
    while (directory && (entry = readdir(directory))) {
        if (entry->d_name[0] == '.') continue;
        snprintf(path, sizeof path, "%s/gpus/%s/information", NVIDIA_PROC, entry->d_name);
        FILE* info = fopen(path, "r");
        if (!info) continue;
        char line[512];
        while (fgets(line, sizeof line, info)) {
            unsigned minor;
            if (sscanf(line, "Device Minor: %u", &minor) != 1 || minor >= 255) continue;
            char device[32], number[16];
            snprintf(device, sizeof device, "nvidia%u", minor);
            snprintf(number, sizeof number, "%u", minor);
            if (!device_exists(device)) {
                attempted = 1;
                run_helper(helper, "-c", number);
                complete = complete && device_exists(device);
            }
            break;
        }
        fclose(info);
    }
    if (directory) closedir(directory);
    if (!device_exists("nvidia-modeset")) {
        attempted = 1;
        run_helper(helper, "-m", NULL);
    }
    if (attempted) {
        fprintf(stderr, "NVIDIA: host device initialization %s\n",
                complete && device_exists("nvidiactl") && device_exists("nvidia-modeset")
                    ? "finished" : "incomplete; check the host driver installation");
    }
}

int main(int argc, char** argv) {
    (void)argc;
    initialize_nvidia();
    char path[PATH_MAX];
    ssize_t size = readlink("/proc/self/exe", path, sizeof path - 1);
    if (size < 0) { perror("AppImage: executable path"); return 127; }
    path[size] = 0;
    char* slash = strrchr(path, '/');
    if (!slash || (size_t)(slash - path) + sizeof "/AppRun.nix" > sizeof path) return 127;
    strcpy(slash, "/AppRun.nix");
    execv(path, argv);
    perror("AppImage: AppRun.nix");
    return 127;
}
