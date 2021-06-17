all: folders server client
	@echo "Bundle Finished"

server: bin/aurrasd
	@echo "Server Finished"

client: bin/aurras
	@echo "Client Finished"

bin/aurrasd: obj/aurrasd.o obj/buffer_manager.o obj/errors.o
	gcc -g obj/aurrasd.o obj/buffer_manager.o obj/errors.o -o bin/aurrasd

obj/aurrasd.o: src/aurrasd.c
	gcc -Wall -g -c src/aurrasd.c -o obj/aurrasd.o

bin/aurras: obj/aurras.o obj/buffer_manager.o obj/errors.o
	gcc -g obj/aurras.o obj/buffer_manager.o obj/errors.o -o bin/aurras

obj/aurras.o: src/aurras.c
	gcc -Wall -g -c src/aurras.c -o obj/aurras.o

obj/buffer_manager.o: src/buffer_manager.c
	gcc -Wall -g -c src/buffer_manager.c -o obj/buffer_manager.o

obj/errors.o: src/errors.c
	gcc -Wall -g -c src/errors.c -o obj/errors.o

folders:
	@mkdir -p tmp
	@mkdir -p obj

clean:
	@rm -f obj/* tmp/* bin/aurras bin/aurrasd
	@echo "Cleaning Complete"

test:
	bin/aurras
	bin/aurras status
	bin/aurras transform samples/sample-1-so.m4a output.mp3 alto eco rapido
