using System; using System.Linq; using System.Collections.Generic;
using Mono.Cecil; using Mono.Cecil.Cil;
// gfxlow <SOR4.dll> [Tipo::Campo=valor ...]
// Zera (ou ajusta) os DEFAULTS de campos setados no .ctor -> reduz CMA no gameplay.
// Padrao SoR4 (sem args): em GfxConfig::.ctor, forca shadowQuality=0(off), ambianceQuality=0(off),
// shadowBlobEnabled=0. Esses campos alimentam GameDrawer::setup_scalable_rendertarget; com off, os
// RTs shadow/ambiance NAO sao alocados (libera CMA exatamente ao abrir a fase). reflectionQuality
// ja nasce 0. Sem Config.txt o jogo usa estes defaults do .ctor.
// Tecnica: acha cada `stfld <Campo>` no .ctor do tipo dono; a instrucao ANTERIOR e o ldc do valor
// -> troca por ldc.i4 <valor>.
class GfxLow {
  static IEnumerable<TypeDefinition> All(ModuleDefinition m){foreach(var t in m.Types){yield return t;foreach(var n in Nest(t))yield return n;}}
  static IEnumerable<TypeDefinition> Nest(TypeDefinition t){foreach(var n in t.NestedTypes){yield return n;foreach(var x in Nest(n))yield return x;}}

  // (TipoSimples, Campo) -> valor
  static readonly List<(string typ,string fld,int val)> DEFAULTS = new(){
    ("GfxConfig","shadowQuality",0),
    ("GfxConfig","ambianceQuality",0),
    ("GfxConfig","shadowBlobEnabled",0),
  };

  static void Main(string[] a){
    if(a.Length<1){ Console.Error.WriteLine("uso: gfxlow <SOR4.dll> [Tipo::Campo=valor ...]"); Environment.Exit(2); }
    var targets = new List<(string typ,string fld,int val)>();
    foreach(var s in a.Skip(1)){
      // formato Tipo::Campo=valor
      var eq=s.Split('='); var tc=eq[0].Split(new[]{"::"},StringSplitOptions.None);
      targets.Add((tc[0], tc[1], eq.Length>1?int.Parse(eq[1]):0));
    }
    if(targets.Count==0) targets = DEFAULTS;

    var asm=AssemblyDefinition.ReadAssembly(a[0], new ReaderParameters{ReadWrite=true});
    var m=asm.MainModule; int n=0;
    foreach(var t in All(m)){
      foreach(var meth in t.Methods){
        if(meth.Name!=".ctor" || !meth.HasBody) continue;
        var il=meth.Body.GetILProcessor();
        // copia a lista (vamos mexer ao iterar)
        foreach(var ins in meth.Body.Instructions.ToList()){
          if(ins.OpCode!=OpCodes.Stfld) continue;
          var fr=ins.Operand as FieldReference; if(fr==null) continue;
          foreach(var (typ,fld,val) in targets){
            if(fr.Name!=fld) continue;
            // casa o tipo dono pelo nome simples (NestedType: t.Name == "GfxConfig")
            if(t.Name!=typ && !t.FullName.EndsWith("/"+typ)) continue;
            var prev=ins.Previous;
            if(prev==null) continue;
            il.Replace(prev, il.Create(OpCodes.Ldc_I4, val));
            n++; Console.WriteLine($"gfxlow {t.Name}::{fld} -> {val}");
          }
        }
      }
    }
    asm.Write(); Console.WriteLine("gfxlow patched: "+n);
    if(n==0) Environment.Exit(2);
  }
}
