#!/usr/bin/env python3
"""Desmonta um .unity3d (UnityFS) no layout SOLTO que a Unity ja procura.

Por que existe: o Sally Face entrega o conteudo num asset pack
(`datapack.unity3d`) que, na maquina do jogador, o Play Core montaria.  Fora do
Android nao ha Play Core: o `libunity` so aceita montar esse arquivo quando o
caminho do pacote e' um diretorio cujo nome CONTEM "UnityDataAssetPack" **e**
a enquete de estado responde COMPLETED antes do primeiro frame -- e mesmo assim
ele nunca chegou a pedir o caminho.

O caminho que o proprio engine ja tenta sozinho e' o antigo: ele faz
`stat("assets/bin/Data/resources.assets")` e as variantes `.resS`/`.split0`.
Entao em vez de emular o Play Core, entregamos exatamente isso -- os arquivos
serializados soltos, byte a byte como estao dentro do container.  Sem Play
Core, sem asset pack, sem reserializacao: cada entrada sai identica.

Uso:  unbundle_datapack.py <arquivo.unity3d> <diretorio-de-saida>
"""
import os
import sys

import UnityPy


def unbundle(bundle_path, out_dir):
    env = UnityPy.load(bundle_path)
    bundle = list(env.files.values())[0]
    entries = getattr(bundle, "files", None)
    if not entries:
        raise SystemExit(f"{bundle_path}: nao e' um container com arquivos internos")

    os.makedirs(out_dir, exist_ok=True)
    total = 0
    for name, entry in entries.items():
        reader = getattr(entry, "reader", entry)
        data = bytes(reader.bytes)
        target = os.path.join(out_dir, name)
        os.makedirs(os.path.dirname(target), exist_ok=True)
        with open(target, "wb") as fh:
            fh.write(data)
        total += len(data)
        print(f"  {name}  {len(data)} B")
    print(f"{os.path.basename(bundle_path)}: {len(entries)} arquivos, {total} B")
    return len(entries), total


def main(argv):
    if len(argv) != 3:
        print(__doc__)
        return 2
    unbundle(argv[1], argv[2])
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
