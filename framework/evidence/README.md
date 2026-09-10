# Evidência física da framework

`CFW-EVIDENCE.json` é o catálogo sanitizado de claims independentes por
CFW/stack. Um nível histórico, comunitário, image-backed ou host/sintético nunca
se transforma em prova da candidata atual por semelhança de aparelho, GPU,
engine ou port.

O gate canônico é:

```sh
python3 -B framework/tests/test_cfw_evidence.py
python3 -B framework/tests/test_firmware_matrix_v3.py
```

## Regra de promoção

Um claim só recebe `receipt.present: true` quando todas as condições abaixo
forem verdadeiras:

1. existe receipt sanitizado versionado para o claim exato;
2. o receipt identifica o commit/tag testado da framework e o commit do port;
3. ZIP SHA-256, ELF SHA-256/build-id e generation ID estão completos;
4. os cenários declarados foram executados sobre esses mesmos bytes;
5. o manifesto privado dos arquivos brutos foi selado por SHA-256;
6. o receipt público não contém endereço, hostname, credencial, caminho
   pessoal, filename/origem do container do dono ou save;
7. o escopo do claim não excede o que os cenários provaram.

Ausência de receipt mantém `present: false` e a lacuna escrita. Claims nunca
são promovidos em lote nem herdam a prova de outro claim.

## Contrato de entrega do teste físico V3

O executor físico entrega um bundle privado, um índice sanitizado dos arquivos
citados e um resumo sanitizado com esta forma lógica. O exemplo abaixo é um
modelo, não um receipt:

```json
{
  "schema": "nx-v3-physical-receipt-v1",
  "schema_version": 1,
  "claim_id": "CLAIM_ID_FROM_CFW_EVIDENCE",
  "evidence_level": "physical-full-scoped",
  "evidence_index_ref": "receipts/PRIVATE-BUNDLE-INDEX.sha256",
  "evidence_index_sha256": "64_HEX",
  "evidence_path_prefix": "device-class",
  "framework": {
    "tested_commit": "40_HEX",
    "tested_tag": "IMMUTABLE_RC_OR_COMPONENT_TAG",
    "aggregate_release_tag": null,
    "composition": "base-tag-plus-pinned-components",
    "components_sha256": "SHA256_OF_GENERATION_COMPONENTS_FILE",
    "nxrelease_manifest_file_sha256": "SHA256_OF_PACKED_NXRELEASE_MANIFEST_FILE",
    "component_commits": {
      "component-name": "40_HEX"
    }
  },
  "port": {
    "id": "PORT_ID",
    "version": "PORT_VERSION",
    "commit": "40_HEX"
  },
  "artifact": {
    "zip_sha256": "64_HEX",
    "zip_size": 1,
    "executable_member": "PORT_ID/PORT_ID-nextos",
    "executable_sha256": "64_HEX",
    "executable_build_id": "GNU_BUILD_ID_HEX",
    "generation_id": "64_HEX"
  },
  "artifact_proofs": {
    "candidate_manifest": {
      "evidence_ref": "candidate/CANDIDATE.sha256",
      "evidence_sha256": "64_HEX"
    },
    "freeze": {
      "evidence_ref": "candidate/FREEZE.json",
      "evidence_sha256": "64_HEX"
    }
  },
  "stack": {
    "cfw": "SANITIZED_CFW_NAME_AND_BUILD",
    "hardware_class": "SANITIZED_HARDWARE_CLASS",
    "renderer": "MEASURED_RENDERER",
    "graphics_api": "MEASURED_API_AND_VERSION",
    "drawable": "WIDTHxHEIGHT"
  },
  "scenarios": {
    "clean_install": {
      "passed": true,
      "evidence_ref": "device-class/clean-install.log",
      "evidence_sha256": "64_HEX"
    }
  },
  "claim_bindings": {
    "exact scenario text from CFW-EVIDENCE.json": ["clean_install"]
  },
  "single_channel": {
    "route": "native-r8-swizzle_OR_luminance-alpha-dup",
    "evidence_ref": "device-class/graphics.log",
    "evidence_sha256": "64_HEX"
  },
  "owner_state": {
    "before_evidence": {
      "evidence_ref": "device-class/prestate.txt",
      "evidence_sha256": "64_HEX"
    },
    "after_evidence": {
      "evidence_ref": "device-class/poststate.txt",
      "evidence_sha256": "64_HEX"
    },
    "compared": {
      "home": {"before_sha256": "64_HEX", "after_sha256": "64_HEX"},
      "gamedata": {"before_sha256": "64_HEX", "after_sha256": "64_HEX"},
      "gptk": {"before_sha256": "64_HEX", "after_sha256": "64_HEX"},
      "settings": {"before_sha256": "64_HEX", "after_sha256": "64_HEX"}
    },
    "byte_identical": true
  },
  "gaps": ["every excluded or unsealed scenario"],
  "private_manifest_sha256": "64_HEX",
  "sanitized": true
}
```

Cada par `evidence_ref`/`evidence_sha256` precisa existir no índice sanitizado.
O índice é derivado do manifesto privado depois de uma auditoria do bundle, mas
não incorpora os arquivos brutos. O gate versionado valida o receipt, o índice e
suas ligações; ele não substitui a auditoria semântica prévia do bundle privado.
Os dois receipts de GPU da mesma rodada devem repetir o mesmo `framework`,
`port`, `artifact` e `artifact_proofs`; divergência reprova a alegação de mesmos
bytes em múltiplos devices.

`components_sha256` é o SHA-256 do arquivo `components.sha256` da geração; não
é o digest de entrada do FREEZE nem o SHA do manifesto do nxrelease. O segundo
fica em `nxrelease_manifest_file_sha256`, sempre com rótulo distinto.

## Importação no catálogo

Depois de validar o bundle:

- guardar somente o receipt sanitizado sob `framework/evidence/receipts/`;
- guardar junto o índice sanitizado contendo apenas os arquivos citados;
- definir `sanitized_ref` como caminho relativo a `framework/evidence/`;
- copiar o SHA-256 do manifesto privado para `private_manifest_sha256`;
- atualizar `framework_ref` para a identidade exata testada;
- listar em `scenarios` somente os casos que passaram;
- remover de `gaps` apenas as lacunas satisfeitas pelo receipt;
- manter intactos todos os demais claims;
- rodar os dois gates acima e a bateria completa antes da tag agregada.

O gate abre cada `sanitized_ref` e índice marcado como presente e falha fechado
se houver arquivo ausente, symlink, fuga de `framework/evidence/receipts`,
literal privado, índice alterado, `claim_id`/nível divergente, cenário sem
ligação exata, identidade ausente, tag que não resolve ao commit declarado,
estado do dono divergente ou desacordo de artefato entre receipts da mesma
rodada. Os hashes do manifesto privado permanecem selos de uma auditoria feita
antes da importação; os arquivos privados não são abertos pelo gate público.
