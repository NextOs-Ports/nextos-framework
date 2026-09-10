using System; using System.Linq; using System.Collections.Generic;
using Mono.Cecil; using Mono.Cecil.Cil;
class NoopM {
  static void Main(string[] a){
    // noopm <dll> [Type.]Method ...
    var asm=AssemblyDefinition.ReadAssembly(a[0], new ReaderParameters{ReadWrite=true});
    var names=new HashSet<string>(a.Skip(1)); int n=0;
    foreach(var t in AllTypes(asm.MainModule)) foreach(var m in t.Methods){
      if(!m.HasBody) continue;
      if(!names.Contains(m.Name) && !names.Contains(t.Name+"."+m.Name) && !names.Contains(t.Name+".*") && !names.Contains(t.FullName+".*")) continue;
      if(m.IsConstructor || m.Name==".cctor") continue; // mantem ctors
      var b=m.Body; b.Instructions.Clear(); b.Variables.Clear(); b.ExceptionHandlers.Clear(); b.InitLocals=false;
      var il=b.GetILProcessor(); var rt=m.ReturnType;
      if(rt.MetadataType==MetadataType.Void) il.Append(il.Create(OpCodes.Ret));
      else if(rt.IsValueType||rt.IsGenericParameter){ var v=new VariableDefinition(rt); b.Variables.Add(v); b.InitLocals=true;
        il.Append(il.Create(OpCodes.Ldloca_S,v)); il.Append(il.Create(OpCodes.Initobj,rt)); il.Append(il.Create(OpCodes.Ldloc_0)); il.Append(il.Create(OpCodes.Ret)); }
      else { il.Append(il.Create(OpCodes.Ldnull)); il.Append(il.Create(OpCodes.Ret)); }
      n++; Console.WriteLine("noop: "+t.FullName+"."+m.Name);
    }
    asm.Write(); Console.WriteLine("total noop: "+n);
  }
  static IEnumerable<TypeDefinition> AllTypes(ModuleDefinition m){ foreach(var t in m.Types){ yield return t; foreach(var x in Nested(t)) yield return x; } }
  static IEnumerable<TypeDefinition> Nested(TypeDefinition t){ foreach(var n in t.NestedTypes){ yield return n; foreach(var x in Nested(n)) yield return x; } }
}
