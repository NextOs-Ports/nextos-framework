using System; using System.Linq; using System.Collections.Generic;
using Mono.Cecil; using Mono.Cecil.Cil;
// rettrue <SOR4.dll> <Type::method> [<Type::method> ...]
// Substitui o corpo de cada metodo bool por: return true (ldc.i4.1; ret).
// Motivo SoR4: load_save_and_config_is_finished() = AndroidServices.DidFinishLoadingCloudFiles()
// que esta STUBADO -> retorna false p/ sempre -> TitleScreen fica preso em loading=true.
// Forcar true faz o titulo avancar pro menu logo apos o press.
class RetTrue {
  static IEnumerable<TypeDefinition> All(ModuleDefinition m){foreach(var t in m.Types){yield return t;foreach(var n in Nest(t))yield return n;}}
  static IEnumerable<TypeDefinition> Nest(TypeDefinition t){foreach(var n in t.NestedTypes){yield return n;foreach(var x in Nest(n))yield return x;}}
  static void Main(string[] a){
    var asm=AssemblyDefinition.ReadAssembly(a[0], new ReaderParameters{ReadWrite=true});
    var m=asm.MainModule; int n=0;
    var targets=new HashSet<string>(a.Skip(1));
    foreach(var t in All(m)) foreach(var meth in t.Methods){
      string key=t.FullName+"::"+meth.Name;
      if(!targets.Contains(key) || !meth.HasBody) continue;
      var b=meth.Body; b.Instructions.Clear(); b.Variables.Clear(); b.ExceptionHandlers.Clear();
      var il=b.GetILProcessor();
      il.Append(il.Create(OpCodes.Ldc_I4_1));
      il.Append(il.Create(OpCodes.Ret));
      n++; Console.WriteLine("rettrue "+key);
    }
    asm.Write(); Console.WriteLine("rettrue patched: "+n);
    if(n==0) Environment.Exit(2);
  }
}
