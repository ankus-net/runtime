// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

#include <dlfcn.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

typedef int (*race_fn)(void);

// Each run needs a fresh process: the race depends on class constructors and lock statics that run only once.
#define RUNS 200

static void pause_milliseconds(long milliseconds)
{
    struct timespec remaining = {milliseconds / 1000, (milliseconds % 1000) * 1000000};
    while (nanosleep(&remaining, &remaining) == -1 && errno == EINTR)
    {
    }
}

static int run_case(const char* library_path)
{
    void* library = dlopen(library_path, RTLD_NOW | RTLD_LOCAL);
    if (library == NULL)
    {
        fprintf(stderr, "%s\n", dlerror());
        return 11;
    }

    race_fn race = (race_fn)dlsym(library, "lock_statics_race");
    if (race == NULL)
    {
        return 12;
    }

    return race() == 0 ? 73 : 1;
}

// Runs one case with a 30-second deadline, so a hang is a bounded failure.
static int supervise(const char* host, const char* library, int run, int* reported)
{
    fflush(NULL);
    pid_t child = fork();
    if (child == -1)
    {
        return 0;
    }

    if (child == 0)
    {
        execl(host, host, library, "--case", (char*)NULL);
        _exit(4);
    }

    int status = 0;
    pid_t waited = 0;
    for (int elapsed = 0; elapsed < 3000 && waited == 0; elapsed++)
    {
        waited = waitpid(child, &status, WNOHANG);
        if (waited == 0)
        {
            pause_milliseconds(10);
        }
    }

    if (waited == 0)
    {
        kill(child, SIGKILL);
        waitpid(child, &status, 0);
        printf("lock statics run %d: FAIL (timed out)\n", run);
        return 0;
    }

    int passed = waited == child && WIFEXITED(status) && WEXITSTATUS(status) == 73;
    if (!passed && (*reported)++ < 5)
    {
        printf("lock statics run %d: FAIL (wait status %d)\n", run, status);
    }

    return passed;
}

int main(int argc, char** argv)
{
    if (argc == 3 && strcmp(argv[2], "--case") == 0)
    {
        return run_case(argv[1]);
    }

    if (argc != 2)
    {
        fprintf(stderr, "usage: %s library\n", argv[0]);
        return 2;
    }

    int failures = 0;
    int reported = 0;
    for (int run = 0; run < RUNS; run++)
    {
        failures += !supervise(argv[0], argv[1], run, &reported);
    }

    printf("lock statics: %d of %d runs failed\n", failures, RUNS);
    return failures == 0 ? 0 : 1;
}
