# Sweep


build
=====

cc main.c -o sweep -lm -lglfw -lGLESv2 -g -std=c99 -pedantic -Wall -Wextra
#### on windows use clang.

web
---
emcc main.c -o sweep.js -lglfw -lGLESv2 -g -std=c99 -pedantic -Wall -Wextra -sUSE_GLFW=3 -sFULL_ES2=1 -sFULL_ES3=1

