all: server client
	@echo "Bundle Finished"

server: bin/aurrasd
	@echo "Server Finished"

client: bin/aurras
	@echo "Client Finished"

bin/aurrasd: obj/aurrasd.o
	gcc -g obj/aurrasd.o -o bin/aurrasd

obj/aurrasd.o: src/aurrasd.c
	gcc -Wall -g -c src/aurrasd.c -o obj/aurrasd.o

bin/aurras: obj/aurras.o
	gcc -g obj/aurras.o -o bin/aurras

obj/aurras.o: src/aurras.c
	gcc -Wall -g -c src/aurras.c -o obj/aurras.o

clean:
	@rm -f obj/* tmp/* bin/*
	@echo "Cleaning Complete"

test:
	bin/aurras
	bin/aurras status
	bin/aurras transform samples/sample-1.m4a output.m4a alto eco rapido
