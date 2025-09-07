# UDPBD Server

By Rick Gaiser

windows version by Alex Parrado

brought to github by El_isra

## How to compile with MSYS2 for windows

Download and install [MSYS2](https://www.msys2.org/).  
Open a MSYS2 UCRT64 terminal.  

```bash
pacman -Syuu
pacman --needed -S git make mingw-w64-ucrt-x86_64-gcc

git clone -b windows https://github.com/israpps/udpbd-server.git
cd ./udpbd-server/
make
```
Upon success udpbd-server.exe will be compiled.  

