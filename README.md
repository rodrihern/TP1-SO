# TP1 – ChompChamps

Este proyecto implementa un juego multi–proceso con memoria compartida y semáforos POSIX:
- master: orquesta el juego y la sincronización
- player: decide movimientos y los envía al master por pipe
- view: muestra el tablero con ncurses

A continuación se detalla cómo compilar y ejecutar dentro del contenedor Docker provisto por la cátedra y cómo usar el binario con sus parámetros.

## Requisitos
- Docker instalado
- Imagen de la cátedra (multi–arquitectura):

```bash
# descarga la imagen
docker pull agodio/itba-so-multi-platform:3.0
```

## Ejecutar en Docker
Recomendado: montar el workspace actual y forzar la plataforma linux/amd64 para compatibilidad con los binarios y librerías del entorno de la cátedra.

```bash
# desde la raíz del repo (donde está el makefile)
docker run -v ${PWD}:/root \
	--privileged --platform linux/amd64 -ti \
	--name SO agodio/itba-so-multi-platform:3.0
```


 El flag `-v ${PWD}:/root` monta el directorio actual dentro del contenedor (en `/root`).

### Instalar dependencias dentro del contenedor
La imagen no trae `ncurses` por defecto. Antes de compilar, instalar:

```bash
apt-get update && apt-get install -y libncurses-dev
```

## Compilación
Dentro del contenedor (prompt `root@...:/root#`), compilar con make:

```bash
make
```

Esto genera los binarios en `bin/`:
- `bin/master`
- `bin/player`
- `bin/view`

Atajos disponibles en el makefile:
- `make run` ejecuta un ejemplo rápido con 2 jugadores y la vista.
- `make clean` limpia binarios y objetos.

## Script de ejecución rápida
Hay un script `./run.sh` que ejecuta el juego con N jugadores idénticos:

```bash
# N es opcional (por defecto 2). Debe estar entre 1 y 9.
./run.sh 4
```

Internamente invoca:
```
./bin/master -v ./bin/view -p ./bin/player ./bin/player ...
```

## Uso del programa (parámetros)
El binario principal es `./bin/master`. Parámetros admitidos:

- `-w <width>`: ancho del tablero (por defecto 10, mínimo 10, máximo 100)
- `-h <height>`: alto del tablero (por defecto 10, mínimo 10, máximo 100)
- `-d <ms>`: delay en milisegundos entre impresiones de la vista (por defecto 200)
- `-t <s>`: timeout global en segundos sin movimientos válidos (por defecto 100)
- `-s <seed>`: semilla del RNG para reproducibilidad (por defecto time(NULL))
- `-v <view_bin>`: ruta al ejecutable de la vista (opcional)
- `-p <player1> [player2 ...]`: lista de ejecutables de jugadores (1..9)

Ejemplos:

```bash
# 2 jugadores y vista ncurses por defecto del repo
./bin/master -w 20 -h 12 -d 150 -t 60 -s 12345 \
	-v ./bin/view -p ./bin/player ./bin/player

# 5 jugadores, sin vista (modo headless)
./bin/master -w 30 -h 20 -t 120 -p \
	./bin/player ./bin/player ./bin/player ./bin/player ./bin/player
```

## Estructura del repo
```
include/
	common.h, reader_sync.h, writer_sync.h, shm.h
src/
	master.c, player.c, view.c, shm.c
bin/
	master, player, view (generados por make)
run.sh
makefile
```