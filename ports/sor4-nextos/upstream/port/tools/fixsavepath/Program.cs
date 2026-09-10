using System; using System.Linq; using System.Collections.Generic;
using Mono.Cecil; using Mono.Cecil.Cil;
// Reescreve CommonLib.platform.get_save_file_path(string file) p/ usar a env HOME (que FUNCIONA)
// em vez de Environment.GetFolderPath(Personal) (retorna VAZIO nessa runtime self-contained) ->
// o save tinha path sem diretorio -> CreateDirectory("") lancava. Agora: Path.Combine(HOME, file).
class FixSavePath {
  static void Main(string[] a){
    var asm=AssemblyDefinition.ReadAssembly(a[0], new ReaderParameters{ReadWrite=true});
    var mod=asm.MainModule;
    var getEnv=mod.ImportReference(typeof(Environment).GetMethod("GetEnvironmentVariable", new[]{typeof(string)}));
    var combine=mod.ImportReference(typeof(System.IO.Path).GetMethod("Combine", new[]{typeof(string),typeof(string)}));
    int n=0;
    foreach(var t in AllTypes(mod)) foreach(var m in t.Methods){
      if(m.Name!="get_save_file_path" || !m.HasBody) continue;
      var il=m.Body.GetILProcessor();
      m.Body.Instructions.Clear(); m.Body.Variables.Clear(); m.Body.ExceptionHandlers.Clear();
      var cont=il.Create(OpCodes.Nop);
      il.Append(il.Create(OpCodes.Ldstr,"HOME"));
      il.Append(il.Create(OpCodes.Call,getEnv));      // home
      il.Append(il.Create(OpCodes.Dup));
      il.Append(il.Create(OpCodes.Brtrue,cont));      // home!=null -> cont
      il.Append(il.Create(OpCodes.Pop));
      il.Append(il.Create(OpCodes.Ldstr,"/storage/roms/ports/sor4/save"));
      il.Append(cont);
      il.Append(il.Create(OpCodes.Ldarg_0));          // filename
      il.Append(il.Create(OpCodes.Call,combine));
      il.Append(il.Create(OpCodes.Ret));
      n++; Console.WriteLine("get_save_file_path reescrito (HOME) em "+t.FullName+" args="+m.Parameters.Count);
    }
    asm.Write(); Console.WriteLine("total: "+n);
  }
  static IEnumerable<TypeDefinition> AllTypes(ModuleDefinition m){ foreach(var t in m.Types){ yield return t; foreach(var x in Nested(t)) yield return x; } }
  static IEnumerable<TypeDefinition> Nested(TypeDefinition t){ foreach(var x in t.NestedTypes){ yield return x; foreach(var y in Nested(x)) yield return y; } }
}
