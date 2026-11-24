# winres.sh - sourced by the shell builds (tools/package_windows.sh, launcher/native/build.sh). It fills in
# tools/winres/ucmr.rc.in (version information from tools/VERSION, the manifest tools/winres/ucmr.manifest and an
# optional icon) and compiles it with windres into a COFF object to link into the program or DLL:
#   winres <object.o> <file name> <file description> [<icon id> <icon file>]
# A file name ending in .dll gets the DLL's version type and manifest id (2); any other is a program (manifest id 1).

winres_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)

winres_abs() {   # a file's absolute path in Windows form with forward slashes (C:/...), as the .rc needs it
    cygpath -m "$(cd -- "$(dirname -- "$1")" && pwd)/$(basename -- "$1")"
}

winres() {
    local obj=$1 file=$2 description=$3 version type=VFT_APP manifest_id=1 icon_line=''
    version=$(tr -d '\r\n' < "$winres_dir/../VERSION")
    [[ $version =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || { echo "tools/VERSION is not x.y.z: $version" >&2; return 1; }
    [[ $file == *.dll ]] && { type=VFT_DLL; manifest_id=2; }
    [[ -n ${4:-} ]] && icon_line="$4 ICON \"$(winres_abs "$5")\""
    sed -e "s|@UCMR_ICON_LINE@|$icon_line|" \
        -e "s|@UCMR_MANIFEST_LINE@|$manifest_id 24 \"$(winres_abs "$winres_dir/ucmr.manifest")\"|" \
        -e "s|@UCMR_VERSION_COMMA@|${version//./,},0|g" -e "s|@UCMR_VERSION@|$version|g" \
        -e "s|@UCMR_FILETYPE@|$type|" -e "s|@UCMR_DESCRIPTION@|$description|" \
        -e "s|@UCMR_COPYRIGHT@|GNU GPL v2 or later|" -e "s|@UCMR_INTERNAL_NAME@|${file%.*}|" -e "s|@UCMR_FILENAME@|$file|" \
        "$winres_dir/ucmr.rc.in" > "${obj%.o}.rc"
    windres "${obj%.o}.rc" -O coff -o "$obj"
}
