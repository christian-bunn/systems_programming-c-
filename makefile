# Makefile

CC = gcc
CFLAGS = -Wall -Wextra -pthread -I./headers

# Source files
SRCS = src/call.c src/car.c src/controller.c src/internal.c src/safety.c src/network.c src/shared_memory.c src/utils.c

# Object files
OBJS = $(SRCS:.c=.o)

# Executables
BINARIES = car controller call internal safety

all: $(BINARIES)

car: src/car.o src/shared_memory.o src/network.o src/utils.o
	$(CC) $(CFLAGS) -o car src/car.o src/shared_memory.o src/network.o src/utils.o -lpthread

controller: src/controller.o src/network.o src/utils.o
	$(CC) $(CFLAGS) -o controller src/controller.o src/network.o src/utils.o -lpthread

call: src/call.o src/network.o src/utils.o
	$(CC) $(CFLAGS) -o call src/call.o src/network.o src/utils.o

internal: src/internal.o src/shared_memory.o src/utils.o
	$(CC) $(CFLAGS) -o internal src/internal.o src/shared_memory.o src/utils.o -lpthread

safety: src/safety.o src/shared_memory.o src/utils.o
	$(CC) $(CFLAGS) -o safety src/safety.o src/shared_memory.o src/utils.o -lpthread

# Compile object files
src/%.o: src/%.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(BINARIES) src/*.o
