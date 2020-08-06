CC		:= gcc

CPPFLAGS	:= -MMD -Wall -Wextra -Werror -DNDEBUG
CFLAGS		:= -std=gnu18 -march=native -O3 -fomit-frame-pointer -pipe
LDFLAGS		:= -Wl,-O3 -Wl,--as-needed -s
LDLIBS		:= -lreadline

PROGS		:= shell

shell: shell.o command.o lexer.o jobs.o

.PHONY: clean
clean:
	$(RM) *.o *.d $(PROGS)

# vim: ts=8 sw=8 noet
