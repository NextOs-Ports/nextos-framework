using System; using System.Linq; using System.Collections.Generic;
using Mono.Cecil; using Mono.Cecil.Cil;
// coopsplit <dll>
//   Habilita co-op local 2/3/4P no SOR4. O build mobile bloqueia o co-op em DUAS camadas:
//
//   (A) CommonLib.platform::update_game_pad_state_array funde slot[0] |= slot[1..3] (todo input
//       vai pro Player 1) e zera slot[1..3] (isConnected=false). FIX: truncar o metodo apos a
//       Fase 1 (so a leitura dos pads fisicos distintos), descartando fusao+limpeza. Assim cada
//       pad fica no seu slot e is_controller_connected(1/2/3) volta a ser true.
//
//   (B) CharacterSelectionScreen::handle_input_handle_first_button_presses (o handler de JOIN que
//       varre os controladores e, ao apertar 'select' num pad nao-atribuido, chama
//       assign_controller_to_new_local_player) esta DEFINIDO mas NUNCA E CHAMADO neste build -> o
//       join de P2/3/4 nunca dispara; o handle_input() pula direto pro 'second_button_presses'
//       (confirm), entao o A do 2o pad cai no P1. FIX: inserir a chamada do join em handle_input()
//       ANTES do clear_unconnected/second_button_presses, com early-return quando um join ocorre
//       (pra a tecla de join nao tambem confirmar o P1 no mesmo frame).
//
//   As duas correcoes sao necessarias juntas: (A) mantem o pad conectado pra (B) o join "pegar" e
//   o clear_unconnected nao remover o player recem-criado. Seguro p/ 1 player.
class CoopSplit {
  static int Main(string[] a){
    if(a.Length<1){ Console.Error.WriteLine("uso: coopsplit <dll>"); return 2; }
    var asm=AssemblyDefinition.ReadAssembly(a[0], new ReaderParameters{ReadWrite=true});
    int truncated=0, wired=0;

    // ---- (A) truncar update_game_pad_state_array apos a Fase 1 ----
    foreach(var t in AllTypes(asm.MainModule)) foreach(var m in t.Methods){
      if(!m.HasBody || m.Name!="update_game_pad_state_array") continue;
      var instrs=m.Body.Instructions;
      int cut=-1;
      for(int i=0;i+2<instrs.Count;i++){
        var ins=instrs[i];
        if(ins.OpCode!=OpCodes.Blt && ins.OpCode!=OpCodes.Blt_S) continue;
        if(instrs[i+1].OpCode!=OpCodes.Ldc_I4_1) continue;
        var n2=instrs[i+2];
        bool stV4=(n2.OpCode==OpCodes.Stloc_S||n2.OpCode==OpCodes.Stloc)
                  && n2.Operand is VariableDefinition vd && vd.Index==4;
        if(!stV4) continue;
        if(cut!=-1){ Console.Error.WriteLine("ERRO: corte (A) ambiguo"); return 3; }
        cut=i;
      }
      if(cut==-1){ Console.Error.WriteLine("ERRO: ponto de corte (A) nao achado"); return 4; }
      var removed=new HashSet<Instruction>();
      for(int i=cut+1;i<instrs.Count;i++) removed.Add(instrs[i]);
      for(int i=0;i<=cut;i++){
        var ins=instrs[i];
        if(ins.Operand is Instruction tgt && removed.Contains(tgt)){ Console.Error.WriteLine("ERRO: salto mantido->removido (A)"); return 5; }
        if(ins.Operand is Instruction[] arr && arr.Any(removed.Contains)){ Console.Error.WriteLine("ERRO: switch->removido (A)"); return 5; }
      }
      int before=instrs.Count;
      var il=m.Body.GetILProcessor();
      while(instrs.Count>cut+1) il.Remove(instrs[instrs.Count-1]);
      il.Append(il.Create(OpCodes.Ret));
      Console.WriteLine("coopsplit (A): "+t.FullName+"."+m.Name+" truncado "+before+"->"+instrs.Count+" instrucoes");
      truncated++;
    }

    // ---- (B) chamar o handler de JOIN dentro de CharacterSelectionScreen::handle_input ----
    var css = AllTypes(asm.MainModule).FirstOrDefault(t => t.Name=="CharacterSelectionScreen");
    if(css==null){ Console.Error.WriteLine("ERRO: tipo CharacterSelectionScreen nao achado"); return 7; }
    var join = css.Methods.FirstOrDefault(m => m.Name=="handle_input_handle_first_button_presses");
    var hin  = css.Methods.FirstOrDefault(m => m.Name=="handle_input" && m.ReturnType.FullName=="System.Void" && m.HasBody);
    if(join==null){ Console.Error.WriteLine("ERRO: metodo de join nao achado"); return 8; }
    if(hin==null){ Console.Error.WriteLine("ERRO: handle_input(void) nao achado"); return 9; }
    // ja foi fiado? (idempotente)
    bool already = hin.Body.Instructions.Any(i => (i.OpCode==OpCodes.Call||i.OpCode==OpCodes.Callvirt)
                     && i.Operand is MethodReference mr0 && mr0.Name=="handle_input_handle_first_button_presses");
    if(already){ Console.WriteLine("coopsplit (B): handle_input ja chama o join (idempotente)"); }
    else {
      var body=hin.Body;
      // ancora: a chamada a handle_input_clear_unconnected_player_states (roda antes do confirm)
      var anchorCall = body.Instructions.FirstOrDefault(i => (i.OpCode==OpCodes.Call||i.OpCode==OpCodes.Callvirt)
                         && i.Operand is MethodReference mr && mr.Name=="handle_input_clear_unconnected_player_states");
      if(anchorCall==null){ Console.Error.WriteLine("ERRO: ancora clear_unconnected nao achada em handle_input"); return 10; }
      var anchorLdarg = anchorCall.Previous; // ldarg.0 que empurra o 'this' p/ a chamada-ancora
      if(anchorLdarg==null || anchorLdarg.OpCode!=OpCodes.Ldarg_0){ Console.Error.WriteLine("ERRO: ldarg.0 da ancora inesperado"); return 11; }
      var il=body.GetILProcessor();
      var i1=il.Create(OpCodes.Ldarg_0);
      var i2=il.Create(OpCodes.Call, join);
      var i3=il.Create(OpCodes.Brfalse, anchorLdarg); // sem join -> segue p/ a ancora (fluxo normal)
      var i4=il.Create(OpCodes.Ret);                  // join ocorreu -> early-out (nao confirma P1 no mesmo frame)
      il.InsertBefore(anchorLdarg, i1);
      il.InsertBefore(anchorLdarg, i2);
      il.InsertBefore(anchorLdarg, i3);
      il.InsertBefore(anchorLdarg, i4);
      // CRITICO: o handle_input tem branches que saltam DIRETO p/ a ancora (clear_unconnected),
      // pulando por cima do bloco inserido. Redireciona esses branches p/ a 1a instrucao do join
      // (i1) -> assim o caminho ativo da tela SEMPRE passa pela checagem de join. (i3, o nosso
      // proprio brfalse, continua apontando p/ a ancora.)
      int retargets=0;
      foreach(var ins in body.Instructions){
        if(ins==i3) continue;
        if(ins.Operand is Instruction tgt && tgt==anchorLdarg){ ins.Operand=i1; retargets++; }
        else if(ins.Operand is Instruction[] arr){ for(int k=0;k<arr.Length;k++) if(arr[k]==anchorLdarg){ arr[k]=i1; retargets++; } }
      }
      Console.WriteLine("coopsplit (B): join fiado em handle_input (early-return; "+retargets+" branches redirecionados p/ a checagem)");
      wired++;
    }

    if(truncated==0){ Console.Error.WriteLine("ERRO: (A) nao aplicado"); return 6; }
    asm.Write(); Console.WriteLine("OK coopsplit: A(trunc)="+truncated+" B(join-wired)="+wired);
    return 0;
  }
  static IEnumerable<TypeDefinition> AllTypes(ModuleDefinition m){ foreach(var t in m.Types){ yield return t; foreach(var x in Nested(t)) yield return x; } }
  static IEnumerable<TypeDefinition> Nested(TypeDefinition t){ foreach(var x in t.NestedTypes){ yield return x; foreach(var y in Nested(x)) yield return y; } }
}
