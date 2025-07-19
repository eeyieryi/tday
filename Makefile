default: build
	./tday

config.h:
	cp config.def.h $@

build: config.h
	cc -o tday -l sqlite3 tday.c

clean:
	rm tday tday.db
