using System; using System.Linq; using System.Collections.Generic;
using Mono.Cecil; using Mono.Cecil.Cil;
// dumpil <dll> <methodName> [full] [typeFilter]
class DumpIL {
  static void Main(string[] a){
    var asm=AssemblyDefinition.ReadAssembly(a[0]); string mn=a[1];
    bool full=a.Length>2 && a[2]=="full"; string tf=a.Length>3?a[3]:null;
    foreach(var t in AllTypes(asm.MainModule)) foreach(var m in t.Methods){
      if(m.Name!=mn) continue; if(tf!=null && !t.FullName.Contains(tf)) continue;
      Console.WriteLine("=== "+t.FullName+"."+m.Name+" static="+(!m.HasThis)+" ===");
      if(!m.HasBody) continue;
      if(m.Body.HasExceptionHandlers) foreach(var h in m.Body.ExceptionHandlers)
        Console.WriteLine("  [EH "+h.HandlerType+" try@"+h.TryStart.Offset.ToString("X")+"-"+h.TryEnd.Offset.ToString("X")+" handler@"+h.HandlerStart.Offset.ToString("X")+"]");
      foreach(var ins in m.Body.Instructions){
        if(full) Console.WriteLine("  IL_"+ins.Offset.ToString("X4")+": "+ins.OpCode+(ins.Operand!=null?" "+OpStr(ins.Operand):""));
        else if(ins.OpCode==OpCodes.Call||ins.OpCode==OpCodes.Callvirt||ins.OpCode==OpCodes.Newobj){
          var mr=ins.Operand as MethodReference; if(mr!=null) Console.WriteLine("  "+ins.OpCode+" -> "+mr.DeclaringType.Name+"::"+mr.Name); }
      }
    }
  }
  static string OpStr(object o){ var mr=o as MethodReference; if(mr!=null) return mr.DeclaringType.Name+"::"+mr.Name; if(o is string s) return "\""+s+"\""; if(o is Instruction i) return "IL_"+i.Offset.ToString("X4"); return o.ToString(); }
  static IEnumerable<TypeDefinition> AllTypes(ModuleDefinition m){ foreach(var t in m.Types){ yield return t; foreach(var x in Nested(t)) yield return x; } }
  static IEnumerable<TypeDefinition> Nested(TypeDefinition t){ foreach(var x in t.NestedTypes){ yield return x; foreach(var y in Nested(x)) yield return y; } }
}
