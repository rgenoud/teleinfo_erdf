#!/bin/sh

# quick and dirty script to transform CSV into SQL script


fields='`TIMESTAMP`, `ADCO`, `OPTARIF`, `ISOUSC`, `HCHC`, `HCHP`, `PTEC`, `IINST`, `IMAX`, `PAPP`, `HHPHC`, `MOTDETAT`'
table="edf_data"

if [ $# -ne 1 ]; then
	usage
	exit 1
fi

if [ ! -e "$1" ]; then
	usage
	exit 1
fi

while read line ; do
	printf 'INSERT INTO `%s` (%s) VALUES (%s);\n' "$table" "$fields" "$line"
done < "$1"
