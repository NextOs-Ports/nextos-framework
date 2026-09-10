#!/usr/bin/env python3
# wwise_extract.py - parser OFFLINE dos soundbanks Wwise do SoR4 (bank version 135).
# Mapeia event_name(FNV) -> wem(s) reais (via HIRC) e extrai cada wem embarcado (DIDX) como .opus
# (o chunk "data" do wem 0x3040 JA E um Ogg Opus padrao -> toca direto na libopus/OpenAL).
# Saida: <outdir>/<wem_id>.opus  +  manifest.txt (linhas "event_id <tab> wem_id[,wem_id...]")
#
# RESOLUCAO CORRETA (a chave do som de combate):
#   * Event(0x04): segue SO as acoes de PLAY (actionType high-byte == 0x04). Acoes de
#     Stop/SetState/SetSwitch (0x01/0x02/0x03...) NAO produzem som e levavam o parser antigo
#     a varrer a arvore inteira -> wem generico unico para todo golpe.
#   * Action(0x03): pega o target.
#   * Sound(0x02): pega o sourceID (o wem real).
#   * Containers RanSeq(0x05)/Switch(0x06)/ActorMixer(0x07)/Layer(0x09): le a CHILD-LIST
#     REAL (numChildren u32 + numChildren*childID u32). O bloco vem depois do NodeBaseParams
#     de tamanho variavel; achamos o primeiro bloco "N + N ids" em que TODOS os ids existem
#     no HIRC e sao de tipo tocavel (Sound/RanSeq/Switch/ActorMixer/Layer). Validado contra a
#     estrutura real (playlist/switch-list referenciam exatamente esses filhos).
import struct, sys, os

PLAYABLE = {0x02, 0x05, 0x06, 0x07, 0x09}  # Sound, RanSeq, Switch, ActorMixer, Layer/Blend

def fnv1_32(name):
    h = 2166136261
    for c in name.lower().encode("ascii"):
        h = (h * 16777619) & 0xFFFFFFFF
        h ^= c
    return h

def parse_bank(path):
    d = open(path, "rb").read()
    version = None; didx = {}; data_off = None; hirc_objs = {}
    i = 0
    while i < len(d) - 8:
        tag = d[i:i+4]; sz = struct.unpack("<I", d[i+4:i+8])[0]; body = d[i+8:i+8+sz]
        if tag == b"BKHD":
            version = struct.unpack("<I", body[0:4])[0]
        elif tag == b"DIDX":
            for k in range(sz // 12):
                wid, off, wsz = struct.unpack("<III", body[k*12:k*12+12])
                didx[wid] = (off, wsz)
        elif tag == b"DATA":
            data_off = i + 8
        elif tag == b"HIRC":
            num = struct.unpack("<I", body[0:4])[0]; p = 4
            for _ in range(num):
                otype = body[p]; osize = struct.unpack("<I", body[p+1:p+5])[0]
                ob = body[p+5:p+5+osize]
                oid = struct.unpack("<I", ob[0:4])[0]
                hirc_objs[oid] = (otype, ob)
                p += 5 + osize
        i += 8 + sz
    return d, version, didx, data_off, hirc_objs

def parse_event_actions(ob):
    # Event(type 0x04): id(4), count(u8), count*actionId(u32).
    cnt = ob[4]; acts = []; p = 5
    for _ in range(cnt):
        if p+4 > len(ob): break
        acts.append(struct.unpack("<I", ob[p:p+4])[0]); p += 4
    return acts

def parse_action(ob):
    # Action(type 0x03): id(4), u16 actionType, u32 idExt(target), ...
    if len(ob) < 10: return None
    atype = struct.unpack("<H", ob[4:6])[0]
    target = struct.unpack("<I", ob[6:10])[0]
    return atype, target

def parse_sound_source(ob):
    # Sound(type 0x02): id(4), u32 pluginID, u8 streamType, u32 sourceID, u32 fileID, ...
    if len(ob) < 14: return None
    streamType = ob[8]
    sourceID = struct.unpack("<I", ob[9:13])[0]
    return streamType, sourceID

def find_children(ob, hirc):
    # Acha a CHILD-LIST real de um container: primeiro bloco (N u32) + (N ids u32) em que
    # todos os ids sao objetos tocaveis do HIRC. Ignora a posicao 4 (=ulID). N>=1.
    for off in range(4, len(ob) - 4):
        n = struct.unpack("<I", ob[off:off+4])[0]
        if n < 1 or n > 256: continue
        if off + 4 + 4*n > len(ob): continue
        ids = struct.unpack("<%dI" % n, ob[off+4:off+4+4*n])
        if all(c in hirc and hirc[c][0] in PLAYABLE for c in ids):
            return list(ids)
    return []

def collect_leaves(oid, hirc, seen, depth=0):
    # Desce a hierarquia: Sound->source, Containers->child-list real. Retorna lista de sourceIDs.
    if oid in seen or depth > 16 or oid not in hirc: return []
    seen.add(oid)
    otype, ob = hirc[oid]
    res = []
    if otype == 0x02:  # Sound
        s = parse_sound_source(ob)
        if s: res.append(s[1])  # sourceID
    elif otype in (0x05, 0x06, 0x07, 0x09):  # RanSeq/Switch/ActorMixer/Layer
        for cid in find_children(ob, hirc):
            res += collect_leaves(cid, hirc, seen, depth+1)
    return res

def resolve_event(eid, hirc):
    # Event -> SO acoes de Play -> target -> folhas. Retorna sourceIDs em ordem, sem dup.
    otype, ob = hirc[eid]
    wems = []
    for a in parse_event_actions(ob):
        if a not in hirc or hirc[a][0] != 0x03: continue
        pa = parse_action(hirc[a][1])
        if not pa: continue
        atype, target = pa
        if (atype >> 8) != 0x04:  # SO Play (0x04xx)
            continue
        wems += collect_leaves(target, hirc, set())
    out = []; s = set()
    for w in wems:
        if w not in s: s.add(w); out.append(w)
    return out

def extract_wem(src, WEM_SRC, outdir, extracted):
    if src not in WEM_SRC or src in extracted: return src in extracted
    d, pos, wsz = WEM_SRC[src]
    wem = d[pos:pos+wsz]
    j = 12; data = None
    while j < len(wem) - 8:
        t = wem[j:j+4]; s = struct.unpack("<I", wem[j+4:j+8])[0]
        if t == b"data": data = wem[j+8:j+8+s]; break
        j += 8 + s + (s & 1)
    if data and data[:4] == b"OggS":
        open(os.path.join(outdir, "%d.opus" % src), "wb").write(data); extracted.add(src); return True
    return False

# Quantos wem variantes por evento gravar no manifest (o reimpl sorteia 1 por disparo).
MAX_IDS = int(os.environ.get("SOR4_MAX_VARIANTS", "8"))

def main():
    banks = sys.argv[1].split(",")
    outdir = sys.argv[2]
    os.makedirs(outdir, exist_ok=True)
    ALL_HIRC = {}; WEM_SRC = {}
    for bp in banks:
        d, ver, didx, data_off, hirc = parse_bank(bp)
        sys.stderr.write("bank %s ver=%s didx=%d hirc=%d\n" % (os.path.basename(bp), ver, len(didx), len(hirc)))
        ALL_HIRC.update(hirc)
        if data_off is not None:
            for wid, (off, wsz) in didx.items():
                WEM_SRC[wid] = (d, data_off + off, wsz)
    extracted = set(); manifest = []; nstream = 0; nmulti = 0
    for eid, (otype, ob) in ALL_HIRC.items():
        if otype != 0x04: continue  # Event
        srcs = resolve_event(eid, ALL_HIRC)
        emb = []
        for src in srcs:
            if src in WEM_SRC:
                if extract_wem(src, WEM_SRC, outdir, extracted): emb.append(src)
            else:
                nstream += 1
            if len(emb) >= MAX_IDS: break
        if emb:
            if len(emb) > 1: nmulti += 1
            manifest.append((eid, emb))
    # ---- music_ids.txt: wem IDs que sao MUSICA (referenciados por MusicSegment/Track/RanSeq
    # no HIRC). O reimpl (wwise_native.c) roteia ESSES p/ ao_music_request (UMA fonte, que
    # SUBSTITUI a anterior) em vez de ao_play_streamed_sfx (PARALELO) -> corrige a musica
    # sobreposta. O limiar por TAMANHO classificava ~300 segmentos de musica <1.5MB como SFX
    # paralelo (uma musica por cima da outra). Aqui usamos o HIRC (preciso, 0 overlap c/ SFX).
    import glob
    banks_dir = os.path.dirname(os.path.abspath(banks[0]))
    ga_wems = set()
    for p in glob.glob(os.path.join(banks_dir, "*.wem")):
        try: ga_wems.add(int(os.path.basename(p)[:-4]))
        except Exception: pass
    music_u32 = set()
    for oid, (ot, ob) in ALL_HIRC.items():
        if ot in (0x0A, 0x0B, 0x0D):  # MusicSegment, MusicTrack, MusicRanSeq
            for off in range(0, len(ob)-3, 4): music_u32.add(struct.unpack_from("<I", ob, off)[0])
            for off in range(2, len(ob)-3, 4): music_u32.add(struct.unpack_from("<I", ob, off)[0])
    music_ids = sorted(ga_wems & music_u32)
    with open(os.path.join(outdir, "music_ids.txt"), "w") as f:
        f.write("\n".join(str(m) for m in music_ids))
    sys.stderr.write("music_ids=%d (de %d wems no gameassets) -> %s/music_ids.txt\n"
                     % (len(music_ids), len(ga_wems), outdir))

    with open(os.path.join(outdir, "manifest.txt"), "w") as f:
        for eid, ids in manifest:
            f.write("%d\t%s\n" % (eid, ",".join(str(x) for x in ids)))
    sys.stderr.write("eventos c/ SFX embarcado=%d (multi-variante=%d)  wems extraidos=%d  refs streamed=%d -> %s\n"
                     % (len(manifest), nmulti, len(extracted), nstream, outdir))

if __name__ == "__main__":
    main()
