
CC = g++
OUT = app

all:
	${CC} test.cpp -o ${OUT} 
	#-lpthread

clean:
	rm ${OUT} -rf
	clear

