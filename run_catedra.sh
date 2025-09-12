#!/usr/bin/env bash
set -euo pipefail

usage() {
	echo "Uso: $0 [N]" >&2
	echo "  N: cantidad de jugadores (1..9). Si se omite, usa 2 jugadores por defecto." >&2
}

# Construir la lista de jugadores
players=("./bin/player" "./bin/player")

if [[ $# -gt 1 ]]; then
	usage; exit 1
fi

if [[ $# -eq 1 ]]; then
	arg="$1"
	if [[ "$arg" =~ ^[0-9]+$ ]] && (( arg >= 1 && arg <= 9 )); then
		players=()
		for ((i=0; i<arg; i++)); do
			players+=("./bin/player")
		done
	else
		echo "Error: N debe ser un número entre 1 y 9" >&2
		usage; exit 1
	fi
fi

exec ./master_catedra -v ./bin/view -p "${players[@]}"