#docker run -it --name SO   -v "${PWD}:/work"   -w /work   --shm-size=512m   agodio/itba-so-multi-platform:3.0 bash
#!/bin/bash

CONTAINER_NAME="SO"
IMAGE_NAME="agodio/itba-so-multi-platform:3.0"

# Crear el container (solo si no existe)
docker create -it --name $CONTAINER_NAME -v "${PWD}:/work" -w /work --shm-size=512m $IMAGE_NAME bash

# Iniciar el container
docker start -ai $CONTAINER_NAME

# Destruir el container
docker rm $CONTAINER_NAME