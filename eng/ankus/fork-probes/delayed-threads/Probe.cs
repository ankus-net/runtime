// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

namespace DelayedThreadsProbe;

/// <summary>
/// Gives a dormant native host short managed callbacks, each of which starts a replacement finalizer that leaving
/// the callback must wait for.
/// </summary>
public static class Probe
{
    /// <summary>
    /// Attaches the native owner, which starts the runtime and its first finalizer.
    /// </summary>
    /// <returns>Zero on successful initialization.</returns>
    [UnmanagedCallersOnly(EntryPoint = "delayed_probe_initialize", CallConvs = [typeof(CallConvCdecl)])]
    public static int Initialize() => 0;

    /// <summary>
    /// Allocates and collects without waiting for finalizers, so the callback itself never waits for the delayed
    /// finalizer, and returns the next value.
    /// </summary>
    /// <param name="value">The value to increment.</param>
    /// <returns>The value plus one when the allocation survived the collection, otherwise a negative value.</returns>
    [UnmanagedCallersOnly(EntryPoint = "delayed_probe_call", CallConvs = [typeof(CallConvCdecl)])]
    public static int Call(int value)
    {
        byte[] payload = Allocate(value);
        GC.Collect();
        return payload[0] == (byte)value && payload[^1] == (byte)value ? value + 1 : -1;
    }

    /// <summary>
    /// Allocates an array marked with the value at both ends.
    /// </summary>
    /// <param name="value">The marker.</param>
    /// <returns>The array.</returns>
    [MethodImpl(MethodImplOptions.NoInlining)]
    private static byte[] Allocate(int value)
    {
        byte[] payload = new byte[64 * 1024];
        payload[0] = (byte)value;
        payload[^1] = (byte)value;
        return payload;
    }
}
