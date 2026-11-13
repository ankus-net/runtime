# Delayed runtime threads

On a busy machine a newly created thread can stay runnable for seconds before it
first runs. Each wait in fork support is for another runtime thread that needs
only CPU time to finish, such as a replacement finalizer attaching after a host
callback, so a fixed limit turned a scheduling delay into a failed process. This
host replaces `pthread_create` and holds each new runtime thread for 11 seconds
before it starts, past the roughly 10 seconds the runtime once allowed.

The `enable` case delays the first finalizer, which enabling fork support waits
for. The `cycles` case delays the replacement finalizer that each of two host
callbacks starts, then checks that a forked child can still call managed code; a
child is not the host, so nothing there waits for its new finalizer. Each case
must exit 0 and report in standard error what it waited for. The runtime before this change fails both
with `NativeAOT fork support: host service threads did not detach.` or an enable
result of -4.

On Linux x64, after building the Release Native AOT runtime and CoreLib:

```sh
dotnet publish -c Release -r linux-x64 -o publish -p:IlcSdkPath="$ANKUS_AOT_SDK/"
cc -std=gnu17 -O2 -Wall -Wextra -Werror host.c -ldl -pthread -rdynamic -o host
./host "$PWD/publish/DelayedThreadsProbe.so"
```

The run takes about 35 seconds, since each case waits out its delayed threads.
