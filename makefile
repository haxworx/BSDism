
default:
	-mkdir $(HOME)/bin
	$(CC) -lm bsd_generic_sensors.c -o $(HOME)/bin/sensors
	cp tmux.conf $(HOME)/.tmux.conf
	cp volctl $(HOME)/bin
	chmod +x $(HOME)/bin/*
