function flush_block(    i, text) {
  text = ""
  for (i = 1; i <= block_n; i++)
    text = text block[i] "\n"

  if (text ~ /renpy\/pygame\/rwobject\.pyx/ &&
      text ~ /(I\/O operation on closed file|expected bytes, str found)/) {
    block_n = 0
    delete block
    in_block = 0
    return
  }

  printf "%s", text
  block_n = 0
  delete block
  in_block = 0
}

{
  if (ENVIRON["SUMMERTIME_KEEP_TRACEBACK_LOG"] != "1") {
    if ($0 ~ /Traceback \(most recent call last\):/)
      next

    if ($0 ~ /During handling of the above exception/)
      next

    if ($0 ~ /Exception ignored in:/)
      next

    if ($0 ~ /(renpy\/pygame\/rwobject\.pyx|renpy\.pygame\.rwobject)/)
      next

    if ($0 ~ /(I\/O operation on closed file|expected bytes, str found)/)
      next

    if ($0 ~ /^'?renpy\.pygame\.rwobject\.python_(read|seek)'?$/)
      next

    if ($0 ~ /^(TypeError|ValueError):?/)
      next

    if ($0 ~ /(msg = <char \*> e|f\.seek\(seek, whence\)|data = f\.read\(size \* maxnum\))/)
      next
  }

  if ($0 == "") {
    if (blank_line)
      next
    blank_line = 1
    print
    next
  }

  blank_line = 0

  if (in_block) {
    if ($0 ~ /^Traceback \(most recent call last\):$/ && block_n > 0)
      flush_block()

    in_block = 1
    block[++block_n] = $0

    if ($0 == "")
      flush_block()
    next
  }

  if ($0 ~ /renpy\/pygame\/rwobject\.pyx/)
    next

  if ($0 ~ /(I\/O operation on closed file|expected bytes, str found)/)
    next

  if ($0 == "During handling of the above exception, another exception occurred:")
    next

  if ($0 ~ /Exception ignored in:.*renpy\.pygame\.rwobject\.python_(read|seek)/)
    next

  if ($0 ~ /^    (msg = <char \*> e|f\.seek\(seek, whence\)|data = f\.read\(size \* maxnum\))$/)
    next

  if ($0 ~ /^Traceback \(most recent call last\):$/) {
    in_block = 1
    block_n = 1
    block[1] = $0
    next
  }

  print
}

END {
  if (in_block)
    flush_block()
}
