// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

typedef int (*probe_fn)(void);
typedef int (*call_fn)(int);
typedef int (*create_fn)(pthread_t*, const pthread_attr_t*, void* (*)(void*), void*);

// On a busy machine a new thread can wait seconds before it first runs. Linked with -rdynamic, this definition
// replaces pthread_create for the runtime, and while delays are on each new thread sleeps before it starts,
// longer than the runtime once allowed for its threads to attach.
static atomic_int s_delay_new_threads;
static const long s_delay_ms = 11000;

typedef struct
{
    void* (*start)(void*);
    void* argument;
} delayed_start;

static void* start_after_delay(void* state)
{
    delayed_start start = *(delayed_start*)state;
    free(state);
    struct timespec delay = {s_delay_ms / 1000, (s_delay_ms % 1000) * 1000000};
    while (nanosleep(&delay, &delay) == -1 && errno == EINTR)
    {
    }

    return start.start(start.argument);
}

int pthread_create(pthread_t* thread, const pthread_attr_t* attributes, void* (*start)(void*), void* argument)
{
    static create_fn create;
    if (create == NULL)
    {
        create = (create_fn)dlsym(RTLD_NEXT, "pthread_create");
    }

    if (!atomic_load(&s_delay_new_threads))
    {
        return create(thread, attributes, start, argument);
    }

    delayed_start* state = malloc(sizeof(delayed_start));
    if (state == NULL)
    {
        return ENOMEM;
    }

    state->start = start;
    state->argument = argument;
    int result = create(thread, attributes, start_after_delay, state);
    if (result != 0)
    {
        free(state);
    }

    return result;
}

static int run_case(const char* library_path, const char* mode)
{
    // Each case delays at most four threads by 11 seconds; anything slower is a failure.
    alarm(120);
    if (strcmp(mode, "enable") == 0)
    {
        atomic_store(&s_delay_new_threads, 1);
    }

    void* library = dlopen(library_path, RTLD_NOW | RTLD_LOCAL);
    if (library == NULL)
    {
        fprintf(stderr, "%s\n", dlerror());
        return 10;
    }

    probe_fn initialize = (probe_fn)dlsym(library, "delayed_probe_initialize");
    call_fn call = (call_fn)dlsym(library, "delayed_probe_call");
    probe_fn enable = (probe_fn)dlsym(library, "RhEnableForkSupport");
    probe_fn enter = (probe_fn)dlsym(library, "RhEnterForkHost");
    probe_fn leave = (probe_fn)dlsym(library, "RhExitForkHost");
    if (initialize == NULL || call == NULL || enable == NULL || enter == NULL || leave == NULL)
    {
        return 11;
    }

    if (initialize() != 0 || enable() != 1)
    {
        return 12;
    }

    // Each managed callback starts a replacement finalizer, and leaving it waits for that thread to attach.
    atomic_store(&s_delay_new_threads, strcmp(mode, "cycles") == 0);
    for (int cycle = 0; cycle < 2; cycle++)
    {
        if (enter() != 1 || call(cycle) != cycle + 1 || leave() != 1)
        {
            return 13;
        }
    }

    pid_t child = fork();
    if (child == -1)
    {
        return 14;
    }

    if (child == 0)
    {
        int ok = enter() == 1 && call(41) == 42 && leave() == 1;
        _exit(ok ? 0 : 1);
    }

    int status;
    pid_t waited;
    do
    {
        waited = waitpid(child, &status, 0);
    } while (waited == -1 && errno == EINTR);

    return waited == child && WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : 15;
}

// Runs a case in a fresh process and requires success and a report of the long wait in its standard error.
static int run_isolated(const char* self, const char* library_path, const char* mode)
{
    int pipes[2];
    if (pipe(pipes) != 0)
    {
        return 0;
    }

    pid_t child = fork();
    if (child == -1)
    {
        return 0;
    }

    if (child == 0)
    {
        dup2(pipes[1], STDERR_FILENO);
        close(pipes[0]);
        close(pipes[1]);
        execl(self, self, library_path, mode, (char*)NULL);
        _exit(16);
    }

    close(pipes[1]);
    char output[16384];
    size_t used = 0;
    ssize_t read_bytes;
    while ((read_bytes = read(pipes[0], output + used, sizeof(output) - 1 - used)) != 0)
    {
        if (read_bytes == -1)
        {
            if (errno == EINTR)
            {
                continue;
            }

            break;
        }

        used += (size_t)read_bytes;
        if (used == sizeof(output) - 1)
        {
            break;
        }
    }

    output[used] = '\0';
    close(pipes[0]);
    int status;
    pid_t waited;
    do
    {
        waited = waitpid(child, &status, 0);
    } while (waited == -1 && errno == EINTR);

    int succeeded = waited == child && WIFEXITED(status) && WEXITSTATUS(status) == 0;
    int reported = strstr(output, "NativeAOT fork support: still waiting for") != NULL;
    printf("%s: %s (wait status %d, waiting reported: %s)\n", mode, succeeded && reported ? "PASS" : "FAIL",
        waited == child ? status : -1, reported ? "yes" : "no");
    if (!(succeeded && reported))
    {
        fputs(output, stdout);
    }

    return succeeded && reported;
}

int main(int argc, char** argv)
{
    if (argc == 3)
    {
        return run_case(argv[1], argv[2]);
    }

    if (argc != 2)
    {
        fprintf(stderr, "usage: %s library\n", argv[0]);
        return 17;
    }

    int passed = run_isolated(argv[0], argv[1], "enable");
    passed &= run_isolated(argv[0], argv[1], "cycles");
    return passed ? 0 : 1;
}
