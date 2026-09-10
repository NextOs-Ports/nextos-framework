using System; using System.Linq; using System.Collections.Generic;
using Mono.Cecil; using Mono.Cecil.Cil;
// verstub <SOR4.dll>: CommonLib.utils::get_version_identifier chama APIs Android
// (Application.Context.PackageManager.GetPackageInfo) que sao stub -> NRE no menu.
// Substitui o corpo por: return "1.4.5".
class VerStub {
  static IEnumerable<TypeDefinition> All(ModuleDefinition m){foreach(var t in m.Types){yield return t;foreach(var n in Nest(t))yield return n;}}
  static IEnumerable<TypeDefinition> Nest(TypeDefinition t){foreach(var n in t.NestedTypes){yield return n;foreach(var x in Nest(n))yield return x;}}
  static void Main(string[] a){
    var asm=AssemblyDefinition.ReadAssembly(a[0], new ReaderParameters{ReadWrite=true});
    var m=asm.MainModule;
    int n=0;
    foreach(var t in All(m)) foreach(var meth in t.Methods){
      if(meth.Name!="get_version_identifier" || !meth.HasBody) continue;
      var b=meth.Body; b.Instructions.Clear(); b.Variables.Clear(); b.ExceptionHandlers.Clear();
      var il=b.GetILProcessor();
      il.Append(il.Create(OpCodes.Ldstr, "1.4.5"));
      il.Append(il.Create(OpCodes.Ret));
      n++; Console.WriteLine("stubbed "+t.FullName+"::"+meth.Name);
    }
    asm.Write(); Console.WriteLine("verstub patched: "+n);
    if(n==0) Environment.Exit(2);
  }
}
