// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

using System.Collections.Generic;
using System.Runtime;
using System.Runtime.ExceptionServices;
using System.Text.Unicode;

namespace System
{
    public static partial class AppContext
    {
        private static unsafe Dictionary<string, object?> InitializeDataStore()
        {
            uint count = RuntimeImports.RhGetKnobValues(out byte** keys, out byte** values);

            var dataStore = new Dictionary<string, object?>((int)count);
            for (int i = 0; i < count; i++)
            {
                dataStore.Add(DecodeKnob(keys[i]), DecodeKnob(values[i]));
            }

            return dataStore;
        }

        // A lock can first be contended inside UTF8Encoding's class constructor, and initializing the lock's statics
        // reads AppContext, so decode without Encoding.UTF8. Invalid sequences become U+FFFD as Encoding.UTF8 does.
        private static unsafe string DecodeKnob(byte* knob)
        {
            ReadOnlySpan<byte> bytes = new ReadOnlySpan<byte>(knob, string.strlen(knob));
            char[] chars = new char[bytes.Length];
            Utf8.ToUtf16(bytes, chars, out _, out int written);
            return new string(chars, 0, written);
        }

        [RuntimeExport("OnFirstChanceException")]
        internal static void OnFirstChanceException(object e)
        {
            FirstChanceException?.Invoke(/* AppDomain */ null, new FirstChanceExceptionEventArgs((Exception)e));
        }
    }
}
