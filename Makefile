BIN=udpbd_server.dll
OBJS=main.o

$(BIN): $(OBJS)
	
	g++ -shared -static -static-libgcc -static-libstdc++ -o $@ $^ -lws2_32

$(OBJS): main.cpp
	g++ -fno-inline -Wall -c -Os $^ -o $@


all: $(BIN)

clean:
	rm -f $(BIN) $(OBJS)

install: $(BIN)
	cp $(BIN) $(PS2DEV)/bin
