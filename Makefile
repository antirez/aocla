all: aocla

SANITIZE=-fsanitize=address

aocla: aocla.c
	$(CC) -g -ggdb aocla.c -Wall -W -pedantic -O2 \
	      $(SANITIZE) -o aocla

clean:
	rm -rf aocla *.dSYM
