#include <dirent.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>

/* Passive GC topology is exported only by the owned probe binary. */
typedef int32_t (*heap_count_fn)(int32_t maximum);
static heap_count_fn collector_heap_count;
static int collector_maximum;
static bool collector_server;

#ifdef __linux__
/* Linux sets PF_EXITING when a thread enters do_exit, before pthread_join can return for it, and lists the
 * thread in /proc/self/task until do_exit releases it. A thread the runtime has joined can still be listed for
 * a moment, but it has left user code and holds no lock. Returns 1 for such a thread, 0 for a live one and -1 when
 * its state can't be read. */
static int
thread_has_exited(int thread)
{
    int stat = openat(thread, "stat", O_RDONLY | O_CLOEXEC);
    if (stat < 0)
    {
        return errno == ENOENT || errno == ESRCH ? 1 : -1;
    }

    char buffer[512] = {0};
    ssize_t length;
    do
    {
        length = read(stat, buffer, sizeof(buffer) - 1);
    } while (length < 0 && errno == EINTR);

    int read_error = errno;
    close(stat);
    if (length < 0)
    {
        return read_error == ESRCH ? 1 : -1;
    }

    /* The name can hold spaces and parentheses, so the fields start after its last parenthesis. */
    char *fields = strrchr(buffer, ')');
    char state;
    unsigned long flags;
    if (fields == NULL || sscanf(fields + 1, " %c %*d %*d %*d %*d %*d %lu", &state, &flags) != 2)
    {
        return -1;
    }

    const unsigned long exiting = 0x4;
    return (flags & exiting) != 0 || state == 'X' || state == 'Z' ? 1 : 0;
}
#endif

/* Read native thread names without entering the managed runtime. Threads that have exited are not counted. */
static int
count_named_threads(const char *expected_name)
{
#ifdef __linux__
    DIR *directory = opendir("/proc/self/task");
    if (directory == NULL)
    {
        return -1;
    }

    int count = 0;
    struct dirent *entry;
    for (;;)
    {
        errno = 0;
        entry = readdir(directory);
        if (entry == NULL)
        {
            if (errno != 0)
            {
                count = -1;
            }

            break;
        }

        if (entry->d_name[0] < '0' || entry->d_name[0] > '9')
        {
            continue;
        }

        int thread = openat(dirfd(directory), entry->d_name, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (thread < 0)
        {
            if (errno == ENOENT)
            {
                continue;
            }

            count = -1;
            break;
        }

        int name = openat(thread, "comm", O_RDONLY | O_CLOEXEC);
        int open_error = errno;
        if (name < 0)
        {
            close(thread);
            if (open_error == ENOENT || open_error == ESRCH)
            {
                continue;
            }

            count = -1;
            break;
        }

        char buffer[16] = {0};
        ssize_t length;
        do
        {
            length = read(name, buffer, sizeof(buffer) - 1);
        } while (length < 0 && errno == EINTR);

        int read_error = errno;
        close(name);
        if (length < 0 && read_error != ESRCH)
        {
            close(thread);
            count = -1;
            break;
        }

        if (length != (ssize_t) strlen(expected_name) || memcmp(buffer, expected_name, (size_t) length) != 0)
        {
            close(thread);
            continue;
        }

        int exited = thread_has_exited(thread);
        close(thread);
        if (exited < 0)
        {
            count = -1;
            break;
        }

        if (exited == 0)
        {
            count++;
        }
    }

    closedir(directory);
    return count;
#else
    (void) expected_name;
    return -1;
#endif
}

/* Observe the exact prepare boundary, after runtime retirement and before fork. */
static void
capture_collector_checkpoint(void)
{
    int server = count_named_threads(".NET Server GC\n");
    int background = count_named_threads(".NET BGC\n");
    int active = collector_heap_count(0);
    int maximum = collector_heap_count(1);
    emit("checkpoint-server-threads", 0, server);
    emit("checkpoint-background-threads", 0, background);
    emit("checkpoint-active-heaps", 0, active);
    emit("checkpoint-maximum-heaps", 0, maximum);
    if (active < 1 || active > maximum || maximum != collector_maximum)
    {
        _exit(181);
    }

#ifdef __linux__
    if (server != 0 || background != 0)
    {
        _exit(182);
    }
#endif
}

/* Register before runtime prepare so reverse prepare order observes retired threads. */
static bool
initialize_collector_inventory(void *library, bool server)
{
    collector_heap_count = (heap_count_fn) dlsym(library, "RhGetForkGCHeapCount");
    if (collector_heap_count == NULL)
    {
        return false;
    }

    collector_server = server;
    collector_maximum = collector_heap_count(1);
    emit("initial-maximum-heaps", 0, collector_maximum);
    emit("initial-active-heaps", 0, collector_heap_count(0));
    if ((server && collector_maximum < 2) || (!server && collector_maximum != 1))
    {
        return false;
    }

    return pthread_atfork(capture_collector_checkpoint, NULL, NULL) == 0;
}

/* Require the original server-collector complement after managed recovery. */
static bool
check_collector_inventory(void)
{
    if (collector_heap_count == NULL)
    {
        return true;
    }

    int expected = collector_server ? collector_maximum : 0;
    int count = count_named_threads(".NET Server GC\n");
#ifdef __linux__
    for (int attempt = 0; count != expected && attempt < 1000; attempt++)
    {
        struct timespec pause = {0, 1000000};
        nanosleep(&pause, NULL);
        count = count_named_threads(".NET Server GC\n");
    }
#else
    (void) expected;
#endif
    emit("running-server-threads", 0, count);
    emit("running-active-heaps", 0, collector_heap_count(0));
#ifdef __linux__
    if (count != expected)
    {
        return false;
    }
#endif
    return collector_heap_count(1) == collector_maximum;
}
