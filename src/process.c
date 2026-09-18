/* posix_spawn and waitpid are POSIX, outside the C11 library. */
#define _POSIX_C_SOURCE 200809L

#include "process.h"

#include <stdio.h>

#if defined(_WIN32)

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

/* DESIGN: Windows takes one command line where POSIX takes a list, and
   the C runtime of the program parses it back. An argument is quoted by
   those rules: a run of backslashes doubles before a quote, and a quote
   of the argument itself is escaped. */
static void quote(const char *arg, struct text *out)
{
    size_t i;

    if (arg[0] != '\0' && strpbrk(arg, " \t\n\v\"") == NULL) {
        text_append(out, arg);
        return;
    }
    text_append(out, "\"");
    for (i = 0; arg[i] != '\0';) {
        size_t slashes = 0;
        while (arg[i] == '\\') {
            slashes++;
            i++;
        }
        if (arg[i] == '\0' || arg[i] == '"') {
            slashes *= 2;
        }
        while (slashes-- > 0) {
            text_append(out, "\\");
        }
        if (arg[i] == '\0') {
            break;
        }
        if (arg[i] == '"') {
            text_append(out, "\\");
        }
        text_append_bytes(out, arg + i, 1);
        i++;
    }
    text_append(out, "\"");
}

/* The UTF-16 of text, which every wide Windows function takes. antic
   holds its strings as UTF-8. */
static wchar_t *widen(const char *text)
{
    int count = MultiByteToWideChar(CP_UTF8, 0, text, -1, NULL, 0);
    wchar_t *wide;

    if (count <= 0) {
        return NULL;
    }
    wide = malloc((size_t)count * sizeof *wide);
    if (wide == NULL) {
        return NULL;
    }
    if (MultiByteToWideChar(CP_UTF8, 0, text, -1, wide, count) <= 0) {
        free(wide);
        return NULL;
    }
    return wide;
}

static int wait_for(HANDLE process, const char *name)
{
    DWORD code = 0;

    if (WaitForSingleObject(process, INFINITE) != WAIT_OBJECT_0 ||
        !GetExitCodeProcess(process, &code)) {
        fprintf(stderr, "antic: waiting for %s: error %lu\n", name,
                (unsigned long)GetLastError());
        return -1;
    }
    return (int)code;
}

/* Start argv[0] with the command line that argv spells. Without an
   application name CreateProcessW searches PATH and appends .exe, as a
   shell does. */
static int start(const char *const argv[], HANDLE output,
                 PROCESS_INFORMATION *info)
{
    struct text line = {0};
    STARTUPINFOW startup;
    wchar_t *wide;
    size_t i;
    BOOL started;

    for (i = 0; argv[i] != NULL; i++) {
        if (i > 0) {
            text_append(&line, " ");
        }
        quote(argv[i], &line);
    }
    wide = widen(text_cstr(&line));
    text_free(&line);
    if (wide == NULL) {
        fprintf(stderr, "antic: cannot run %s: out of memory\n", argv[0]);
        return -1;
    }
    memset(&startup, 0, sizeof startup);
    startup.cb = sizeof startup;
    if (output != NULL) {
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        startup.hStdOutput = output;
        startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    }
    started = CreateProcessW(NULL, wide, NULL, NULL, output != NULL, 0, NULL,
                             NULL, &startup, info);
    free(wide);
    if (!started) {
        fprintf(stderr, "antic: cannot run %s: error %lu\n", argv[0],
                (unsigned long)GetLastError());
        return -1;
    }
    return 0;
}

int process_run(const char *const argv[])
{
    PROCESS_INFORMATION info;
    int status;

    if (start(argv, NULL, &info) != 0) {
        return -1;
    }
    status = wait_for(info.hProcess, argv[0]);
    CloseHandle(info.hProcess);
    CloseHandle(info.hThread);
    return status;
}

int process_capture(const char *const argv[], struct text *out)
{
    SECURITY_ATTRIBUTES attributes;
    PROCESS_INFORMATION info;
    HANDLE reader;
    HANDLE writer;
    char buffer[4096];
    DWORD read_bytes;
    int status;

    memset(&attributes, 0, sizeof attributes);
    attributes.nLength = sizeof attributes;
    attributes.bInheritHandle = TRUE;
    if (!CreatePipe(&reader, &writer, &attributes, 0)) {
        fprintf(stderr, "antic: pipe: error %lu\n",
                (unsigned long)GetLastError());
        return -1;
    }
    /* The child writes into the pipe and never reads from it. */
    SetHandleInformation(reader, HANDLE_FLAG_INHERIT, 0);
    if (start(argv, writer, &info) != 0) {
        CloseHandle(reader);
        CloseHandle(writer);
        return -1;
    }
    CloseHandle(writer);
    while (ReadFile(reader, buffer, sizeof buffer - 1, &read_bytes, NULL) &&
           read_bytes > 0) {
        buffer[read_bytes] = '\0';
        text_append(out, buffer);
    }
    CloseHandle(reader);
    status = wait_for(info.hProcess, argv[0]);
    CloseHandle(info.hProcess);
    CloseHandle(info.hThread);
    return status;
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
