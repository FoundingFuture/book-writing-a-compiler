/* posix_spawn and waitpid are POSIX, outside the C11 library. */
#define _POSIX_C_SOURCE 200809L

#include "process.h"

#include <stdio.h>

#if defined(_WIN32)

/* DESIGN: Windows starts processes with CreateProcess, which chapter 16
   adds together with the Windows targets. */
int process_run(const char *const argv[])
{
    fprintf(stderr, "antic: cannot run %s: not implemented on Windows yet\n",
            argv[0]);
    return -1;
}

int process_capture(const char *const argv[], struct text *out)
{
    (void)out;
    return process_run(argv);
}

#else

#include <errno.h>
#include <spawn.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

static int wait_for(pid_t pid, const char *name)
{
    int status;

    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) {
            fprintf(stderr, "antic: waiting for %s: %s\n", name,
                    strerror(errno));
            return -1;
        }
    }
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    fprintf(stderr, "antic: %s ended by signal %d\n", name, WTERMSIG(status));
    return -1;
}

/* Start argv[0] with the given file actions. posix_spawnp searches PATH
   the way a shell does. */
static int spawn(const char *const argv[], posix_spawn_file_actions_t *actions,
                 pid_t *pid)
{
    int err = posix_spawnp(pid, argv[0], actions, NULL,
                           (char *const *)argv, environ);
    if (err != 0) {
        fprintf(stderr, "antic: cannot run %s: %s\n", argv[0], strerror(err));
        return -1;
    }
    return 0;
}

int process_run(const char *const argv[])
{
    pid_t pid;

    if (spawn(argv, NULL, &pid) != 0) {
        return -1;
    }
    return wait_for(pid, argv[0]);
}

int process_capture(const char *const argv[], struct text *out)
{
    posix_spawn_file_actions_t actions;
    int fds[2];
    pid_t pid;
    char buffer[4096];
    ssize_t n;
    int started;

    if (pipe(fds) != 0) {
        fprintf(stderr, "antic: pipe: %s\n", strerror(errno));
        return -1;
    }
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, fds[1], STDOUT_FILENO);
    posix_spawn_file_actions_addclose(&actions, fds[0]);
    posix_spawn_file_actions_addclose(&actions, fds[1]);
    started = spawn(argv, &actions, &pid);
    posix_spawn_file_actions_destroy(&actions);
    close(fds[1]);
    if (started != 0) {
        close(fds[0]);
        return -1;
    }
    while ((n = read(fds[0], buffer, sizeof buffer - 1)) > 0) {
        buffer[n] = '\0';
        text_append(out, buffer);
    }
    close(fds[0]);
    return wait_for(pid, argv[0]);
}

#endif
