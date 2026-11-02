# Math server - run "make" in this folder.
#
#   server          the math server (project folder)
#   client/client   the client; a file cannot be called "client" here because
#                   the client/ folder already uses that name
#   matinvpar       standalone parallel matrix inversion (for testing)
#   kmeanspar       standalone parallel k-means (for testing)
#
# Extra flags, e.g. sanitizers: make EXTRA_FLAGS=-fsanitize=thread

CC          := gcc
EXTRA_FLAGS ?=
CFLAGS      := -std=gnu11 -O2 -Wall -Wextra -pthread -Imathserver/include $(EXTRA_FLAGS)
LDFLAGS     := -pthread $(EXTRA_FLAGS)
DEPFLAGS     = -MMD -MP

SRC_DIR := mathserver/src
OBJ_DIR := mathserver/objects

SERVER_SRCS := server.c server_util.c protocol.c job.c parallel.c \
               matinv-par.c kmeans-par.c strategy_fork.c mux_core.c \
               strategy_muxbasic.c strategy_muxscale.c
SERVER_OBJS := $(addprefix $(OBJ_DIR)/,$(SERVER_SRCS:.c=.o))

SERVER    := server
CLIENT    := client/client
MATINVPAR := matinvpar
KMEANSPAR := kmeanspar

all: $(SERVER) $(CLIENT) $(MATINVPAR) $(KMEANSPAR)

$(SERVER): $(SERVER_OBJS)
	$(CC) $(LDFLAGS) -o $@ $^

$(CLIENT): client/client.c $(OBJ_DIR)/protocol.o mathserver/include/protocol.h
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ client/client.c $(OBJ_DIR)/protocol.o

$(MATINVPAR): $(OBJ_DIR)/matinv-standalone.o $(OBJ_DIR)/parallel.o
	$(CC) $(LDFLAGS) -o $@ $^

$(KMEANSPAR): $(OBJ_DIR)/kmeans-standalone.o $(OBJ_DIR)/parallel.o
	$(CC) $(LDFLAGS) -o $@ $^

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c | $(OBJ_DIR)
	$(CC) $(CFLAGS) $(DEPFLAGS) -c -o $@ $<

$(OBJ_DIR)/matinv-standalone.o: $(SRC_DIR)/matinv-par.c | $(OBJ_DIR)
	$(CC) $(CFLAGS) $(DEPFLAGS) -DMATINV_STANDALONE -c -o $@ $<

$(OBJ_DIR)/kmeans-standalone.o: $(SRC_DIR)/kmeans-par.c | $(OBJ_DIR)
	$(CC) $(CFLAGS) $(DEPFLAGS) -DKMEANS_STANDALONE -c -o $@ $<

$(OBJ_DIR):
	mkdir -p $@

clean:
	rm -f $(SERVER) $(CLIENT) $(MATINVPAR) $(KMEANSPAR) $(OBJ_DIR)/*.o $(OBJ_DIR)/*.d

.PHONY: all clean

-include $(wildcard $(OBJ_DIR)/*.d)
