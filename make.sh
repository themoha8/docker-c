CFLAGS="-Wall -ansi -std=c89"

VERBOSITY=0
SRC=`ls *.c`

while [ "$1" = "-v" ]; do
	VERBOSITY=$((VERBOSITY+1))
	shift
done


run ()
{
	if [ $VERBOSITY -gt 0 ]; then
		echo "$@";
	fi

	"$@" || exit 1
}

if [ ! -d "alpine" ]; then
	mkdir alpine
	tar -xzf alpine-minirootfs-3.21.3-x86_64.tar.gz -C alpine
fi

if [ "$#" -eq  0 ]; then
	for file in $SRC; do
		if [ "$file" == "create_container.c" ]; then
			run gcc -o "${file%.c}" -g $CFLAGS "$file" "lib/netlinklib.c"
			continue
		fi
		run gcc -o "${file%.c}" -g $CFLAGS "$file"
	done
	exit 0
fi

if [ "$#" -gt  0 ]; then
	if [ "$1" == "clean" ]; then
		for file in $SRC; do
			run rm -f "${file%.c}"
			run rm -f "${file%.c}.o"
		done
	elif [ "$1" == "fmt" ]; then
		if which indent > /dev/null; then
			for file in $SRC; do
				run indent -kr -ts4 -l120 $file
			done
			rm -f *.c~
		fi
	else
		if [ "$1" == "create_container.c" ]; then
			run gcc -o "${1%.c}" -g $CFLAGS "$1" "lib/netlinklib.c"
			exit 0
		fi
		run gcc -o "${1%.c}" -g $CFLAGS "$1"
	fi
fi
