using System;
using System.Collections.Generic;
using System.Linq;
using Mono.Cecil;
using Mono.Cecil.Cil;

// Makes the Android platform lifecycle portable to the native Linux host:
//   * save paths use launcher-provided HOME (with a device-neutral cwd fallback);
//   * save_config is restored as a local Config.txt serializer;
//   * the in-game Quit action exits after the game's normal shutdown sequence.
// The complete method bodies are rebuilt so this tool also upgrades the v2 RC,
// where save_config had intentionally been replaced with a no-op.
internal static class FixPlatform
{
    private static int Main(string[] args)
    {
        if (args.Length != 1)
        {
            Console.Error.WriteLine("usage: fixplatform <SOR4.dll>");
            return 2;
        }

        using var assembly = AssemblyDefinition.ReadAssembly(
            args[0], new ReaderParameters { ReadWrite = true });
        var module = assembly.MainModule;
        var platform = AllTypes(module).SingleOrDefault(t => t.FullName == "CommonLib.platform");
        var serializer = AllTypes(module).SingleOrDefault(t => t.FullName == "CommonLib.simple_serializer");
        var program = AllTypes(module).SingleOrDefault(t => t.FullName == "BeatThemAll.MetaGame.program");
        if (platform is null || serializer is null || program is null)
        {
            Console.Error.WriteLine("fixplatform: required SOR4 platform types were not found");
            return 3;
        }

        var getSavePath = platform.Methods.SingleOrDefault(m =>
            m.Name == "get_save_file_path" && m.IsStatic &&
            m.Parameters.Count == 1 && m.Parameters[0].ParameterType.FullName == "System.String" &&
            m.ReturnType.FullName == "System.String");
        var saveConfig = platform.Methods.SingleOrDefault(m =>
            m.Name == "save_config" && m.IsStatic && m.Parameters.Count == 2 &&
            m.Parameters[1].ParameterType.FullName == "System.Boolean");
        var serialize = serializer.Methods.SingleOrDefault(m =>
            m.Name == "serialize" && m.IsStatic && m.Parameters.Count == 3 &&
            m.Parameters[1].ParameterType.FullName == "System.String" &&
            m.Parameters[2].ParameterType.FullName == "System.Boolean" &&
            m.ReturnType.FullName == "System.Void");
        var shutdown = program.Methods.SingleOrDefault(m =>
            m.Name == "shutdown" && m.IsStatic && m.Parameters.Count == 0 && m.HasBody);
        if (getSavePath is null || saveConfig is null || serialize is null || shutdown is null)
        {
            Console.Error.WriteLine("fixplatform: required SOR4 platform methods were not found");
            return 4;
        }

        RewriteSavePath(module, getSavePath);
        RewriteSaveConfig(module, saveConfig, getSavePath, serialize);
        if (!RewriteQuit(module, shutdown))
            return 5;

        assembly.Write();
        Console.WriteLine("fixplatform: save path, Config.txt and in-game Quit patched");
        return 0;
    }

    private static void RewriteSavePath(ModuleDefinition module, MethodDefinition method)
    {
        ResetBody(method);
        var getEnvironmentVariable = module.ImportReference(typeof(Environment).GetMethod(
            nameof(Environment.GetEnvironmentVariable), new[] { typeof(string) }));
        var getCurrentDirectory = module.ImportReference(typeof(Environment).GetProperty(
            nameof(Environment.CurrentDirectory))!.GetMethod);
        var isNullOrEmpty = module.ImportReference(typeof(string).GetMethod(
            nameof(string.IsNullOrEmpty), new[] { typeof(string) }));
        var combine = module.ImportReference(typeof(System.IO.Path).GetMethod(
            nameof(System.IO.Path.Combine), new[] { typeof(string), typeof(string) }));

        var il = method.Body.GetILProcessor();
        var haveHome = il.Create(OpCodes.Nop);
        il.Append(il.Create(OpCodes.Ldstr, "HOME"));
        il.Append(il.Create(OpCodes.Call, getEnvironmentVariable));
        il.Append(il.Create(OpCodes.Dup));
        il.Append(il.Create(OpCodes.Call, isNullOrEmpty));
        il.Append(il.Create(OpCodes.Brfalse, haveHome));
        il.Append(il.Create(OpCodes.Pop));
        il.Append(il.Create(OpCodes.Call, getCurrentDirectory));
        il.Append(haveHome);
        il.Append(il.Create(OpCodes.Ldarg_0));
        il.Append(il.Create(OpCodes.Call, combine));
        il.Append(il.Create(OpCodes.Ret));
    }

    private static void RewriteSaveConfig(
        ModuleDefinition module, MethodDefinition method,
        MethodDefinition getSavePath, MethodDefinition serialize)
    {
        ResetBody(method);
        var il = method.Body.GetILProcessor();
        il.Append(il.Create(OpCodes.Ldarg_0));
        il.Append(il.Create(OpCodes.Ldstr, "Config.txt"));
        il.Append(il.Create(OpCodes.Call, module.ImportReference(getSavePath)));
        il.Append(il.Create(OpCodes.Ldarg_1));
        il.Append(il.Create(OpCodes.Call, module.ImportReference(serialize)));
        il.Append(il.Create(OpCodes.Ret));
    }

    private static bool RewriteQuit(ModuleDefinition module, MethodDefinition shutdown)
    {
        var exit = module.ImportReference(typeof(Environment).GetMethod(
            nameof(Environment.Exit), new[] { typeof(int) }));
        var calls = shutdown.Body.Instructions.Where(i =>
            (i.OpCode == OpCodes.Call || i.OpCode == OpCodes.Callvirt) &&
            i.Operand is MethodReference mr &&
            ((mr.DeclaringType.FullName == "Android.OS.Process" && mr.Name == "KillProcess") ||
             (mr.DeclaringType.FullName == "System.Environment" && mr.Name == "Exit")))
            .ToList();
        if (calls.Count != 1)
        {
            Console.Error.WriteLine($"fixplatform: expected one shutdown exit call, found {calls.Count}");
            return false;
        }
        calls[0].OpCode = OpCodes.Call;
        calls[0].Operand = exit;
        return true;
    }

    private static void ResetBody(MethodDefinition method)
    {
        method.Body.Instructions.Clear();
        method.Body.Variables.Clear();
        method.Body.ExceptionHandlers.Clear();
        method.Body.InitLocals = false;
        method.Body.MaxStackSize = 8;
    }

    private static IEnumerable<TypeDefinition> AllTypes(ModuleDefinition module)
    {
        foreach (var type in module.Types)
        {
            yield return type;
            foreach (var nested in NestedTypes(type))
                yield return nested;
        }
    }

    private static IEnumerable<TypeDefinition> NestedTypes(TypeDefinition type)
    {
        foreach (var nested in type.NestedTypes)
        {
            yield return nested;
            foreach (var child in NestedTypes(nested))
                yield return child;
        }
    }
}
