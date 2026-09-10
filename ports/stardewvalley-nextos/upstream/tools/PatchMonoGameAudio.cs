using System;
using System.IO;
using System.Linq;
using Mono.Cecil;
using Mono.Cecil.Cil;

// Stardew Valley's Android MonoGame build forces every XACT stream into a
// MemoryStream.  music.xwb is about 247 MiB, so MemoryStream growth briefly
// needs almost two full copies and exhausts the small NextOS devices.
//
// Keep XNB loading on Android's AssetManager, but route XACT files through a
// normal seekable FileStream.  The launcher supplies a Content ->
// assets/Content symlink, so the original relative paths remain valid.
internal static class PatchMonoGameAudio
{
    private static MethodReference FindExistingFileOpenRead(ModuleDefinition module)
    {
        foreach (TypeDefinition type in module.Types)
        {
            MethodReference result = FindExistingFileOpenRead(type);
            if (result != null)
                return result;
        }

        throw new InvalidOperationException("System.IO.File.OpenRead reference not found");
    }

    private static MethodReference FindExistingPathCombine(ModuleDefinition module)
    {
        foreach (TypeDefinition type in module.Types)
        {
            MethodReference result = FindExistingPathCombine(type);
            if (result != null)
                return result;
        }

        throw new InvalidOperationException("System.IO.Path.Combine(string,string) reference not found");
    }

    private static MethodReference FindExistingPathCombine(TypeDefinition type)
    {
        foreach (MethodDefinition method in type.Methods)
        {
            if (!method.HasBody)
                continue;

            foreach (Instruction instruction in method.Body.Instructions)
            {
                MethodReference candidate = instruction.Operand as MethodReference;
                if (candidate != null &&
                    candidate.DeclaringType.FullName == "System.IO.Path" &&
                    candidate.Name == "Combine" &&
                    candidate.Parameters.Count == 2 &&
                    candidate.Parameters.All(parameter =>
                        parameter.ParameterType.FullName == "System.String"))
                    return candidate;
            }
        }

        foreach (TypeDefinition nested in type.NestedTypes)
        {
            MethodReference result = FindExistingPathCombine(nested);
            if (result != null)
                return result;
        }

        return null;
    }

    private static MethodReference FindExistingFileOpenRead(TypeDefinition type)
    {
        foreach (MethodDefinition method in type.Methods)
        {
            if (!method.HasBody)
                continue;

            foreach (Instruction instruction in method.Body.Instructions)
            {
                MethodReference candidate = instruction.Operand as MethodReference;
                if (candidate != null &&
                    candidate.DeclaringType.FullName == "System.IO.File" &&
                    candidate.Name == "OpenRead" &&
                    candidate.Parameters.Count == 1 &&
                    candidate.Parameters[0].ParameterType.FullName == "System.String")
                    return candidate;
            }
        }

        foreach (TypeDefinition nested in type.NestedTypes)
        {
            MethodReference result = FindExistingFileOpenRead(nested);
            if (result != null)
                return result;
        }

        return null;
    }

    private static int RedirectTitleContainerCalls(TypeDefinition type,
                                                    MethodReference assetOpenRead)
    {
        int patched = 0;
        foreach (MethodDefinition method in type.Methods)
        {
            if (!method.HasBody)
                continue;

            foreach (Instruction instruction in method.Body.Instructions)
            {
                MethodReference called = instruction.Operand as MethodReference;
                if (called == null ||
                    called.DeclaringType.FullName != "Microsoft.Xna.Framework.TitleContainer" ||
                    called.Name != "OpenStream")
                    continue;

                instruction.Operand = assetOpenRead;
                patched++;
            }
        }

        foreach (TypeDefinition nested in type.NestedTypes)
            patched += RedirectTitleContainerCalls(nested, assetOpenRead);

        return patched;
    }

    public static int Main(string[] args)
    {
        if (args.Length != 2)
        {
            Console.Error.WriteLine("usage: PatchMonoGameAudio <input.dll> <output.dll>");
            return 2;
        }

        AssemblyDefinition assembly = AssemblyDefinition.ReadAssembly(args[0]);
        ModuleDefinition module = assembly.MainModule;
        MethodReference fileOpenRead = FindExistingFileOpenRead(module);

        TypeDefinition audioEngine = module.GetType("Microsoft.Xna.Framework.Audio.AudioEngine");
        TypeDefinition waveBank = module.GetType("Microsoft.Xna.Framework.Audio.WaveBank");
        if (audioEngine == null || waveBank == null)
            throw new InvalidOperationException("MonoGame XACT types not found");

        MethodReference pathCombine = FindExistingPathCombine(module);
        MethodDefinition assetOpenRead = new MethodDefinition(
            "SdvOpenAssetFile",
            MethodAttributes.Assembly | MethodAttributes.Static | MethodAttributes.HideBySig,
            fileOpenRead.ReturnType);
        assetOpenRead.Parameters.Add(new ParameterDefinition(
            "relativePath", ParameterAttributes.None, module.TypeSystem.String));
        ILProcessor helperIl = assetOpenRead.Body.GetILProcessor();
        helperIl.Emit(OpCodes.Ldstr, "assets");
        helperIl.Emit(OpCodes.Ldarg_0);
        helperIl.Emit(OpCodes.Call, pathCombine);
        helperIl.Emit(OpCodes.Call, fileOpenRead);
        helperIl.Emit(OpCodes.Ret);
        audioEngine.Methods.Add(assetOpenRead);

        MethodDefinition openStream = audioEngine.Methods.Single(method =>
            method.Name == "OpenStream" && method.Parameters.Count == 2);

        int directCalls = RedirectTitleContainerCalls(audioEngine, assetOpenRead);
        directCalls += RedirectTitleContainerCalls(waveBank, assetOpenRead);

        // The original method overwrites useMemoryStream with true:
        //   ldc.i4.1; starg.s useMemoryStream
        // Make the assignment a no-op so callers can request a FileStream.
        Instruction forcedTrue = openStream.Body.Instructions.FirstOrDefault(instruction =>
            instruction.OpCode == OpCodes.Ldc_I4_1 &&
            instruction.Next != null &&
            instruction.Next.OpCode == OpCodes.Starg_S);
        if (forcedTrue == null)
            throw new InvalidOperationException("forced XACT MemoryStream sequence not found");
        forcedTrue.OpCode = OpCodes.Ldarg_1;
        forcedTrue.Operand = null;

        if (directCalls < 2)
            throw new InvalidOperationException("expected AudioEngine and WaveBank stream redirects");

        assembly.Write(args[1]);
        Console.WriteLine("patched {0} XACT file opens; disabled forced MemoryStream", directCalls);
        return 0;
    }
}
