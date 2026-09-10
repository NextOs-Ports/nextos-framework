"""Lê o anel de carimbos que o port escreve (src/probe_ring.c) e reconstrói a
linha do tempo do frame travado.

O log formatado não serve para esse bug: medido, ligar TR_JNILOG fez os frames
de 2385ms desaparecerem -- o atraso do próprio instrumento deixava a cadeia do
timer do Lime se re-armar.  Por isso o port só grava stores num anel mapeado, e
a interpretação acontece aqui fora.

Uso:  python3 read_probe.py probe.bin [quantos_ms_antes_do_travamento]
"""
import struct, sys

HDR = struct.Struct("<IIQI12x")          # magica, registros, escritas, congelado
REC = struct.Struct("<IHBBII")           # us, tid, kind, pad, a, b
MAGIC = 0x50524254

SLEEP, SWAP, EVENT = 1, 2, 3
BUCKET = {0: "outro", 1: "WaitEvent(principal)", 2: "Timer(SDL)"}

path = sys.argv[1]
window_ms = float(sys.argv[2]) if len(sys.argv) > 2 else 2500.0

blob = open(path, "rb").read()
magic, capacity, written, frozen = HDR.unpack_from(blob, 0)
if magic != MAGIC:
    sys.exit("arquivo sem a magica: o port ainda nao inicializou o anel")

print("registros gravados: %d | capacidade: %d | congelado: %s"
      % (written, capacity, "sim" if frozen else "nao"))
if written > capacity:
    print("(anel deu a volta: os %d mais antigos se perderam)" % (written - capacity))

start = max(0, written - capacity)
recs = []
for i in range(start, written):
    off = HDR.size + (i % capacity) * REC.size
    us, tid, kind, _pad, a, b = REC.unpack_from(blob, off)
    recs.append((us, tid, kind, a, b))
recs.sort()

# O frame travado é o alvo: achar o pior SWAP e mostrar o que veio antes dele.
worst = None
for us, tid, kind, a, b in recs:
    if kind == SWAP and (worst is None or b > worst[4]):
        worst = (us, tid, kind, a, b)
if not worst:
    sys.exit("nenhum frame registrado ainda")

t_end = worst[0]
t_ini = t_end - window_ms * 1000
print("\npior frame: #%d levou %d ms (em t=%.3fs)\n"
      % (worst[3], worst[4], t_end / 1e6))

# O que aconteceu na janela: quem dormiu, quanto, e se algum evento entrou.
sleeps = {}
total_ms = {}
events = []
frames = []
for us, tid, kind, a, b in recs:
    if not (t_ini <= us <= t_end):
        continue
    if kind == SLEEP:
        key = (tid, b)
        sleeps[key] = sleeps.get(key, 0) + 1
        total_ms[key] = total_ms.get(key, 0) + a
    elif kind == EVENT:
        events.append((us, a))
    elif kind == SWAP:
        frames.append((us, a, b))

print("na janela de %.0f ms antes desse frame:" % window_ms)
for (tid, bucket), n in sorted(sleeps.items(), key=lambda kv: -kv[1]):
    print("  tid %-6d %-22s %6d sonos, %6d ms pedidos"
          % (tid, BUCKET.get(bucket, "?"), n, total_ms[(tid, bucket)]))

print("  eventos empurrados: %d" % len(events))
if events:
    for us, a in events[:10]:
        print("     t=%+8.1f ms  tipo=0x%x" % ((us - t_end) / 1000.0, a))

print("  frames apresentados: %d" % len(frames))

# A conclusão que interessa: silêncio total durante a espera aponta timer que
# não disparou; evento chegando sem frame sair aponta consumo.
if not events:
    print("\n=> NENHUM evento entrou enquanto a thread principal esperava:")
    print("   o timer do Lime nao disparou.  Suspeito = calculo do deadline.")
else:
    print("\n=> Evento(s) entraram na espera; se o frame so saiu bem depois,")
    print("   o problema e o CONSUMO do evento, nao o disparo do timer.")
