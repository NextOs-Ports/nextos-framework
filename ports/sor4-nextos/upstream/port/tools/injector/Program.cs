using System; using System.Linq; using Mono.Cecil; using Mono.Cecil.Cil;
class Injector {
  static void Main(string[] a){
    // injector <Mono.Android.dll> <SOR4Bridge.dll>
    string mono=a[0], bridgePath=a[1];
    var rp=new ReaderParameters{ ReadWrite=true };
    var asm=AssemblyDefinition.ReadAssembly(mono, rp);
    var bridge=AssemblyDefinition.ReadAssembly(bridgePath);
    var ab=bridge.MainModule.Types.First(t=>t.FullName=="SOR4Bridge.AssetBridge");
    var mOpen=ab.Methods.First(m=>m.Name=="Open");
    var mGet=ab.Methods.First(m=>m.Name=="GetAssets");
    var mList=ab.Methods.First(m=>m.Name=="List");
    var impOpen=asm.MainModule.ImportReference(mOpen);
    var impGet=asm.MainModule.ImportReference(mGet);
    var impList=asm.MainModule.ImportReference(mList);
    int nOpen=0,nGet=0,nList=0;
    foreach(var t in All(asm.MainModule)){
      foreach(var m in t.Methods){
        if(!m.HasBody) continue;
        // AssetManager.Open(string) -> AssetBridge.Open
        if(m.Name=="Open" && t.Name=="AssetManager" && m.Parameters.Count==1 && m.Parameters[0].ParameterType.MetadataType==MetadataType.String && m.ReturnType.Name=="Stream"){
          var il=Rewrite(m); il.Append(il.Create(OpCodes.Ldarg_1)); il.Append(il.Create(OpCodes.Call,impOpen)); il.Append(il.Create(OpCodes.Ret)); nOpen++;
        }
        // AssetManager.List(string) -> AssetBridge.List
        else if(m.Name=="List" && t.Name=="AssetManager" && m.Parameters.Count==1 && m.Parameters[0].ParameterType.MetadataType==MetadataType.String && m.ReturnType.FullName=="System.String[]"){
          var il=Rewrite(m); il.Append(il.Create(OpCodes.Ldarg_1)); il.Append(il.Create(OpCodes.Call,impList)); il.Append(il.Create(OpCodes.Ret)); nList++;
        }
        // *.get_Assets() -> AssetBridge.GetAssets
        else if(m.Name=="get_Assets" && m.Parameters.Count==0 && m.ReturnType.Name=="AssetManager"){
          var il=Rewrite(m); il.Append(il.Create(OpCodes.Call,impGet)); il.Append(il.Create(OpCodes.Ret)); nGet++;
        }
      }
    }
    asm.Write();
    Console.WriteLine($"injetado: Open={nOpen} List={nList} get_Assets={nGet}");
  }
  static ILProcessor Rewrite(MethodDefinition m){ var b=m.Body; b.Instructions.Clear(); b.Variables.Clear(); b.ExceptionHandlers.Clear(); b.InitLocals=false; return b.GetILProcessor(); }
  static System.Collections.Generic.IEnumerable<TypeDefinition> All(ModuleDefinition mod){
    foreach(var t in mod.Types){ yield return t; foreach(var n in Nested(t)) yield return n; } }
  static System.Collections.Generic.IEnumerable<TypeDefinition> Nested(TypeDefinition t){
    foreach(var n in t.NestedTypes){ yield return n; foreach(var x in Nested(n)) yield return x; } }
}
