
CC = g++
OUT = app

all:
	${CC} test.cpp -o ${OUT}

clean:
	rm ${OUT} -rf
