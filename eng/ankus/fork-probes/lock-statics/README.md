# Lock statics initialize inside class constructors

Native AOT reads the lock spin settings when a lock is first contended, and that
can happen inside any class constructor. Class constructors themselves take the
runtime's class-construction locks, so two threads that run class constructors
at the same moment contend there first.

This host loads a Native AOT library 200 times, each in a fresh process with a
30-second deadline. In each one, the host thread constructs `Encoding.UTF8`
while a second thread, released at the same instant, runs the encoder
replacement fallback's class constructor. Every process must finish both class
constructors and exit normally.

Before the correction the settings were read through
`Environment.GetEnvironmentVariable`, which marshals the variable name through
`Encoding.UTF8` on Unix, and the first `AppContext` access decoded the runtime
configuration through it too. When the contention happened inside UTF8Encoding's
class constructor, either access found `Encoding.UTF8` still null, and the
resulting `TypeInitializationException` aborted the process without a message.
On Linux x64, 52 to 62 of the 200 processes aborted in each of three runs.
The corrected runtime passed 10,000 processes on Linux x64, half of them pinned
to four CPUs. On macOS ARM64, 200 processes passed on a partial correction that
still aborted on Linux, so this check finds the race on Linux.

After building the Release Native AOT runtime and CoreLib, on Linux x64:

```sh
dotnet publish -c Release -r linux-x64 -o publish -p:IlcSdkPath="$ANKUS_AOT_SDK/"
cc -std=gnu17 -O2 -Wall -Wextra -Werror host.c -ldl -o host
./host "$PWD/publish/NativeLockStaticsProbe.so"
```

On macOS, use `osx-arm64` or `osx-x64`, omit `-ldl` and select the `.dylib`
library.
