using System; using System.Linq; using System.Collections.Generic;
using Mono.Cecil; using Mono.Cecil.Cil;
class InjLog {
  static void Main(string[] a){
    // injlog <SOR4.dll> <SOR4Bridge.dll> name1 name2 ...
    string asmPath=a[0], bridgePath=a[1]; var names=new HashSet<string>(a.Skip(2));
    var asm=AssemblyDefinition.ReadAssembly(asmPath, new ReaderParameters{ReadWrite=true});
    var bridge=AssemblyDefinition.ReadAssembly(bridgePath);
    var log=bridge.MainModule.Types.First(t=>t.FullName=="SOR4Bridge.AssetBridge").Methods.First(m=>m.Name=="Log");
    var impLog=asm.MainModule.ImportReference(log);
    int n=0;
    foreach(var t in AllTypes(asm.MainModule)) foreach(var m in t.Methods){
      bool argMode = names.Contains("@"+m.Name);
      if(!m.HasBody || (!names.Contains(m.Name) && !argMode)) continue;
      var il=m.Body.GetILProcessor(); var first=m.Body.Instructions[0];
      if(argMode && m.Parameters.Count>0 && m.Parameters[0].ParameterType.MetadataType==MetadataType.String){
        il.InsertBefore(first, il.Create(OpCodes.Ldarg, m.IsStatic?0:1)); // primeiro parametro string
        il.InsertBefore(first, il.Create(OpCodes.Call, impLog));
      } else {
        il.InsertBefore(first, il.Create(OpCodes.Ldstr, t.Name+"."+m.Name+" ENTER"));
        il.InsertBefore(first, il.Create(OpCodes.Call, impLog));
      }
      n++;
    }
    asm.Write(); Console.WriteLine("injetado log em "+n+" metodos");
  }
  static IEnumerable<TypeDefinition> AllTypes(ModuleDefinition m){ foreach(var t in m.Types){ yield return t; foreach(var x in Nested(t)) yield return x; } }
  static IEnumerable<TypeDefinition> Nested(TypeDefinition t){ foreach(var n in t.NestedTypes){ yield return n; foreach(var x in Nested(n)) yield return x; } }
}
