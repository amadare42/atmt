# embed_file.cmake - writes a binary file as a C array (cmake -DIN=... -DOUT=... -DNAME=... -P).
# The window's font is compiled into the app this way, so nothing has to sit next to it.
file(READ "${IN}" hex HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${hex}")
file(WRITE "${OUT}" "// generated from ${IN} by manager/cmake/embed_file.cmake - do not edit\n#pragma once\nstatic const unsigned char ${NAME}[] = {\n${bytes}\n};\n")
