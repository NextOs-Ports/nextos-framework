using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text;
using Mono.Cecil;
using Mono.Cecil.Cil;

// Apply the small offline/controller fixes required by the NextOS loader
// without running a general-purpose assembly writer. Rewriting the DLL would
// disturb the Xamarin assembly-store/AOT identity, so this changes existing IL
// bytes in-place. Tokens, table shapes and the MVID remain untouched;
// The controller wrapper lives in the stripped CodeView record, which is part
// of the mapped .text section. This gives us a small, identity-preserving IL
// code cave without adding metadata rows or rewriting the assembly.
internal static class PatchStardewOsk
{
    private sealed class PeSection
    {
        public int HeaderOffset;
        public int VirtualSize;
        public int VirtualAddress;
        public int RawSize;
        public int RawOffset;
    }

    private sealed class CodeCave
    {
        public int FileOffset;
        public int Rva;
        public int Size;
    }

    private static int Align4(int value)
    {
        return (value + 3) & ~3;
    }

    private static PeSection[] ReadSections(byte[] image)
    {
        int pe = BitConverter.ToInt32(image, 0x3c);
        int sectionCount = BitConverter.ToUInt16(image, pe + 6);
        int optionalSize = BitConverter.ToUInt16(image, pe + 20);
        int sectionOffset = pe + 24 + optionalSize;
        PeSection[] sections = new PeSection[sectionCount];

        for (int i = 0; i < sectionCount; ++i, sectionOffset += 40)
        {
            sections[i] = new PeSection {
                HeaderOffset = sectionOffset,
                VirtualSize = BitConverter.ToInt32(image, sectionOffset + 8),
                VirtualAddress = BitConverter.ToInt32(image, sectionOffset + 12),
                RawSize = BitConverter.ToInt32(image, sectionOffset + 16),
                RawOffset = BitConverter.ToInt32(image, sectionOffset + 20)
            };
        }
        return sections;
    }

    private static int RvaToFileOffset(byte[] image, int rva)
    {
        foreach (PeSection section in ReadSections(image))
        {
            int span = Math.Max(section.VirtualSize, section.RawSize);
            if (rva >= section.VirtualAddress &&
                rva < section.VirtualAddress + span)
                return section.RawOffset + (rva - section.VirtualAddress);
        }

        throw new InvalidOperationException("method RVA is outside PE sections");
    }

    private static int FileOffsetToRva(byte[] image, int fileOffset)
    {
        foreach (PeSection section in ReadSections(image))
        {
            if (fileOffset >= section.RawOffset &&
                fileOffset < section.RawOffset + section.RawSize)
                return section.VirtualAddress + (fileOffset - section.RawOffset);
        }

        throw new InvalidOperationException("file offset is outside PE sections");
    }

    private static int MethodCodeOffset(byte[] image, int methodOffset, out int codeSize)
    {
        byte first = image[methodOffset];
        if ((first & 3) == 2)
        {
            codeSize = first >> 2;
            return methodOffset + 1;
        }

        if ((first & 3) != 3)
            throw new InvalidOperationException("unsupported managed method header");

        ushort flagsAndSize = BitConverter.ToUInt16(image, methodOffset);
        int headerSize = ((flagsAndSize >> 12) & 0xf) * 4;
        codeSize = BitConverter.ToInt32(image, methodOffset + 4);
        return methodOffset + headerSize;
    }

    private static int GetCodeOffset(byte[] image, MethodDefinition method,
                                     out int codeSize)
    {
        int methodOffset = RvaToFileOffset(image, method.RVA);
        int codeOffset = MethodCodeOffset(image, methodOffset, out codeSize);
        if (!method.HasBody || codeSize != method.Body.CodeSize)
            throw new InvalidOperationException(method.FullName + " code size mismatch");
        return codeOffset;
    }

    private static void ReplaceBodyWithTrue(byte[] image, MethodDefinition method)
    {
        if (method.Body.HasExceptionHandlers)
            throw new InvalidOperationException(method.FullName + " has exception handlers");
        int codeSize;
        int codeOffset = GetCodeOffset(image, method, out codeSize);
        if (codeSize < 2)
            throw new InvalidOperationException(method.FullName + " body is too short");
        Array.Clear(image, codeOffset, codeSize);
        image[codeOffset] = 0x17;     // ldc.i4.1
        image[codeOffset + 1] = 0x2a; // ret
    }

    private static void PatchFloatDefaultBeforeField(byte[] image,
                                                      MethodDefinition method,
                                                      FieldDefinition field,
                                                      float oldValue,
                                                      float newValue)
    {
        if (!method.HasBody || method.Body.HasExceptionHandlers)
            throw new InvalidOperationException(
                method.FullName + " has an unexpected body");
        Instruction store = method.Body.Instructions.Single(instruction =>
            instruction.OpCode.Code == Mono.Cecil.Cil.Code.Stfld &&
            instruction.Operand is FieldReference &&
            ((FieldReference)instruction.Operand).MetadataToken ==
                field.MetadataToken &&
            instruction.Previous != null &&
            instruction.Previous.OpCode.Code == Mono.Cecil.Cil.Code.Ldc_R4 &&
            Math.Abs((float)instruction.Previous.Operand - oldValue) < 0.0001f);
        Instruction load = store.Previous;
        int codeSize;
        int codeOffset = GetCodeOffset(image, method, out codeSize);
        int diskOffset = codeOffset + load.Offset;
        if (diskOffset + 5 > codeOffset + codeSize || image[diskOffset] != 0x22 ||
            Math.Abs(BitConverter.ToSingle(image, diskOffset + 1) - oldValue) >=
                0.0001f)
            throw new InvalidOperationException(
                method.FullName + " zoom default precondition failed");
        Buffer.BlockCopy(BitConverter.GetBytes(newValue), 0, image,
                         diskOffset + 1, 4);
    }

    private static int ReadIndex(byte[] image, ref int offset, int size)
    {
        int value;
        if (size == 2)
            value = BitConverter.ToUInt16(image, offset);
        else if (size == 4)
            value = BitConverter.ToInt32(image, offset);
        else
            throw new InvalidOperationException("unsupported metadata index size");
        offset += size;
        return value;
    }

    private static int TableIndexSize(uint[] rowCounts, int table)
    {
        return rowCounts[table] < 65536 ? 2 : 4;
    }

    private static int CodedIndexSize(uint[] rowCounts, int tagBits,
                                      params int[] tables)
    {
        uint largest = 0;
        foreach (int table in tables)
            largest = Math.Max(largest, rowCounts[table]);
        return largest < (1u << (16 - tagBits)) ? 2 : 4;
    }

    private static int GetMethodDefRowOffset(byte[] image,
                                              MethodDefinition method)
    {
        int pe = BitConverter.ToInt32(image, 0x3c);
        int optional = pe + 24;
        int magic = BitConverter.ToUInt16(image, optional);
        int directories = optional + (magic == 0x20b ? 112 : 96);
        int cliRva = BitConverter.ToInt32(image, directories + 14 * 8);
        int cli = RvaToFileOffset(image, cliRva);
        int metadataRva = BitConverter.ToInt32(image, cli + 8);
        int metadata = RvaToFileOffset(image, metadataRva);
        if (Encoding.ASCII.GetString(image, metadata, 4) != "BSJB")
            throw new InvalidOperationException("invalid CLI metadata root");

        int versionLength = BitConverter.ToInt32(image, metadata + 12);
        int stream = Align4(metadata + 16 + versionLength);
        int streamCount = BitConverter.ToUInt16(image, stream + 2);
        stream += 4;
        int tables = -1;
        for (int i = 0; i < streamCount; ++i)
        {
            int relative = BitConverter.ToInt32(image, stream);
            int name = stream + 8;
            int end = name;
            while (image[end] != 0) ++end;
            string streamName = Encoding.ASCII.GetString(image, name, end - name);
            if (streamName == "#~" || streamName == "#-")
                tables = metadata + relative;
            stream = Align4(end + 1);
        }
        if (tables < 0)
            throw new InvalidOperationException("CLI metadata tables not found");

        byte heapSizes = image[tables + 6];
        ulong valid = BitConverter.ToUInt64(image, tables + 8);
        uint[] rows = new uint[64];
        int cursor = tables + 24;
        for (int table = 0; table < 64; ++table)
            if ((valid & (1UL << table)) != 0)
            {
                rows[table] = BitConverter.ToUInt32(image, cursor);
                cursor += 4;
            }

        int strings = (heapSizes & 0x01) != 0 ? 4 : 2;
        int guids = (heapSizes & 0x02) != 0 ? 4 : 2;
        int blobs = (heapSizes & 0x04) != 0 ? 4 : 2;
        int resolutionScope = CodedIndexSize(rows, 2, 0, 26, 35, 1);
        int typeDefOrRef = CodedIndexSize(rows, 2, 2, 1, 27);

        int[] sizes = new int[7];
        sizes[0] = 2 + strings + guids * 3;
        sizes[1] = resolutionScope + strings * 2;
        sizes[2] = 4 + strings * 2 + typeDefOrRef +
                   TableIndexSize(rows, 4) + TableIndexSize(rows, 6);
        sizes[3] = TableIndexSize(rows, 4);
        sizes[4] = 2 + strings + blobs;
        sizes[5] = TableIndexSize(rows, 6);
        sizes[6] = 4 + 2 + 2 + strings + blobs + TableIndexSize(rows, 8);

        for (int table = 0; table < 6; ++table)
            cursor += checked((int)rows[table] * sizes[table]);
        int rid = checked((int)method.MetadataToken.RID);
        if (rid <= 0 || (uint)rid > rows[6])
            throw new InvalidOperationException("invalid MethodDef row id");
        return cursor + (rid - 1) * sizes[6];
    }

    private static CodeCave StripDebugDirectoryAndGetCodeCave(byte[] image,
                                                               int requiredSize)
    {
        const int knownDataOffset = 0x8db9a4;
        const int knownDataRva = 0x8dd7a4;
        const int knownDataSize = 0x8b;
        int pe = BitConverter.ToInt32(image, 0x3c);
        int optional = pe + 24;
        int magic = BitConverter.ToUInt16(image, optional);
        int directories = optional + (magic == 0x20b ? 112 : 96);
        int debugRva = BitConverter.ToInt32(image, directories + 6 * 8);
        int debugSize = BitConverter.ToInt32(image, directories + 6 * 8 + 4);
        CodeCave cave = null;

        if (debugRva != 0 && debugSize >= 28)
        {
            int debug = RvaToFileOffset(image, debugRva);
            for (int entry = debug;
                 entry + 28 <= debug + debugSize; entry += 28)
            {
                int type = BitConverter.ToInt32(image, entry + 12);
                int size = BitConverter.ToInt32(image, entry + 16);
                int dataRva = BitConverter.ToInt32(image, entry + 20);
                int data = BitConverter.ToInt32(image, entry + 24);
                if (type != 2 || size < requiredSize || data < 0 ||
                    data + size > image.Length)
                    continue;
                if (Encoding.ASCII.GetString(image, data, 4) != "RSDS")
                    continue;
                if (dataRva == 0)
                    dataRva = FileOffsetToRva(image, data);
                if (RvaToFileOffset(image, dataRva) != data)
                    throw new InvalidOperationException(
                        "CodeView RVA/file offset mismatch");
                cave = new CodeCave {
                    FileOffset = data,
                    Rva = dataRva,
                    Size = size
                };
                Array.Clear(image, data, size);
                break;
            }
            if (cave == null)
                throw new InvalidOperationException(
                    "suitable CodeView code cave was not found");
            Array.Clear(image, debug, debugSize);
            Array.Clear(image, directories + 6 * 8, 8);
        }
        else
        {
            // A previously prepared 1.6.15.3 assembly has the same exact
            // CodeView bytes already stripped. Accept that state only at the
            // content-addressed, section-validated location.
            if (knownDataOffset + knownDataSize > image.Length ||
                FileOffsetToRva(image, knownDataOffset) != knownDataRva ||
                image.Skip(knownDataOffset).Take(knownDataSize)
                     .Any(value => value != 0))
                throw new InvalidOperationException(
                    "debug directory is absent and the known code cave is not clean");
            cave = new CodeCave {
                FileOffset = knownDataOffset,
                Rva = knownDataRva,
                Size = knownDataSize
            };
        }
        return cave;
    }

    private static void WriteToken(List<byte> code, MetadataToken token)
    {
        code.AddRange(BitConverter.GetBytes(token.ToInt32()));
    }

    private static void SetMethodRva(byte[] image, MethodDefinition method,
                                     int newRva)
    {
        int methodRow = GetMethodDefRowOffset(image, method);
        int oldRva = BitConverter.ToInt32(image, methodRow);
        if (oldRva != method.RVA)
            throw new InvalidOperationException(
                method.FullName + " MethodDef RVA verification failed");
        Buffer.BlockCopy(BitConverter.GetBytes(newRva), 0, image, methodRow, 4);
    }

    private static byte[] BuildOptionsGamePadBody(TypeDefinition optionsPage,
                                                   MethodDefinition baseScroll,
                                                   MethodDefinition optionsScroll)
    {
        // The stack keeps the successful `isinst OptionsPage` result alive
        // across both button comparisons, avoiding a second cast and keeping
        // this wrapper small enough for a tiny IL method header.
        List<byte> code = new List<byte>();
        code.Add(0x02);             // ldarg.0
        code.Add(0x75); WriteToken(code, optionsPage.MetadataToken); // isinst
        code.Add(0x25);             // dup
        code.Add(0x2c); int notOptionsBranch = code.Count; code.Add(0);
        code.Add(0x03);             // ldarg.1
        code.Add(0x17);             // ldc.i4.1 Buttons.DPadUp
        code.Add(0x2e); int upBranch = code.Count; code.Add(0);
        code.Add(0x03);             // ldarg.1
        code.Add(0x18);             // ldc.i4.2 Buttons.DPadDown
        code.Add(0x2e); int downBranch = code.Count; code.Add(0);
        code.Add(0x26);             // pop OptionsPage
        code.Add(0x2b); int fallbackBranch = code.Count; code.Add(0);
        int notOptions = code.Count;
        code.Add(0x26);             // pop null isinst result
        int fallback = code.Count;
        code.Add(0x02);             // ldarg.0
        code.Add(0x03);             // ldarg.1
        code.Add(0x28); WriteToken(code, baseScroll.MetadataToken); // call alias
        code.Add(0x2a);             // ret
        int up = code.Count;
        code.Add(0x1f); code.Add(100); // up: +100
        code.Add(0x6f); WriteToken(code, optionsScroll.MetadataToken); // callvirt
        code.Add(0x2a);             // ret
        int down = code.Count;
        code.Add(0x1f); code.Add(unchecked((byte)-100)); // down: -100
        code.Add(0x6f); WriteToken(code, optionsScroll.MetadataToken); // callvirt
        code.Add(0x2a);             // ret

        code[notOptionsBranch] = checked(
            (byte)(sbyte)(notOptions - (notOptionsBranch + 1)));
        code[upBranch] = checked((byte)(sbyte)(up - (upBranch + 1)));
        code[downBranch] = checked((byte)(sbyte)(down - (downBranch + 1)));
        code[fallbackBranch] = checked(
            (byte)(sbyte)(fallback - (fallbackBranch + 1)));
        return code.ToArray();
    }

    private static int PatchOptionsControllerScroll(byte[] image,
                                                     ModuleDefinition module,
                                                     CodeCave cave)
    {
        TypeDefinition optionsPage = module.GetType("StardewValley.Menus.OptionsPage");
        TypeDefinition clickableMenu = module.GetType(
            "StardewValley.Menus.IClickableMenu");
        MethodDefinition receiveKey = optionsPage.Methods.Single(method =>
            method.Name == "receiveKeyPress" && method.Parameters.Count == 1);
        MethodDefinition baseGamePad = clickableMenu.Methods.Single(method =>
            method.Name == "receiveGamePadButton" &&
            method.Parameters.Count == 1);
        MethodDefinition baseScroll = clickableMenu.Methods.Single(method =>
            method.Name == "receiveScrollWheelAction" &&
            method.Parameters.Count == 1);
        MethodDefinition optionsScroll = optionsPage.Methods.Single(method =>
            method.Name == "receiveScrollWheelAction" &&
            method.Parameters.Count == 1);
        if (!receiveKey.HasBody || receiveKey.Body.CodeSize != 1 ||
            receiveKey.Body.Instructions.Count != 1 ||
            receiveKey.Body.Instructions[0].OpCode.Code !=
                Mono.Cecil.Cil.Code.Ret)
            throw new InvalidOperationException("unexpected OptionsPage.receiveKeyPress body");
        if (!baseGamePad.HasBody || baseGamePad.Body.CodeSize != 31 ||
            !baseScroll.HasBody || baseScroll.Body.CodeSize != 1 ||
            !optionsScroll.HasBody || optionsScroll.Body.CodeSize != 54)
            throw new InvalidOperationException(
                "unexpected controller/scroll method bodies");

        byte[] code = BuildOptionsGamePadBody(
            optionsPage, baseScroll, optionsScroll);
        if (code.Length >= 64 || code.Length + 1 > cave.Size)
            throw new InvalidOperationException("controller wrapper does not fit code cave");
        image[cave.FileOffset] = checked((byte)((code.Length << 2) | 2));
        Buffer.BlockCopy(code, 0, image, cave.FileOffset + 1, code.Length);

        // Keep the original base implementation addressable through the
        // otherwise no-op base wheel method, then redirect gamepad dispatch to
        // the wrapper. Non-OptionsPage menus therefore retain byte-for-byte
        // base B/close behavior.
        SetMethodRva(image, baseScroll, baseGamePad.RVA);
        SetMethodRva(image, baseGamePad, cave.Rva);
        return cave.Rva;
    }

    public static int Main(string[] args)
    {
        if (args.Length != 2)
        {
            Console.Error.WriteLine("usage: PatchStardewOsk <input.dll> <output.dll>");
            return 2;
        }

        AssemblyDefinition assembly = AssemblyDefinition.ReadAssembly(args[0]);
        ModuleDefinition module = assembly.MainModule;
        TypeDefinition textBox = module.GetType("StardewValley.Menus.TextBox");
        TypeDefinition game1 = module.GetType("StardewValley.Game1");
        TypeDefinition newDaySync = module.GetType("StardewValley.NewDaySynchronizer");
        TypeDefinition options = module.GetType("StardewValley.Options");
        MethodDefinition androidKeyboard = textBox.Methods.Single(method =>
            method.Name == "ShowAndroidKeyboard" && method.Parameters.Count == 0);
        MethodDefinition showTextEntry = game1.Methods.Single(method =>
            method.Name == "showTextEntry" && method.Parameters.Count == 1 &&
            method.Parameters[0].ParameterType.FullName == textBox.FullName);
        MethodDefinition readyForSave = newDaySync.Methods.Single(method =>
            method.Name == "readyForSave" && method.Parameters.Count == 0);
        MethodDefinition getText = textBox.Methods.Single(method =>
            method.Name == "get_Text" && method.Parameters.Count == 0);
        MethodDefinition getTitleText = textBox.Methods.Single(method =>
            method.Name == "get_TitleText" && method.Parameters.Count == 0);
        MethodDefinition setText = textBox.Methods.Single(method =>
            method.Name == "set_Text" && method.Parameters.Count == 1);
        MethodDefinition optionsConstructor = options.Methods.Single(method =>
            method.Name == ".ctor" && method.Parameters.Count == 0);
        MethodDefinition setOptionsToDefaults = options.Methods.Single(method =>
            method.Name == "setToDefaults" && method.Parameters.Count == 0);
        FieldDefinition baseZoomLevel = options.Fields.Single(field =>
            field.Name == "baseZoomLevel");
        FieldDefinition singlePlayerBaseZoomLevel = options.Fields.Single(field =>
            field.Name == "singlePlayerBaseZoomLevel");
        MethodReference stringEquals = module.GetMemberReferences()
            .OfType<MethodReference>().First(method =>
                method.DeclaringType.FullName == "System.String" &&
                method.Name == "op_Equality" && method.Parameters.Count == 2);

        if (!androidKeyboard.HasBody || androidKeyboard.Body.HasExceptionHandlers)
            throw new InvalidOperationException("unexpected ShowAndroidKeyboard body");

        byte[] image = File.ReadAllBytes(args[0]);
        CodeCave codeCave = StripDebugDirectoryAndGetCodeCave(image, 64);
        int diskCodeSize;
        int codeOffset = GetCodeOffset(image, androidKeyboard, out diskCodeSize);
        if (diskCodeSize < 7)
            throw new InvalidOperationException("ShowAndroidKeyboard code size mismatch");
        if (image[codeOffset] != 0x72)
            throw new InvalidOperationException("expected initial ldstr in keyboard body");
        int emptyStringToken = BitConverter.ToInt32(image, codeOffset + 1);

        // Clear only the localized placeholder (Text == TitleText), then open
        // the game's own controller-friendly TextEntryMenu. Real names survive
        // repeated clicks, matching the Android text-field behavior.
        Array.Clear(image, codeOffset, diskCodeSize);
        List<byte> keyboard = new List<byte>();
        keyboard.Add(0x02); // ldarg.0
        keyboard.Add(0x28); WriteToken(keyboard, getText.MetadataToken);
        keyboard.Add(0x02); // ldarg.0
        keyboard.Add(0x28); WriteToken(keyboard, getTitleText.MetadataToken);
        keyboard.Add(0x28); WriteToken(keyboard, stringEquals.MetadataToken);
        keyboard.Add(0x2c); int keepTextBranch = keyboard.Count; keyboard.Add(0);
        keyboard.Add(0x02); // ldarg.0
        keyboard.Add(0x72);
        keyboard.AddRange(BitConverter.GetBytes(emptyStringToken));
        keyboard.Add(0x28); WriteToken(keyboard, setText.MetadataToken);
        int keepText = keyboard.Count;
        keyboard.Add(0x02); // ldarg.0
        keyboard.Add(0x28); WriteToken(keyboard, showTextEntry.MetadataToken);
        keyboard.Add(0x2a); // ret
        keyboard[keepTextBranch] = checked(
            (byte)(sbyte)(keepText - (keepTextBranch + 1)));
        if (keyboard.Count > diskCodeSize)
            throw new InvalidOperationException("patched keyboard body does not fit");
        Buffer.BlockCopy(keyboard.ToArray(), 0, image, codeOffset,
                         keyboard.Count);

        // This Android build can wait forever in the network ready barrier on
        // an offline first save. NextOS has no online multiplayer backend, so
        // the offline port can acknowledge that barrier immediately.
        ReplaceBodyWithTrue(image, readyForSave);
        // Options are serialized inside each save. Existing saves overwrite
        // singlePlayerBaseZoomLevel during XML load, but a brand-new farm kept
        // the Android default 1.0 and therefore looked much closer than the
        // already-tuned test save. Use the game's supported minimum (0.75) as
        // the constructor/default only; never overwrite a stored preference.
        PatchFloatDefaultBeforeField(image, optionsConstructor,
                                     singlePlayerBaseZoomLevel, 1.0f, 0.75f);
        PatchFloatDefaultBeforeField(image, setOptionsToDefaults,
                                     baseZoomLevel, 1.0f, 0.75f);
        int optionsRva = PatchOptionsControllerScroll(image, module, codeCave);

        File.WriteAllBytes(args[1], image);
        Console.WriteLine(
            "patched OSK RVA 0x{0:x}, offline save RVA 0x{1:x}, options gamepad RVA 0x{2:x}, new-game zoom 0.75",
            androidKeyboard.RVA, readyForSave.RVA, optionsRva);
        return 0;
    }
}
