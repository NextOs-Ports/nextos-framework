using System; using System.Linq; using System.Collections.Generic;
using Mono.Cecil; using Mono.Cecil.Cil;
// injeta File.WriteAllText("/tmp/saveerr.txt", msg) logo apos o String.Format no catch do
// wrapper program.save_save_game, p/ capturar a excecao do save.
class DumpExc {
  static void Main(string[] a){
    var asm=AssemblyDefinition.ReadAssembly(a[0], new ReaderParameters{ReadWrite=true});
    var mod=asm.MainModule;
    var wat=mod.ImportReference(typeof(System.IO.File).GetMethod("WriteAllText", new[]{typeof(string),typeof(string)}));
    int n=0;
    foreach(var t in AllTypes(mod)) foreach(var m in t.Methods){
      if(m.Name!="save_save_game" || !t.FullName.Contains("MetaGame") || !m.HasBody) continue;
      var il=m.Body.GetILProcessor();
      foreach(var ins in m.Body.Instructions.ToList()){
        if(ins.OpCode==OpCodes.Call && ins.Operand is MethodReference mr && mr.Name=="Format" && mr.DeclaringType.Name=="String"){
          var st=ins.Next; // stloc do msg
          Instruction ldvar;
          if(st.OpCode==OpCodes.Stloc_3) ldvar=il.Create(OpCodes.Ldloc_3);
          else if(st.OpCode==OpCodes.Stloc_2) ldvar=il.Create(OpCodes.Ldloc_2);
          else if(st.OpCode==OpCodes.Stloc_1) ldvar=il.Create(OpCodes.Ldloc_1);
          else if(st.OpCode==OpCodes.Stloc_0) ldvar=il.Create(OpCodes.Ldloc_0);
          else if(st.OpCode==OpCodes.Stloc_S||st.OpCode==OpCodes.Stloc) ldvar=il.Create(OpCodes.Ldloc,(VariableDefinition)st.Operand);
          else { Console.WriteLine("store inesperado: "+st.OpCode); continue; }
          var c=il.Create(OpCodes.Call, wat);
          var lv=ldvar; var lp=il.Create(OpCodes.Ldstr,"/tmp/saveerr.txt");
          il.InsertAfter(st, lp); il.InsertAfter(lp, lv); il.InsertAfter(lv, c);
          n++; Console.WriteLine("injetado log de save-err em "+t.FullName);
        }
      }
    }
    asm.Write(); Console.WriteLine("total: "+n);
  }
  static IEnumerable<TypeDefinition> AllTypes(ModuleDefinition m){ foreach(var t in m.Types){ yield return t; foreach(var x in Nested(t)) yield return x; } }
  static IEnumerable<TypeDefinition> Nested(TypeDefinition t){ foreach(var x in t.NestedTypes){ yield return x; foreach(var y in Nested(x)) yield return y; } }
}
