using System; using System.Linq; using System.Collections.Generic;
using Mono.Cecil; using Mono.Cecil.Cil;
// skipvideo <SOR4.dll>
// Pula a intro: em program::reset_game troca 'newobj StartGameVideoScreen::.ctor()'
// por 'newobj TitleScreen::.ctor()' -> cai direto no menu (destino pos-intro).
// Mali nao decodifica video; SuperVideoPlayer e' so stub. Indo ao TitleScreen evita
// a tela de video (que da NullReferenceException com videoPlayer null).
class SkipVideo {
  static void Main(string[] a){
    var asm=AssemblyDefinition.ReadAssembly(a[0], new ReaderParameters{ReadWrite=true});
    var m=asm.MainModule;
    var titleCtor=AllTypes(m).First(t=>t.FullName=="BeatThemAll.MetaGame.TitleScreen")
                  .Methods.First(x=>x.Name==".ctor" && x.Parameters.Count==0);
    int patched=0;
    foreach(var t in AllTypes(m)) foreach(var meth in t.Methods){
      if(meth.FullName.Replace('/','.').Contains("program::reset_game")==false) continue;
      if(!meth.HasBody) continue;
      var ins=meth.Body.Instructions; var il=meth.Body.GetILProcessor();
      for(int i=0;i<ins.Count;i++){
        if(ins[i].OpCode==OpCodes.Newobj && ins[i].Operand is MethodReference mr
           && mr.DeclaringType.Name=="StartGameVideoScreen" && mr.Name==".ctor"){
          il.Replace(ins[i], il.Create(OpCodes.Newobj, titleCtor));
          patched++;
          Console.WriteLine("patchado reset_game: StartGameVideoScreen -> TitleScreen em "+meth.FullName);
        }
      }
    }
    asm.Write(); Console.WriteLine("skipvideo patched: "+patched);
    if(patched==0){ Console.WriteLine("AVISO: nenhum newobj StartGameVideoScreen em reset_game!"); Environment.Exit(2);}
  }
  static IEnumerable<TypeDefinition> AllTypes(ModuleDefinition m){ foreach(var t in m.Types){ yield return t; foreach(var x in Nested(t)) yield return x; } }
  static IEnumerable<TypeDefinition> Nested(TypeDefinition t){ foreach(var n in t.NestedTypes){ yield return n; foreach(var x in Nested(n)) yield return x; } }
}
