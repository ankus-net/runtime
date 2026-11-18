// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text;

namespace NativeLockStaticsProbe;

/// <summary>
/// Makes the first contended lock in the process occur while UTF8Encoding's class constructor runs.
/// </summary>
public static class Probe
{
    /// <summary>
    /// Set once the other thread waits for the release.
    /// </summary>
    private static volatile bool s_ready;

    /// <summary>
    /// Releases both threads together.
    /// </summary>
    private static volatile bool s_go;

    /// <summary>
    /// Keeps the other thread's result reachable.
    /// </summary>
    private static object? s_fallback;

    /// <summary>
    /// Constructs UTF8Encoding while another thread runs a different class constructor.
    /// </summary>
    /// <returns>Zero when both class constructors complete.</returns>
    [UnmanagedCallersOnly(EntryPoint = "lock_statics_race", CallConvs = [typeof(CallConvCdecl)])]
    public static int Race()
    {
        Thread other = new(ConstructFallback) { IsBackground = true };
        other.Start();
        while (!s_ready)
        {
        }

        s_go = true;
        Encoding utf8 = Encoding.UTF8;
        other.Join();
        return utf8 is not null && s_fallback is not null ? 0 : 1;
    }

    /// <summary>
    /// Waits for the release, then runs the encoder replacement fallback's class constructor.
    /// </summary>
    private static void ConstructFallback()
    {
        s_ready = true;
        while (!s_go)
        {
        }

        s_fallback = EncoderFallback.ReplacementFallback;
    }
}
