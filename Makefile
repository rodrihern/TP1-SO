#docker run -it --rm   --name SO   -v "${PWD}:/work"   -w /work   --shm-size=512m   agodio/itba-so-multi-platform:3.0 bash
#gcc -Wall -g -Iinclude -pthread -c src/shm.c -o src/shm.o
#gcc -Wall -g -Iinclude -pthread src/master.c src/shm.o -o master -lrt -pthread
#gcc -Wall -g -Iinclude -pthread src/player.c src/shm.o -o player -lrt -pthread
#gcc -Wall -g -Iinclude -pthread src/view.c src/shm.o -o view -lrt -pthread
#./master -v ./view -p ./player ./player