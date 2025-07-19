default: build
	./tday

build:
	cc -o tday -l sqlite3 tday.c

clean:
	rm tday tday.db

