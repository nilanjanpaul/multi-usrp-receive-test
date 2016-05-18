all : rx_multi_receive

rx_multi_receive : main.cpp CTimer.cpp CTimer.h
	g++ -c main.cpp
	g++ -c CTimer.cpp
	g++ -o rx_multi_receive main.o CTimer.o -L/usr/local/lib -lboost_program_options -lboost_system -lboost_thread -luhd

clean :
	rm rx_multi_receive *.o
