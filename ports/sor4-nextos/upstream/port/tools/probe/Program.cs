using System; using System.IO; using System.Threading; using System.Linq; using System.Collections.Generic;
using Mono.Cecil; using Mono.Cecil.Cil;
// probe <SOR4.dll>: no inicio de GameScreen::update_gui loga path do guiDataRef + thread.
class Probe {
  static IEnumerable<TypeDefinition> All(ModuleDefinition m){foreach(var t in m.Types){yield return t;foreach(var n in Nest(t))yield return n;}}
  static IEnumerable<TypeDefinition> Nest(TypeDefinition t){foreach(var n in t.NestedTypes){yield return n;foreach(var x in Nest(n))yield return x;}}
  static void Main(string[] a){
    var asm=AssemblyDefinition.ReadAssembly(a[0], new ReaderParameters{ReadWrite=true});
    var m=asm.MainModule;
    var gs=All(m).First(t=>t.FullName=="BeatThemAll.MetaGame.GameScreen");
    var upd=gs.Methods.First(x=>x.Name=="update_gui" && x.Parameters.Count==0);
    var guiDataRefFld=gs.Fields.First(f=>f.Name=="guiDataRef");
    var giType=(GenericInstanceType)guiDataRefFld.FieldType;
    var yamlDef=giType.Resolve().Methods.First(x=>x.Name=="yaml_serialize");
    var yamlRef=new MethodReference(yamlDef.Name, m.ImportReference(yamlDef.ReturnType), giType){HasThis=true};
    // refs via reflexao (resolve assembly certo)
    var getErr=m.ImportReference(typeof(Console).GetProperty("Error").GetGetMethod());
    var wl=m.ImportReference(typeof(TextWriter).GetMethod("WriteLine", new[]{typeof(string)}));
    var concat4=m.ImportReference(typeof(string).GetMethod("Concat", new[]{typeof(string),typeof(string),typeof(string),typeof(string)}));
    var curThread=m.ImportReference(typeof(Thread).GetProperty("CurrentThread").GetGetMethod());
    var thrName=m.ImportReference(typeof(Thread).GetProperty("Name").GetGetMethod());
    var il=upd.Body.GetILProcessor();
    var first=upd.Body.Instructions[0];
    void Ins(Instruction i)=>il.InsertBefore(first,i);
    Ins(il.Create(OpCodes.Call, getErr));               // TextWriter
    Ins(il.Create(OpCodes.Ldstr, "[PROBE] path="));
    Ins(il.Create(OpCodes.Ldarg_0)); Ins(il.Create(OpCodes.Ldflda, guiDataRefFld)); Ins(il.Create(OpCodes.Call, yamlRef)); // path
    Ins(il.Create(OpCodes.Ldstr, " thr="));
    Ins(il.Create(OpCodes.Call, curThread)); Ins(il.Create(OpCodes.Callvirt, thrName));   // thread name
    Ins(il.Create(OpCodes.Call, concat4));              // 4 strings
    Ins(il.Create(OpCodes.Callvirt, wl));
    asm.Write(); Console.WriteLine("probe v2 injetado");
  }
}
