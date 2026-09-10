using System; using System.Linq; using System.Collections.Generic;
using Mono.Cecil; using Mono.Cecil.Cil;
class PatchGam {
  static void Main(string[] a){
    // patchgam <SOR4.dll> <SOR4Bridge.dll>
    var asm=AssemblyDefinition.ReadAssembly(a[0], new ReaderParameters{ReadWrite=true});
    var bridge=AssemblyDefinition.ReadAssembly(a[1]);
    var getAssets=bridge.MainModule.Types.First(t=>t.FullName=="SOR4Bridge.AssetBridge").Methods.First(m=>m.Name=="GetAssets");
    var impGetAssets=asm.MainModule.ImportReference(getAssets);
    int patched=0;
    foreach(var t in AllTypes(asm.MainModule)) foreach(var m in t.Methods){
      if(m.Name!="get_AssetManager" || !m.HasBody) continue;
      var ins=m.Body.Instructions; var il=m.Body.GetILProcessor();
      for(int i=0;i<ins.Count;i++){
        // acha callvirt ...Context::get_Assets()
        if(ins[i].OpCode==OpCodes.Callvirt && ins[i].Operand is MethodReference mr && mr.Name=="get_Assets"){
          // acha o 'call Game::get_Activity()' para tras; nopa get_Activity + tudo ate get_Assets
          int gi=-1;
          for(int j=i-1;j>=0 && j>=i-5;j--){
            if((ins[j].OpCode==OpCodes.Call||ins[j].OpCode==OpCodes.Callvirt) && ins[j].Operand is MethodReference gm && gm.Name=="get_Activity"){ gi=j; break; }
          }
          if(gi>=0){ for(int k=gi;k<i;k++) il.Replace(ins[k], il.Create(OpCodes.Nop)); }
          il.Replace(ins[i], il.Create(OpCodes.Call, impGetAssets));
          patched++;
          Console.WriteLine("patchado get_AssetManager em "+t.FullName);
        }
      }
    }
    asm.Write(); Console.WriteLine("get_Assets->GetAssets patched: "+patched);
  }
  static IEnumerable<TypeDefinition> AllTypes(ModuleDefinition m){ foreach(var t in m.Types){ yield return t; foreach(var x in Nested(t)) yield return x; } }
  static IEnumerable<TypeDefinition> Nested(TypeDefinition t){ foreach(var n in t.NestedTypes){ yield return n; foreach(var x in Nested(n)) yield return x; } }
}
