include config.mk

SRC = aiv.c
OBJ = ${SRC:.c=.o}

.c.o:
	@echo CC -c $<
	@${CC} -c ${CFLAGS} $<

aiv: ${OBJ}
	@echo CC -o $@
	@${CC} -o $@ ${OBJ} ${LDFLAGS}

clean:
	rm -f aiv ${OBJ}
