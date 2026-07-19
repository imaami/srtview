#!/usr/bin/bash

set +eu

d=$(dirname "$0") && d=$(realpath -eq "$d") &&
[[ -d "$d/src" && -f "$d/CMakeLists.txt" ]] || exit $?

[[ ! "$NO_STRIP" =~ ^(1|[Oo][Nn]|[Tt][Rr][Uu][Ee]|[Yy]([Ee][Ss])?)$ ]]
declare -i __no_strip=$?

while (( $# )); do
	case "$1" in
	--no-strip) declare -i "${1//-/_}"=1 ;;
	esac
	shift
done

__arch=$(arch 2>/dev/null || lscpu --json 2>/dev/null | jq -r \
         '.lscpu[] | select(.field == "Architecture:") .data' 2>/dev/null)

__build_type="$BUILD_TYPE"
[[ "$__build_type" ]] || case "$__arch" in
aarch64)
	__build_type=MinSizeRel ;;
*)
	__build_type=Release ;;
esac

[[ "$__arch" == 'aarch64' ]] && __mcpu=cpu || __mcpu=arch

__cc="$CC"
__cc_is_clang=0
if [[ "$__cc" ]]; then
	[[ ! "$("$__cc" --version 2>&1)" =~ [Cc]lang ]] || __cc_is_clang=1
else
	case "$__arch" in
	aarch64)
		__cc='clang'
		__cc_is_clang=1 ;;
	*)
		__cc='gcc' ;;
	esac
fi

__cflags="$CFLAGS"
[[ "$__cflags" =~ (^|[[:blank:]])-m(arch|cpu|tune)= ]] || {
	if (( __cc_is_clang )); then
		__cflags+=\ $("$__cc" -### -c -xc /dev/null                 \
		                      -m{arch,cpu,tune}=native 2>&1         |
		              grep -Eo '"-t(arget|une)-cpu" "[^"]+"'        |
		              sed -Ee 's/"-target-cpu" "([^"]+)"/-m'"$__mcpu"'=\1/' \
		                   -e 's/"-tune-cpu" "([^"]+)"/-mtune=\1/'  |
		              grep -v '=$'                                  |
		              xargs)
	else
		__cflags+=\ $(COLUMNS=4096                     \
		              "$__cc" -Q --help=target         \
		                      -m{arch,cpu,tune}=native |
		              tr -d ' \t'                      |
		              grep -E '^-m(arch|cpu|tune)=.+$' |
		              xargs)
	fi

	[[ "$__cflags" =~ (^|[[:blank:]])-m(arch|cpu|tune)= ]] ||
		__cflags+=' -march=native -mtune=native'

	__cflags="${__cflags# }"
}

__cxx="$CXX"
__cxx_is_clang=0
if [[ "$__cxx" ]]; then
	[[ ! "$("$__cxx" --version 2>&1)" =~ [Cc]lang ]] || __cxx_is_clang=1
else
	case "$__arch" in
	aarch64)
		__cxx='clang++'
		__cxx_is_clang=1 ;;
	*)
		__cxx='g++' ;;
	esac
fi

__cxxflags="$CXXFLAGS"
[[ "$__cxxflags" =~ (^|[[:blank:]])-m(arch|cpu|tune)= ]] || {
	if (( __cxx_is_clang )); then
		__cxxflags+=\ $("$__cxx" -### -c -xc++ /dev/null              \
		                         -m{arch,cpu,tune}=native 2>&1        |
		                grep -Eo '"-t(arget|une)-cpu" "[^"]+"'        |
		                sed -Ee 's/"-target-cpu" "([^"]+)"/-m'"$__mcpu"'=\1/' \
		                     -e 's/"-tune-cpu" "([^"]+)"/-mtune=\1/'  |
		                grep -v '=$'                                  |
		                xargs)
	else
		__cxxflags+=\ $(COLUMNS=4096                      \
		                "$__cxx" -Q --help=target         \
		                         -m{arch,cpu,tune}=native |
		                tr -d ' \t'                       |
		                grep -E '^-m(arch|cpu|tune)=.+$'  |
		                xargs)
	fi

	[[ "$__cxxflags" =~ (^|[[:blank:]])-m(arch|cpu|tune)= ]] ||
		__cxxflags+=' -march=native -mtune=native'

	__cxxflags="${__cxxflags# }"
}

__ncpu=$(nproc)
(( __ncpu > 0 )) || __ncpu=2

rm -f -r "$d/build/"                        &&
mkdir -p "$d/build"                         &&
cmake -B "$d/build" -S "$d"                 \
      -DCMAKE_BUILD_TYPE="$__build_type"    \
      -DCMAKE_C_COMPILER="$__cc"            \
      -DCMAKE_C_FLAGS="$__cflags"           \
      -DCMAKE_CXX_COMPILER="$__cxx"         \
      -DCMAKE_CXX_FLAGS="$__cxxflags"       \
      -DCMAKE_EXPORT_COMPILE_COMMANDS=1     \
      -DCMAKE_INSTALL_PREFIX="$HOME/.local" \
      -DCMAKE_VERBOSE_MAKEFILE=1            &&
cmake --build "$d/build" -j "$__ncpu"       &&
{
	(( __no_strip )) ||
	strip --strip-all "$d/build/srtview";
}
