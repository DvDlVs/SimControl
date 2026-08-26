CC      ?= gcc
CFLAGS  ?= -std=c11 -O2 -Wall -Wextra -D_GNU_SOURCE -Iinclude
LDFLAGS ?= -lm -lrt

SRC = src/main.c src/sc_math.c src/sc_steer.c src/sc_config.c src/sc_input.c src/sc_ffb.c src/sc_telem_ams2.c src/sc_telem_games.c src/sc_ipc.c
OBJ = $(SRC:.c=.o)

.PHONY: all clean config

all: simcontrol simcontrol-shm

config: simcontrol
	python3 ui/simcontrol_config.py

simcontrol: $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(LDFLAGS)

simcontrol-shm: src/sc_shm_stub.c include/pcars2data.h
	$(CC) $(CFLAGS) -o $@ src/sc_shm_stub.c $(LDFLAGS)


src/%.o: src/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJ) simcontrol simcontrol-shm
