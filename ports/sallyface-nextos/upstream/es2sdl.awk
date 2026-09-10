# es_input.cfg -> linha SDL_GAMECONTROLLERCONFIG. Compativel com busybox awk
# (sem funcoes, sem delete de array inteiro).
BEGIN { inblk = 0 }
/<inputConfig/ {
  inblk = 1; guid = ""; name = "pad"; keys = ""
  if (match($0, /deviceGUID="[^"]*"/)) guid = substr($0, RSTART+12, RLENGTH-13)
  if (match($0, /deviceName="[^"]*"/)) {
    name = substr($0, RSTART+12, RLENGTH-13)
    gsub(/^[ \t]+/, "", name); gsub(/[ \t]+$/, "", name); gsub(/,/, " ", name)
  }
  next
}
inblk && /<input / {
  if (!match($0, /name="[^"]*"/))  next; nm = substr($0, RSTART+6, RLENGTH-7)
  if (!match($0, /type="[^"]*"/))  next; ty = substr($0, RSTART+6, RLENGTH-7)
  if (!match($0, /id="[^"]*"/))    next; id = substr($0, RSTART+4, RLENGTH-5)
  if (!match($0, /value="[^"]*"/)) next; vl = substr($0, RSTART+7, RLENGTH-8)
  k = nm
  # EmulationStation nomeia a face no padrao Nintendo/retro:
  #   A=leste, B=sul, X=norte, Y=oeste.
  # SDL_GameController com USE_BUTTON_LABELS=0 espera posicoes Xbox:
  #   A=sul, B=leste, X=oeste, Y=norte.
  # Portanto os nomes precisam ser trocados aqui; o id fisico continua sendo
  # exatamente o que o usuario configurou no proprio EmulationStation.
  if      (nm == "a") k = "b"
  else if (nm == "b") k = "a"
  else if (nm == "x") k = "y"
  else if (nm == "y") k = "x"
  else if (nm == "select") k = "back"
  else if (nm == "leftthumb")  k = "leftstick"
  else if (nm == "rightthumb") k = "rightstick"
  else if (nm == "up")    k = "dpup"
  else if (nm == "down")  k = "dpdown"
  else if (nm == "left")  k = "dpleft"
  else if (nm == "right") k = "dpright"
  else if (nm == "leftanalogleft"  || nm == "leftanalogright")  k = "leftx"
  else if (nm == "leftanalogup"    || nm == "leftanalogdown")   k = "lefty"
  else if (nm == "rightanalogleft" || nm == "rightanalogright") k = "rightx"
  else if (nm == "rightanalogup"   || nm == "rightanalogdown")  k = "righty"
  else if (nm == "hotkeyenable" || nm == "hotkey") next
  if (ty == "button")   v = "b" id
  else if (ty == "hat") v = "h" id "." vl
  else if (ty == "axis") {
    v = "a" id
    # SDL espera esquerda/cima negativos e direita/baixo positivos. Alguns
    # pads (inclusive o JHA medido no NextOS) publicam um eixo invertido; o
    # value do es_input.cfg e' a prova, e `~` e' a inversao canonica do mapping
    # SDL. Sem isto o analogico direito levava o cursor para o lado oposto.
    neg = (nm == "leftanalogleft" || nm == "leftanalogup" ||
           nm == "rightanalogleft" || nm == "rightanalogup")
    pos = (nm == "leftanalogright" || nm == "leftanalogdown" ||
           nm == "rightanalogright" || nm == "rightanalogdown")
    if ((neg && (vl + 0) > 0) || (pos && (vl + 0) < 0)) v = v "~"
  }
  else next
  if (index(keys, "," k ":") == 0) keys = keys "," k ":" v
  next
}
inblk && /<\/inputConfig>/ {
  inblk = 0
  if (guid == "") next
  if (want != "" && guid != want) next
  if (wantname != "" && name != wantname) next
  if (index(keys, ",a:") == 0 || index(keys, ",start:") == 0) next
  print guid "," name keys ",platform:Linux,"
  found = 1; exit
}
END { if (!found) exit 1 }
