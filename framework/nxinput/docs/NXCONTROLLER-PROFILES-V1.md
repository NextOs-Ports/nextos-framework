# NXCONTROLLER_PROFILES/1 — bundle de mappings por CFW dentro do ZIP

Contrato do artefato content-addressed (nome sugerido `controllers.nxb`) que
um port V4 pode carregar como **autoridade 3** da ordem soberana
(`nxinput_sovereign.h`): depois do mapping vivo do `control.txt/get_controls`
e do banco oficial corrente do CFW, antes do built-in do runtime.

## Forma

Arquivo texto UTF-8, LF:

```text
NXCONTROLLER_PROFILES/1
# supplier=<fornecedor oficial, ex.: portmaster-gui>
# supplier_commit=<commit oficial do PortMaster/CFW de onde as linhas vieram>
# source_sha256=<sha256 do banco oficial usado como fonte>
# dialect=sdl2-gamecontrollerdb
# platform=Linux
# license=<licença do banco upstream>
# coverage=<n> guids
# claim=<limites honestos; nunca um claim universal>
<linha SDL por GUID, byte-intacta do banco oficial, uma por GUID>
...
```

## Regras

- **Linha 1 é literal** `NXCONTROLLER_PROFILES/1`; sem ela o leitor recusa
  (`bundle-header-invalid`).
- Linhas `#` são metadados/comentários; o resto são entradas SDL2
  `gamecontrollerdb` **byte-intactas** do banco oficial de origem.
- **CFW e versão são só chave de fornecedor/path** — jamais decidem A/B/L2/R2;
  quem decide é a entrada exata por GUID + capacidades medidas + readback.
- Deduplicação por (GUID, domínio/plataforma). Duplicata byte-idêntica
  colapsa; duplicata divergente do mesmo GUID/domínio é **excluída do bundle
  e registrada no manifesto** (a ordem nunca pode decidir).
- Somente o domínio declarado (hoje `platform=Linux`).
- **Proibido**: ROM, executável de jogo, endereço, hostname, IP, caminho
  pessoal, nome de usuário ou qualquer dado pessoal. O builder e o gate do
  nxrelease falham fechado se encontrarem.
- Content-addressed: o SHA-256 do arquivo é a identidade; `nxgenerator` fixa
  esse hash no opt-in declarativo e `nxrelease` valida o pacote contra ele.
  Nada de `latest`, nada de download em runtime.
- Ausência do opt-in preserva bytes e comportamento anteriores do port.

## Builder canônico

`tools/nx-controller-profiles.py` — determinístico (duas execuções sobre a
mesma fonte produzem bytes idênticos), emite `controllers.nxb` +
`controllers.nxb.sha256` + `controllers.nxb.manifest.json` (fonte, commit,
licença, contagens, conflitos excluídos, limites de claim).

## Variantes de FACE_LAYOUT (nxinput 0.10.0)

O schema do bundle **continua V1** — não existe `NXCONTROLLER_PROFILES/2`.
O que muda é a POLÍTICA de composição quando o CFW trata A/B e X/Y como
preferência do usuário (duas linhas oficiais e opostas para o mesmo GUID):

- `controllers.nxb` (base): somente GUIDs/layouts **invariantes**. Uma linha
  mutável modern/retro jamais entra na base — congelá-la foi exatamente o
  defeito de campo do 0.9.0.
- `controllers-modern.nxb` / `controllers-retro.nxb`: bundles V1 completos,
  cada um com a linha oficial da sua variante. O par é obrigatório quando o
  port declara variantes; os dois devem ter o mesmo conjunto de identidades
  e ser idênticos fora da diferença autorizada A/B/X/Y + metadata de
  proveniência.
- `FACE_LAYOUT = auto` declara SOMENTE a base; `modern`/`retro` declaram a
  variante correspondente. Em todos os casos o arquivo declarado é apenas a
  **autoridade 3**: env viva e banco oficial corrente sempre vencem.
- Os três arquivos são regulares, não-symlink, bounded e pinados por SHA-256
  (`controls.controller_profiles` + `face_layout_variants` no opt-in
  declarativo; o empacotador valida o trio byte-exato).
