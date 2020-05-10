# paths
PREFIX = /usr/local

# includes and libs
LIBS = -lxcb -lxcb-errors -lavcodec -lavformat -lavutil -lswscale

# flags
CFLAGS = -Wall -Werror -Wextra -Wno-unused-parameter -g
LDFLAGS = ${LIBS}

# compiler and linker
CC = cc
