using System; using System.Linq; using System.Collections.Generic;
using Mono.Cecil; using Mono.Cecil.Cil;
// coopdiag <dll>
//   Instrumenta CharacterSelectionScreen::handle_input_handle_first_button_presses (o handler de
//   JOIN) com Console.Error.WriteLine (-> log.txt) p/ localizar onde o join de P2 quebra:
//     [COOPDIAG] FBP-ENTER   no inicio do metodo  -> confirma que fix B fez o metodo SER CHAMADO.
//     [COOPDIAG] FBP-ASSIGN  antes do assign_controller_to_new_local_player -> condicoes do join OK.
//   Sem ENTER  -> a chamada do join nao executa (fix B nao pegou).
//   ENTER sem ASSIGN -> is_just_pressed(c,select)/can_assign/already-assigned bloqueiam.
//   ENTER+ASSIGN -> join dispara; problema e' a jusante (clear/second_button_presses).
class CoopDiag {
  static int Main(string[] a){
    var asm=AssemblyDefinition.ReadAssembly(a[0], new ReaderParameters{ReadWrite=true});
    var mod=asm.MainModule;
    var getErr = mod.ImportReference(typeof(System.Console).GetMethod("get_Error"));
    var wl     = mod.ImportReference(typeof(System.IO.TextWriter).GetMethod("WriteLine", new[]{typeof(string)}));
    var css = AllTypes(mod).FirstOrDefault(t=>t.Name=="CharacterSelectionScreen");
    var m = css?.Methods.FirstOrDefault(x=>x.Name=="handle_input_handle_first_button_presses");
    if(m==null||!m.HasBody){ Console.Error.WriteLine("ERRO: metodo nao achado"); return 4; }
    var il=m.Body.GetILProcessor();
    var body=m.Body;
    // marker no ENTER
    var first=body.Instructions[0];
    foreach(var ins in new[]{ il.Create(OpCodes.Call,getErr), il.Create(OpCodes.Ldstr,"[COOPDIAG] FBP-ENTER"), il.Create(OpCodes.Callvirt,wl) })
      il.InsertBefore(first, ins);
    // marker antes do assign_controller_to_new_local_player
    var assignCall = body.Instructions.FirstOrDefault(i=>(i.OpCode==OpCodes.Call||i.OpCode==OpCodes.Callvirt)
                       && i.Operand is MethodReference mr && mr.Name=="assign_controller_to_new_local_player");
    if(assignCall!=null){
      // recuar ate o inicio do "statement" do assign (ldarg.0 que empurra o this)
      var anchor=assignCall;
      while(anchor.Previous!=null && anchor.Previous.OpCode!=OpCodes.Ldarg_0) anchor=anchor.Previous;
      if(anchor.Previous!=null) anchor=anchor.Previous; // o ldarg.0
      foreach(var ins in new[]{ il.Create(OpCodes.Call,getErr), il.Create(OpCodes.Ldstr,"[COOPDIAG] FBP-ASSIGN"), il.Create(OpCodes.Callvirt,wl) })
        il.InsertBefore(anchor, ins);
      Console.WriteLine("coopdiag: markers ENTER+ASSIGN inseridos");
    } else { Console.WriteLine("coopdiag: so ENTER (assign call nao achado)"); }
    asm.Write(); return 0;
  }
  static IEnumerable<TypeDefinition> AllTypes(ModuleDefinition m){ foreach(var t in m.Types){ yield return t; foreach(var x in Nested(t)) yield return x; } }
  static IEnumerable<TypeDefinition> Nested(TypeDefinition t){ foreach(var x in t.NestedTypes){ yield return x; foreach(var y in Nested(x)) yield return y; } }
}
