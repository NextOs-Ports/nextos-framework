# Patches MonoGame GLES p/ Mali-450 (SoR4 port)

Base: MonoGame 3.8.4 source (tarball). Build híbrido **SDL windowing (DESKTOPGL) + render GLES**.

## Como construir o MonoGame.Framework.dll (GLES, net9, v3.8.3.1)

O release completo é reconstruído por `../tools/build-managed-release.sh`. O script fixa
o source MonoGame v3.8.4 no commit `caaa1170c48b8d340690014423e1c8bbc0777899`,
valida o tarball, exige o SDK .NET 9.0.315 e gera MonoGame, bridge, host e ferramentas
sem PDB ou caminhos locais. Use `DOTNET=/caminho/para/dotnet` ao invocá-lo.

Para experimentar só o framework manualmente:

1. Baixar o source MonoGame 3.8.4. O csproj usa PackageReference para
   StbImageSharp/StbImageWriteSharp; os submódulos não são necessários.
2. `python3 apply.py <MonoGame>/MonoGame.Framework`
3. Copiar `MonoGame.Framework.SOR4GLES.csproj` para `<MonoGame>/MonoGame.Framework/`.
4. Passar `Sor4ReferenceDir` e `Sor4BridgeAssembly` ao MSBuild, ou usar os defaults
   sob `ports/sor4/build/`.

## Pontos-chave
- csproj define `OPENGL;GLES;...;DESKTOPGL` (híbrido) + StbImage via NuGet (sem STBSHARP_INTERNAL).
- Pede contexto SDL **ES profile 2.0**; BoundApi=ES; FBO via core GLES2.
- Desktop-only (PolygonMode) gateado `&& !GLES`. Outros desktop-only (MapBuffer/DrawBuffer)
  só em GetData/RenderTarget — tratar se crashar.
- Validado: renderiza (Clear) 20 frames no Mali-450 MP via sdl2-compat→SDL3-mali, EXIT=0.

## Stubs Android (FASE 3)
- `port/tools/stubber/` reescreve corpos de método → default e **retargeta corelib**
  System.Private.CoreLib → System.Runtime (p/ compilar contra os stubs no net9).
- Stubar (refs diretas do SOR4.dll): Mono.Android, Java.Interop, EOSSDK.Android,
  HelpshiftSDKx.Android, _Microsoft.Android.Resource.Designer, SharpFont.Core,
  Xamarin.Android.Google.BillingClient, Xamarin.Firebase.Config,
  Xamarin.Google.Android.Play.Core, Xamarin.GooglePlayServices.{Auth,Base,Basement,Games,Measurement.Api,Tasks}.
- MonoGame compat: `Platform/SOR4Compat.cs` adiciona `AndroidGameActivity : Android.App.Activity`
  (stub) + `Game.Activity`. csproj referencia stubs Mono.Android/Java.Interop.

## Host (port/host/)
- `sor4host`: AssemblyLoadContext.Resolving p/ resolver dlls do dir por nome simples (ignora versão/PKT).
- Boot: carrega SOR4.dll → `CommonLib.xna.CreateGame()` → `((Game)CommonLib.xna.game).Run()`.
  (replica MainActivity.OnCreate sem Android; SetContentView pulado, SDL faz a janela.)
