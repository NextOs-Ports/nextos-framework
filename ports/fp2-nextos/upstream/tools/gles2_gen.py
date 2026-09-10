"""Gera o GLSL ESSL100 de um pass da Unity 2018 no formato exato do pacote.

Nao ha invencao de convencao aqui: cada nome usado abaixo foi LIDO do proprio
APK -- `hlslcc_mtx4x4unity_ObjectToWorld[4]`, `hlslcc_mtx4x4unity_MatrixVP[4]`,
`in_POSITION0` / `in_COLOR0` / `in_TEXCOORD0`, `_MainTex_ST`,
`#define SV_Target0 gl_FragData[0]` -- das variantes GLES20 que a propria Unity
enviou em `unity default resources` (Hidden/Internal-Colored, GUI/Text Shader)
e das strings do `libunity.so`.

O programa e' UM SO texto com `#ifdef VERTEX` / `#ifdef FRAGMENT`, do jeito que
a Unity serializa GLSL: a entrada de vertice carrega a fonte inteira e a de
fragmento fica vazia.
"""

# Bits de m_SourceMap (mascara de canais de vertice da Unity).
CH_VERTEX, CH_NORMAL, CH_TANGENT, CH_COLOR, CH_UV0, CH_UV1 = 1, 2, 4, 8, 16, 32

# m_Type de uma propriedade de ShaderLab.
P_COLOR, P_VECTOR, P_FLOAT, P_RANGE, P_TEXTURE = 0, 1, 2, 3, 4


def _mvp_body():
    return """    u_xlat0 = in_POSITION0.yyyy * hlslcc_mtx4x4unity_ObjectToWorld[1];
    u_xlat0 = hlslcc_mtx4x4unity_ObjectToWorld[0] * in_POSITION0.xxxx + u_xlat0;
    u_xlat0 = hlslcc_mtx4x4unity_ObjectToWorld[2] * in_POSITION0.zzzz + u_xlat0;
    u_xlat0 = u_xlat0 + hlslcc_mtx4x4unity_ObjectToWorld[3];
    u_xlat1 = u_xlat0.yyyy * hlslcc_mtx4x4unity_MatrixVP[1];
    u_xlat1 = hlslcc_mtx4x4unity_MatrixVP[0] * u_xlat0.xxxx + u_xlat1;
    u_xlat1 = hlslcc_mtx4x4unity_MatrixVP[2] * u_xlat0.zzzz + u_xlat1;
    gl_Position = hlslcc_mtx4x4unity_MatrixVP[3] * u_xlat0.wwww + u_xlat1;
"""


def pick_texture(props, order):
    """Nome da textura a amostrar.  `_MainTex` quando existe; senao a PRIMEIRA
    textura declarada pelo shader -- shaders como Hidden/CubeBlend (`_TexA`) ou
    os de terreno (`_Control`) nao tem `_MainTex` e sem isso cairiam no ramo
    "sem textura"."""
    if props.get("_MainTex") == P_TEXTURE:
        return "_MainTex"
    for name in order:
        if props.get(name) == P_TEXTURE:
            return name
    return None


def generate(source_map, props, prop_order=(), label=None):
    """props: dict nome -> m_Type, lido do m_PropInfo do proprio shader."""
    has_color = bool(source_map & CH_COLOR)
    has_uv = bool(source_map & CH_UV0)
    texture = pick_texture(props, prop_order or sorted(props))
    has_tint = props.get("_Color") in (P_COLOR, P_VECTOR)
    has_cutoff = props.get("_Cutoff") in (P_FLOAT, P_RANGE)
    # Sem UV nao ha o que amostrar, mesmo que a propriedade exista.
    sample = texture is not None and has_uv

    # Familia de sprite da Unity.  `_RendererColor` so' existe nela, e ela tem
    # duas regras que NENHUM template generico adivinha:
    #  1. ALFA EM TEXTURA SEPARADA.  Textura ETC1 nao tem canal alfa, entao a
    #     Unity guarda o alfa numa segunda textura (`_AlphaTex`) e o shader faz
    #     `lerp(cor.a, alfa.r, _EnableExternalAlpha)`.  Sem isso todo sprite
    #     ETC1 sai OPACO -- foi exatamente o retangulo branco medido atras do
    #     logo na tela de titulo.  Neste pacote sao 1.476 texturas ETC_RGB4.
    #  2. Alfa PRE-MULTIPLICADO: o blend e' `One OneMinusSrcAlpha`, entao o
    #     fragmento precisa de `rgb *= a`.
    is_sprite = "_RendererColor" in props
    split_alpha = (sample and props.get("_AlphaTex") == P_TEXTURE and
                   "_EnableExternalAlpha" in props)
    premultiply = is_sprite and not has_cutoff

    # Marca de identidade: o GL nao sabe o nome do shader da Unity, entao sem
    # isto nao ha como dizer QUAL shader desenhou um quadrado errado na tela.
    # COMENTARIO NAO SERVE -- medido: a Unity pre-processa a fonte antes de
    # entrega-la ao driver e come os comentarios.  DECLARAR sem usar TAMBEM nao
    # serve: o otimizador GLSL da Unity descarta uniform nao usado (medido:
    # zero ocorrencia de FP2SHADER chegou ao glShaderSource).  Entao a marca e'
    # USADA, num teste que o otimizador nao consegue dobrar e que nunca e'
    # verdadeiro na pratica (o uniform nunca e' escrito, logo vale 0).
    mark = []
    guard = []
    if label:
        ident = "".join(c if c.isalnum() else "_" for c in label)
        mark = ["uniform float FP2SHADER_%s;" % ident]
        # 1e30 estoura o alcance de mediump e o Mali-450 nao tem highp no
        # fragmento.  1e4 cabe no minimo garantido de mediump (2^14) e o
        # uniform nunca e' escrito, entao o teste nunca e' verdadeiro.
        guard = ["    if (FP2SHADER_%s > 1e4) { discard; }" % ident]

    v = ["#ifdef VERTEX", "#version 100", ""] + [
         "uniform \tvec4 hlslcc_mtx4x4unity_ObjectToWorld[4];",
         "uniform \tvec4 hlslcc_mtx4x4unity_MatrixVP[4];"]
    if sample:
        v.append("uniform \tvec4 %s_ST;" % texture)
    v.append("attribute highp vec4 in_POSITION0;")
    if has_color:
        v.append("attribute mediump vec4 in_COLOR0;")
    if has_uv:
        v.append("attribute highp vec2 in_TEXCOORD0;")
    if has_color:
        v.append("varying mediump vec4 vs_COLOR0;")
    if has_uv:
        v.append("varying highp vec2 vs_TEXCOORD0;")
    v += ["vec4 u_xlat0;", "vec4 u_xlat1;", "void main()", "{"]
    v.append(_mvp_body().rstrip("\n"))
    if has_color:
        v.append("    vs_COLOR0 = in_COLOR0;")
    if has_uv:
        if sample:
            v.append("    vs_TEXCOORD0.xy = in_TEXCOORD0.xy * %s_ST.xy + "
                     "%s_ST.zw;" % (texture, texture))
        else:
            v.append("    vs_TEXCOORD0.xy = in_TEXCOORD0.xy;")
    v += ["    return;", "}", "", "#endif"]

    f = ["#ifdef FRAGMENT", "#version 100", ""] + [
         "#ifdef GL_FRAGMENT_PRECISION_HIGH",
         "    precision highp float;",
         "#else",
         "    precision mediump float;",
         "#endif",
         "precision highp int;"] + mark
    if sample:
        f.append("uniform lowp sampler2D %s;" % texture)
    if split_alpha:
        f.append("uniform lowp sampler2D _AlphaTex;")
        f.append("uniform mediump float _EnableExternalAlpha;")
    if is_sprite:
        f.append("uniform mediump vec4 _RendererColor;")
    if has_tint:
        f.append("uniform mediump vec4 _Color;")
    if has_cutoff:
        f.append("uniform mediump float _Cutoff;")
    if has_color:
        f.append("varying mediump vec4 vs_COLOR0;")
    if has_uv:
        f.append("varying highp vec2 vs_TEXCOORD0;")
    f += ["#define SV_Target0 gl_FragData[0]",
          "mediump vec4 u_xlat16_0;", "void main()", "{"] + guard
    if sample:
        f.append("    u_xlat16_0 = texture2D(%s, vs_TEXCOORD0.xy);" % texture)
    elif has_color or has_tint:
        f.append("    u_xlat16_0 = vec4(1.0, 1.0, 1.0, 1.0);")
    else:
        # Sem textura, sem cor de vertice e sem tint nao ha NADA que este pass
        # possa reproduzir de honesto.  Branco opaco enche a tela de retangulos
        # (medido na tela de titulo); o certo e' o pass ficar INVISIVEL.
        f.append("    u_xlat16_0 = vec4(0.0, 0.0, 0.0, 0.0);")
    if split_alpha:
        f.append("    u_xlat16_0.w = mix(u_xlat16_0.w, "
                 "texture2D(_AlphaTex, vs_TEXCOORD0.xy).x, "
                 "_EnableExternalAlpha);")
    if has_color:
        f.append("    u_xlat16_0 = u_xlat16_0 * vs_COLOR0;")
    if has_tint:
        f.append("    u_xlat16_0 = u_xlat16_0 * _Color;")
    if is_sprite:
        f.append("    u_xlat16_0 = u_xlat16_0 * _RendererColor;")
    if has_cutoff:
        f.append("    if (u_xlat16_0.w < _Cutoff) { discard; }")
    if premultiply:
        f.append("    u_xlat16_0.xyz = u_xlat16_0.xyz * u_xlat16_0.www;")
    f += ["    SV_Target0 = u_xlat16_0;", "    return;", "}", "", "#endif"]

    return "\n".join(v) + "\n" + "\n".join(f) + "\n"
