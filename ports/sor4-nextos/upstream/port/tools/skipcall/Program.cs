using System; using System.Linq; using System.Collections.Generic;
using Mono.Cecil; using Mono.Cecil.Cil;
// skipcall <dll> <Method|Type.Method> <CalleeName>
//   Remove a chamada (call/callvirt) a <CalleeName> DENTRO de <Method> BALANCEANDO a pilha.
//   Usado p/ tirar o save_save_game REDUNDANTE do startup (program.initialize) sem disparar o
//   save (que crasha na init do audio). 🔑 O NOP simples (versao antiga) deixava o 'this' + args
//   na pilha -> corrompia o metodo 'initialize' -> QUEBRAVA o setup de input do jogo (controle
//   morto no "TAP ANYWHERE"). Agora troca o 'call' por N 'pop' (N = nargs + this) e empurra um
//   default se o retorno for usado -> stack balanceada -> initialize integro -> input OK.
class SkipCall {
  static void Main(string[] a){
    var asm=AssemblyDefinition.ReadAssembly(a[0], new ReaderParameters{ReadWrite=true});
    string method=a[1], callee=a[2]; int n=0;
    foreach(var t in AllTypes(asm.MainModule)) foreach(var m in t.Methods){
      if(!m.HasBody) continue;
      if(m.Name!=method && (t.Name+"."+m.Name)!=method && (t.FullName+"."+m.Name)!=method) continue;
      var il=m.Body.GetILProcessor();
      foreach(var ins in m.Body.Instructions.ToList()){
        if((ins.OpCode==OpCodes.Call||ins.OpCode==OpCodes.Callvirt) && ins.Operand is MethodReference mr && mr.Name==callee){
          int npop = mr.Parameters.Count + (mr.HasThis ? 1 : 0);
          var repl = new List<Instruction>();
          for(int i=0;i<npop;i++) repl.Add(il.Create(OpCodes.Pop));
          var rt = mr.ReturnType; string rn = rt.FullName;
          if(rn != "System.Void"){
            if(rn=="System.Single") repl.Add(il.Create(OpCodes.Ldc_R4, 0f));
            else if(rn=="System.Double") repl.Add(il.Create(OpCodes.Ldc_R8, 0d));
            else if(rn=="System.Int64"||rn=="System.UInt64") repl.Add(il.Create(OpCodes.Ldc_I8, 0L));
            else if(rt.IsValueType) repl.Add(il.Create(OpCodes.Ldc_I4_0)); // int/bool/etc
            else repl.Add(il.Create(OpCodes.Ldnull));                       // ref
          }
          if(repl.Count==0) repl.Add(il.Create(OpCodes.Nop));
          il.Replace(ins, repl[0]);
          for(int i=1;i<repl.Count;i++) il.InsertAfter(repl[i-1], repl[i]);
          n++; Console.WriteLine("skipcall: "+t.FullName+"."+m.Name+" -> "+callee+" (pop x"+npop+", hasThis="+mr.HasThis+", args="+mr.Parameters.Count+", ret="+rn+")");
        }
      }
    }
    asm.Write(); Console.WriteLine("total skipcall: "+n);
  }
  static IEnumerable<TypeDefinition> AllTypes(ModuleDefinition m){ foreach(var t in m.Types){ yield return t; foreach(var x in Nested(t)) yield return x; } }
  static IEnumerable<TypeDefinition> Nested(TypeDefinition t){ foreach(var x in t.NestedTypes){ yield return x; foreach(var y in Nested(x)) yield return y; } }
}
