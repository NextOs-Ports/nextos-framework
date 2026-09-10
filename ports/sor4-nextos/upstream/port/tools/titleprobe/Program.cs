using System; using System.IO; using System.Linq; using System.Collections.Generic;
using Mono.Cecil; using Mono.Cecil.Cil; using Mono.Cecil.Rocks;
// titleprobe <SOR4.dll>: instrumenta TitleScreen.handle_input p/ logar (no Console.Error):
//   - heartbeat throttled (prova que handle_input EH chamado)
//   - quando QUALQUER botao do pad current[0] esta pressionado OU loading: loga
//     loading, jbp(=is_any_button_just_pressed(true)), curMask, prevMask, conn.
// curMask/prevMask = bitmask a|b|x|y|start|back (bits 0..5). jbp=resultado da query.
class TitleProbe {
  static IEnumerable<TypeDefinition> All(ModuleDefinition m){foreach(var t in m.Types){yield return t;foreach(var n in Nest(t))yield return n;}}
  static IEnumerable<TypeDefinition> Nest(TypeDefinition t){foreach(var n in t.NestedTypes){yield return n;foreach(var x in Nest(n))yield return x;}}

  static void Main(string[] a){
    var asm=AssemblyDefinition.ReadAssembly(a[0], new ReaderParameters{ReadWrite=true});
    var m=asm.MainModule;
    var ts=All(m).First(t=>t.FullName=="BeatThemAll.MetaGame.TitleScreen");
    var hi=ts.Methods.First(x=>x.Name=="handle_input" && x.Parameters.Count==0);

    var inputT=All(m).First(t=>t.FullName=="BeatThemAll.MetaGame.input");
    var jbp=inputT.Methods.First(x=>x.Name=="is_any_button_just_pressed");
    var curFld=inputT.Fields.First(f=>f.Name=="currentGamePadStateArray");
    var prevFld=inputT.Fields.First(f=>f.Name=="previousGamePadStateArray");
    var gps=All(m).First(t=>t.FullName=="CommonLib.GamePadState_s");
    var fA=gps.Fields.First(f=>f.Name=="a");   var fB=gps.Fields.First(f=>f.Name=="b");
    var fX=gps.Fields.First(f=>f.Name=="x");   var fY=gps.Fields.First(f=>f.Name=="y");
    var fSt=gps.Fields.First(f=>f.Name=="start"); var fBk=gps.Fields.First(f=>f.Name=="back");
    var fConn=gps.Fields.First(f=>f.Name=="isConnected");
    var loadingFld=ts.Fields.First(f=>f.Name=="loading");

    // promove visibilidade dos campos que vou ler por IL cross-type (JIT exige acesso real)
    void Pub(FieldDefinition f){ f.Attributes = (f.Attributes & ~FieldAttributes.FieldAccessMask) | FieldAttributes.Public; }
    Pub(curFld); Pub(prevFld);
    Pub(fA); Pub(fB); Pub(fX); Pub(fY); Pub(fSt); Pub(fBk); Pub(fConn);

    // refs externos
    var getErr=m.ImportReference(typeof(Console).GetProperty("Error").GetGetMethod());
    var wl=m.ImportReference(typeof(TextWriter).GetMethod("WriteLine", new[]{typeof(string)}));
    var concat=m.ImportReference(typeof(string).GetMethod("Concat", new[]{typeof(object[])}));
    var boolBox=m.ImportReference(typeof(bool));
    var int32Box=m.ImportReference(typeof(int));

    // helper: campo estatico contador de heartbeat
    var hbFld=new FieldDefinition("__hb", FieldAttributes.Static|FieldAttributes.Private, m.TypeSystem.Int32);
    ts.Fields.Add(hbFld);

    var il=hi.Body.GetILProcessor();
    hi.Body.SimplifyMacros();
    var first=hi.Body.Instructions[0];

    // ---- monta bitmask helper inline: empilha int do estado de array[0] ----
    // gera codigo que, dado o array field, empilha (a|b<<1|x<<2|y<<3|start<<4|back<<5)
    List<Instruction> Mask(FieldReference arrFld){
      var ins=new List<Instruction>();
      void E(Instruction i)=>ins.Add(i);
      // val = 0
      E(il.Create(OpCodes.Ldc_I4_0));
      void Bit(FieldReference f,int shift){
        // val = val | (arr[0].f ? (1<<shift) : 0)
        E(il.Create(OpCodes.Ldsfld, arrFld));
        E(il.Create(OpCodes.Ldc_I4_0));
        E(il.Create(OpCodes.Ldelema, gps));
        E(il.Create(OpCodes.Ldfld, f));
        E(il.Create(OpCodes.Ldc_I4, 1<<shift));
        E(il.Create(OpCodes.Mul));   // bool(0/1) * (1<<shift)
        E(il.Create(OpCodes.Or));
      }
      Bit(fA,0); Bit(fB,1); Bit(fX,2); Bit(fY,3); Bit(fSt,4); Bit(fBk,5);
      return ins;
    }

    // Estrategia: no inicio, calcula curMask e guarda em local; se (curMask!=0 || loading) loga detalhado.
    // Sempre: hb++ ; se (hb % 180 ==0) loga heartbeat.
    var locMask=new VariableDefinition(m.TypeSystem.Int32); hi.Body.Variables.Add(locMask);
    var locConn=new VariableDefinition(m.TypeSystem.Boolean); hi.Body.Variables.Add(locConn);

    var instrs=new List<Instruction>();
    void Ins(Instruction i)=>instrs.Add(i);

    // ---- heartbeat ----
    var afterHB=il.Create(OpCodes.Nop);
    Ins(il.Create(OpCodes.Ldsfld, hbFld));
    Ins(il.Create(OpCodes.Ldc_I4_1));
    Ins(il.Create(OpCodes.Add));
    Ins(il.Create(OpCodes.Stsfld, hbFld));
    Ins(il.Create(OpCodes.Ldsfld, hbFld));
    Ins(il.Create(OpCodes.Ldc_I4, 180));
    Ins(il.Create(OpCodes.Rem));
    Ins(il.Create(OpCodes.Brtrue, afterHB));
    Ins(il.Create(OpCodes.Call, getErr));
    Ins(il.Create(OpCodes.Ldstr, "[HB] handle_input alive"));
    Ins(il.Create(OpCodes.Callvirt, wl));
    Ins(afterHB);

    // ---- curMask -> locMask ----
    foreach(var i in Mask(curFld)) Ins(i);
    Ins(il.Create(OpCodes.Stloc, locMask));

    // ---- conn -> locConn ----
    Ins(il.Create(OpCodes.Ldsfld, curFld));
    Ins(il.Create(OpCodes.Ldc_I4_0));
    Ins(il.Create(OpCodes.Ldelema, gps));
    Ins(il.Create(OpCodes.Ldfld, fConn));
    Ins(il.Create(OpCodes.Stloc, locConn));

    // ---- if (locMask!=0 || this.loading) { log detailed } ----
    var doLog=il.Create(OpCodes.Nop);
    var afterLog=il.Create(OpCodes.Nop);
    Ins(il.Create(OpCodes.Ldloc, locMask));
    Ins(il.Create(OpCodes.Brtrue, doLog));
    Ins(il.Create(OpCodes.Ldarg_0));
    Ins(il.Create(OpCodes.Ldfld, loadingFld));
    Ins(il.Create(OpCodes.Brfalse, afterLog));
    Ins(doLog);

    // Console.Error.WriteLine(string.Concat(new object[]{ "[HI] loading=", this.loading, " jbp=", jbp(true), " cur=", locMask, " prev=", prevMask, " conn=", locConn }))
    Ins(il.Create(OpCodes.Call, getErr));            // TextWriter
    // build object[10]
    Ins(il.Create(OpCodes.Ldc_I4, 10));
    Ins(il.Create(OpCodes.Newarr, m.TypeSystem.Object));
    void Elem(int idx, Action push, TypeReference boxT){
      Ins(il.Create(OpCodes.Dup));
      Ins(il.Create(OpCodes.Ldc_I4, idx));
      push();
      if(boxT!=null) Ins(il.Create(OpCodes.Box, boxT));
      Ins(il.Create(OpCodes.Stelem_Ref));
    }
    Elem(0, ()=>Ins(il.Create(OpCodes.Ldstr, "[HI] loading=")), null);
    Elem(1, ()=>{ Ins(il.Create(OpCodes.Ldarg_0)); Ins(il.Create(OpCodes.Ldfld, loadingFld)); }, boolBox);
    Elem(2, ()=>Ins(il.Create(OpCodes.Ldstr, " jbp=")), null);
    Elem(3, ()=>{ Ins(il.Create(OpCodes.Ldc_I4_1)); Ins(il.Create(OpCodes.Call, jbp)); }, boolBox);
    Elem(4, ()=>Ins(il.Create(OpCodes.Ldstr, " cur=")), null);
    Elem(5, ()=>Ins(il.Create(OpCodes.Ldloc, locMask)), int32Box);
    Elem(6, ()=>Ins(il.Create(OpCodes.Ldstr, " prev=")), null);
    Elem(7, ()=>{ foreach(var i in Mask(prevFld)) Ins(i); }, int32Box);
    Elem(8, ()=>Ins(il.Create(OpCodes.Ldstr, " conn=")), null);
    Elem(9, ()=>Ins(il.Create(OpCodes.Ldloc, locConn)), boolBox);
    Ins(il.Create(OpCodes.Call, concat));
    Ins(il.Create(OpCodes.Callvirt, wl));
    Ins(afterLog);

    // insere tudo antes da primeira instrucao
    foreach(var i in instrs) il.InsertBefore(first, i);

    hi.Body.OptimizeMacros();
    asm.Write();
    Console.WriteLine("titleprobe injetado em TitleScreen.handle_input");
  }
}
