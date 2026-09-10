using System; using System.IO; using System.Linq;
using Mono.Cecil; using Mono.Cecil.Cil;
class Stubber {
  static void Main(string[] a){
    // uso: stubber <in.dll> <out.dll> [throw|default]
    string inp=a[0], outp=a[1]; bool doThrow = a.Length>2 && a[2]=="throw";
    var asm=AssemblyDefinition.ReadAssembly(inp);
    // retarget corelib: System.Private.CoreLib -> System.Runtime (facade net9, p/ compilar contra os stubs)
    foreach(var r in asm.MainModule.AssemblyReferences){
      if(r.Name=="System.Private.CoreLib"){
        r.Name="System.Runtime";
        r.PublicKeyToken=new byte[]{0xb0,0x3f,0x5f,0x7f,0x11,0xd5,0x0a,0x3a};
        r.Version=new Version(9,0,0,0);
      }
    }
    // remove CustomAttributes que referenciam tipos interop problematicos (retarget quebra reflection)
    void StripCA(Mono.Collections.Generic.Collection<CustomAttribute> ca){
      for(int i=ca.Count-1;i>=0;i--){ var n=ca[i].AttributeType.Name;
        if(n=="DefaultDllImportSearchPathsAttribute"||n=="UnmanagedFunctionPointerAttribute") ca.RemoveAt(i); }
    }
    StripCA(asm.CustomAttributes); StripCA(asm.MainModule.CustomAttributes);
    foreach(var t in AllTypes(asm.MainModule)){ StripCA(t.CustomAttributes);
      foreach(var m in t.Methods) StripCA(m.CustomAttributes);
      foreach(var fld in t.Fields) StripCA(fld.CustomAttributes); }
    int methods=0, types=0;
    foreach(var t in AllTypes(asm.MainModule)){
      types++;
      foreach(var m in t.Methods){
        if(!m.HasBody) continue;
        // pula construtores estaticos? nao - zera tudo p/ evitar init JNI
        var b=m.Body; b.Instructions.Clear(); b.Variables.Clear(); b.ExceptionHandlers.Clear();
        b.InitLocals=false;
        var il=b.GetILProcessor();
        if(doThrow && m.Name!=".cctor" && m.Name!=".ctor"){
          // throw new NotImplementedException() -- mas precisa ref; mais simples: default
        }
        EmitDefault(il, m);
        methods++;
      }
    }
    asm.Write(outp);
    Console.WriteLine($"{Path.GetFileName(outp)}: {types} tipos, {methods} metodos stubados");
  }
  static System.Collections.Generic.IEnumerable<TypeDefinition> AllTypes(ModuleDefinition m){
    foreach(var t in m.Types){ yield return t; foreach(var n in Nested(t)) yield return n; }
  }
  static System.Collections.Generic.IEnumerable<TypeDefinition> Nested(TypeDefinition t){
    foreach(var n in t.NestedTypes){ yield return n; foreach(var x in Nested(n)) yield return x; }
  }
  static void EmitDefault(ILProcessor il, MethodDefinition m){
    var rt=m.ReturnType;
    if(rt.MetadataType==MetadataType.Void){ il.Append(il.Create(OpCodes.Ret)); return; }
    if(rt.IsValueType || rt.IsGenericParameter){
      var v=new VariableDefinition(rt); m.Body.Variables.Add(v); m.Body.InitLocals=true;
      il.Append(il.Create(OpCodes.Ldloca_S, v));
      il.Append(il.Create(OpCodes.Initobj, rt));
      il.Append(il.Create(OpCodes.Ldloc_0));
      il.Append(il.Create(OpCodes.Ret));
    } else { il.Append(il.Create(OpCodes.Ldnull)); il.Append(il.Create(OpCodes.Ret)); }
  }
}
