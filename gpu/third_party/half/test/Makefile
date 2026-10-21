CC       = /usr/bin/g++
CFLAGS	 = -Wall -pedantic -g -O2
C11FLAGS = -std=c++0x $(CFLAGS)
CPPFLAGS = -I../include
LDFLAGS  = 

test11: src/test11.cpp ../include/half.hpp
	mkdir -p bin
	$(CC) $(C11FLAGS) $(CPPFLAGS) $(LDFLAGS) -o bin/test src/test11.cpp

test03: src/test03.cpp ../include/half.hpp
	mkdir -p bin
	$(CC) $(CFLAGS) $(CPPFLAGS) $(LDFLAGS) -o bin/test src/test03.cpp
	
asm11: src/test11.cpp ../include/half.hpp
	mkdir -p asm
	$(CC) $(C11FLAGS) $(CPPFLAGS) $(LDFLAGS) -S -o asm/test11.asm src/test11.cpp
	
asm03: src/test11.cpp ../include/half.hpp
	mkdir -p asm
	$(CC) $(CFLAGS) $(CPPFLAGS) $(LDFLAGS) -S -o asm/test03.asm src/test03.cpp

clean:
	rm -rvf *~ src/*~ asm bin
