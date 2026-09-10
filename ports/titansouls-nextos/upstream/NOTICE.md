# Titan Souls compatibility-port notices / Avisos do port

The compatibility loader, adapter and packaging integration in this directory
are part of `nextos_ports_android`, Copyright 2026 NextOS Project contributors,
and are distributed under GNU General Public License version 3 only. The complete
text is in `LICENSE`.

The project follows interoperability techniques developed in the free-software
Android native-porting community. Required acknowledgements include mtojek's
Apache-2.0 Android native-loader work, initdream's Crazy Taxi work and
Producdevity's MIT-licensed Call of Duty: Black Ops Zombies work. Their
copyrights and licences remain their own; this notice does not relicense those
projects.

The adapter uses the external NextOS `nxloader`, `nxandroid`, `nxcompat`, `nxgl`,
`nxinput` and `nxaudio` components at build time; their source is not duplicated
in the standalone port-source export. The public data-free ZIP materializes the
canonical NXExtract 1.2.6 runtime unmodified under the MIT licence and includes
its `licenses/NXExtract-MIT.txt`. SDL2, EGL, OpenGL ES and standard Linux
libraries are supplied by the target firmware and are not bundled.

Titan Souls © Acid Nerve / Devolver Digital. The Android port is by Abstraction
Games. FMOD Ex © Firelight Technologies Pty Ltd. The game, APK, OBB, Android
engine libraries, FMOD Ex binary, assets, artwork, music, sound effects, saves,
marks and all other owner data remain proprietary works of their respective
rightsholders. They are not covered by the GPL or MIT licences and are never
present in the public ZIP. Users must provide them from their own lawful Android
copy.

The unmodified official store media under `docs/images/` is included only for
identification and documentation on the project page. It is not a runtime or
build input, is excluded from the release ZIP, and remains © Acid Nerve /
Devolver Digital.

This independent interoperability project is not official and is not affiliated
with or endorsed by Acid Nerve, Devolver Digital, Abstraction Games, Firelight
Technologies, Google or another rightsholder.

---

O loader, o adapter e a integração de empacotamento desta pasta fazem parte do
`nextos_ports_android`, Copyright 2026 contribuidores do Projeto NextOS, e são
distribuídos sob GNU GPL versão 3 somente. O texto completo está em `LICENSE`.

O export standalone contém somente fontes específicas do port. Os componentes
comuns do framework entram pelo build externo, e o runtime redistribuível do
NXExtract é materializado somente no ZIP público sem dados do jogo.

O jogo, APK, OBB, bibliotecas Android, binário FMOD Ex, assets, arte, música,
efeitos, saves e marcas pertencem aos respectivos titulares. Eles não são
cobertos pelas licenças livres deste projeto e nunca entram no ZIP público. Cada
usuário deve fornecer os dados de sua própria cópia Android obtida legalmente.

As mídias oficiais sem modificação em `docs/images/` servem somente para
identificação e documentação na página do projeto. Elas não entram no runtime,
no build nem no ZIP da release e continuam © Acid Nerve / Devolver Digital.
