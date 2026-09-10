using System; using System.Linq;
using Mono.Cecil;
class AddFwd {
  static void Main(string[] a){
    // addfwd <System.Runtime.dll> ns name [ns name ...]
    var asm=AssemblyDefinitionExt(a[0]);
    var mod=asm.MainModule;
    var spc=mod.AssemblyReferences.FirstOrDefault(r=>r.Name=="System.Private.CoreLib");
    if(spc==null){ spc=new AssemblyNameReference("System.Private.CoreLib", new Version(9,0,0,0)){ PublicKeyToken=new byte[]{0x7c,0xec,0x85,0xd7,0xbe,0xa7,0x79,0x8e} }; mod.AssemblyReferences.Add(spc); }
    for(int i=1;i+1<a.Length;i+=2){
      string ns=a[i], name=a[i+1];
      if(mod.ExportedTypes.Any(e=>e.Namespace==ns && e.Name==name)){ Console.WriteLine("ja existe "+ns+"."+name); continue; }
      var et=new ExportedType(ns, name, mod, spc){ Attributes=TypeAttributes.Forwarder };
      mod.ExportedTypes.Add(et);
      Console.WriteLine("forward add: "+ns+"."+name+" -> System.Private.CoreLib");
    }
    asm.Write();
  }
  static AssemblyDefinition AssemblyDefinitionExt(string p)=>AssemblyDefinition.ReadAssembly(p, new ReaderParameters{ReadWrite=true});
}
