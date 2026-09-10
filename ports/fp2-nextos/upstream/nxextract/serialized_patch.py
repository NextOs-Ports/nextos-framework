"""Reescrita CIRURGICA de um SerializedFile da Unity.

Por que nao usar `UnityPy.save()`: medido neste pacote, um save de no-op ja
muda 68.027 bytes e encolhe o arquivo em ~2 KB.  E' a mesma classe de armadilha
que ja matou a Unity noutro port ("bundle remontado pelo UnityPy").  Aqui nada
e' re-serializado: o header, os tipos e TODOS os objetos que nao vao mudar
saem byte a byte iguais.  So' os objetos alvo trocam de conteudo e a tabela de
objetos e' recalculada -- ela tem largura fixa, entao o metadata nao muda de
tamanho e nada mais se move.

O teste acido esta em `selftest()`: uma reescrita sem nenhuma troca tem de sair
IDENTICA ao arquivo original.
"""
import struct

RECORD = struct.Struct("<qIIi")   # path_id, byte_start(rel), byte_size, type_id
HEADER = struct.Struct(">IIII")   # metadata_size, file_size, version, data_offset


class SerializedFileLayout:
    def __init__(self, blob, objects):
        """objects: (path_id, byte_start_abs, byte_size) NA ORDEM DA TABELA.

        Ordem de tabela e ordem de dados NAO sao a mesma coisa: em
        `globalgamemanagers.assets` deste pacote a tabela desvia da ordem de
        offset a partir da entrada 1579.  Ordenar por offset fazia a busca da
        tabela falhar.  Aqui a ordem da tabela e' preservada como veio, e o
        layout dos dados usa uma ordem separada, por byte_start.
        """
        self.blob = bytes(blob)
        self.metadata_size, self.file_size, self.version, self.data_offset = \
            HEADER.unpack_from(self.blob, 0)
        if self.file_size != len(self.blob):
            raise ValueError("file_size %d != tamanho real %d" %
                             (self.file_size, len(self.blob)))
        self.objects = list(objects)
        self.table_offset = self._find_table()
        self.count_offset = self._find_count()

    def _find_table(self):
        """Acha a tabela de objetos procurando o registro do PRIMEIRO objeto e
        conferindo que TODOS os outros seguem em sequencia.  Uma unica batida
        casual e' impossivel de sobreviver a essa conferencia."""
        first = RECORD.pack(self.objects[0][0],
                            self.objects[0][1] - self.data_offset,
                            self.objects[0][2], 0)[:16]
        limit = min(self.data_offset, len(self.blob))
        start = 0
        found = []
        while True:
            hit = self.blob.find(first, start, limit)
            if hit < 0:
                break
            start = hit + 1
            if self._table_fits(hit):
                found.append(hit)
        if len(found) != 1:
            raise ValueError("tabela de objetos ambigua: %d candidatos" % len(found))
        return found[0]

    def _find_count(self):
        """Offset do inteiro que diz quantos objetos existem.

        Ele fica logo antes da tabela, mas com padding de alinhamento no meio:
        medido neste pacote, a distancia varia entre 4 e 6 bytes conforme o
        arquivo.  Procurar pelo VALOR e' o unico jeito honesto.
        """
        want = len(self.objects)
        for back in (4, 5, 6, 7, 8):
            at = self.table_offset - back
            if at < 0:
                continue
            if struct.unpack_from("<i", self.blob, at)[0] == want:
                return at
        raise ValueError("contagem de objetos nao encontrada antes da tabela")

    def _table_fits(self, offset):
        for index, (path_id, byte_start, byte_size) in enumerate(self.objects):
            at = offset + index * RECORD.size
            if at + RECORD.size > self.data_offset:
                return False
            pid, rel, size, _ = RECORD.unpack_from(self.blob, at)
            if pid != path_id or rel != byte_start - self.data_offset \
                    or size != byte_size:
                return False
        return True

    def alignment(self):
        """Alinhamento real entre objetos, deduzido do proprio arquivo."""
        order = sorted(self.objects, key=lambda o: o[1])
        gaps = []
        for index in range(len(order) - 1):
            end = order[index][1] + order[index][2]
            gaps.append(order[index + 1][1] - end)
        return gaps

    def rebuild(self, replacements, additions=()):
        """replacements: {path_id: bytes}.  additions: [(path_id, type_id, bytes)].

        Acrescentar objeto muda o tamanho do METADATA, coisa que trocar
        conteudo nao muda.  Entao a tabela cresce, tudo o que vem depois dela
        dentro do metadata desliza, a contagem sobe e o `data_offset` e'
        recalculado (mantendo o mesmo alinhamento do arquivo original).  Os
        `byte_start` sao relativos ao data_offset, entao nada mais precisa
        saber que ele andou.
        """
        table_end = self.table_offset + RECORD.size * len(self.objects)
        extra = RECORD.size * len(additions)
        head = bytearray(self.blob[:table_end]) + bytearray(extra) + \
               bytearray(self.blob[table_end:self.data_offset])
        if additions:
            struct.pack_into("<i", head, self.count_offset,
                             len(self.objects) + len(additions))
            for index, (path_id, type_id, payload) in enumerate(additions):
                RECORD.pack_into(head, table_end + index * RECORD.size,
                                 path_id, 0, len(payload), type_id)
        data = bytearray()
        placed = {}
        order = sorted(self.objects, key=lambda o: o[1])
        order = order + [(path_id, None, None)
                         for path_id, _type_id, _payload in additions]
        payloads = {path_id: payload for path_id, _t, payload in additions}
        for index, (path_id, byte_start, byte_size) in enumerate(order):
            payload = replacements.get(path_id)
            if payload is None and byte_start is None:
                payload = payloads[path_id]
            if payload is None:
                payload = self.blob[byte_start:byte_start + byte_size]
            placed[path_id] = (len(data), len(payload))
            data += payload
            if index < len(order) - 1:
                # Preservar o mesmo alinhamento de 8 bytes que a Unity usa
                # entre objetos; o ultimo nao leva padding.
                while len(data) % 8:
                    data.append(0)

        for index, (path_id, _, _) in enumerate(self.objects):
            rel, size = placed[path_id]
            at = self.table_offset + index * RECORD.size
            _, _, _, type_id = RECORD.unpack_from(self.blob, at)
            RECORD.pack_into(head, at, path_id, rel, size, type_id)
        for index, (path_id, type_id, _payload) in enumerate(additions):
            rel, size = placed[path_id]
            RECORD.pack_into(head, table_end + index * RECORD.size,
                             path_id, rel, size, type_id)

        data_offset = self.data_offset
        if extra:
            # O metadata cresceu: empurrar o inicio dos dados mantendo o mesmo
            # alinhamento que o arquivo original usava.
            align = 4096 if self.data_offset % 4096 == 0 else 16
            while data_offset < len(head):
                data_offset += align
            head += bytearray(data_offset - len(head))

        out = bytearray(bytes(head) + bytes(data))
        HEADER.pack_into(out, 0, self.metadata_size + extra, len(out),
                         self.version, data_offset)
        return bytes(out)


def from_unitypy(path, env_file):
    objects = [(o.path_id, o.byte_start, o.byte_size)
               for o in env_file.objects.values()]
    with open(path, "rb") as fh:
        blob = fh.read()
    return SerializedFileLayout(blob, objects)
