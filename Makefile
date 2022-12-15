all: aocla

aocla: aocla.c
	$(CC) -g -ggdb aocla.c -Wall -W -pedantic -O2 -o aocla

clean:
	rm -rf aocla *.dSYM
