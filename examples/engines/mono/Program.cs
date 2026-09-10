// SPDX-License-Identifier: GPL-3.0-only
using System;
using System.Runtime.InteropServices;
class Training {
    [StructLayout(LayoutKind.Sequential)] public struct Sample { public int X, Y; }
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate int Visitor(int value);
    [DllImport("nextos_training", CallingConvention=CallingConvention.Cdecl)]
    static extern int nextos_transform(Sample input, out Sample output, Visitor callback);
    static int Main() {
        var input=new Sample {X=7,Y=9};Sample output;
        Visitor callback=delegate(int x) {return x+1;};
        int status=nextos_transform(input,out output,callback);
        GC.KeepAlive(callback);
        if(status!=0 || output.X!=8 || output.Y!=9) return 1;
        input.X=2000;
        if(nextos_transform(input,out output,callback)!=-1) return 2;
        Console.WriteLine("PASS: managed/native layout, callback lifetime and error propagation; Linux host only");
        return 0;
    }
}
