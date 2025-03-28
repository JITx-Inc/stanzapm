#!/bin/bash
set -Eeuxo pipefail
PS4='>>> '
TOP="${PWD}"

# This script is designed to be run from a Concourse Task with the following env vars

USAGE="STANZA_CONFIG=/path $0"

# Required env var inputs
echo "     STANZA_CONFIG:" "${STANZA_CONFIG:?Usage: ${USAGE}}"          # directory where .stanza config file will be stored, as in normal stanza behavior

# Defaulted env var inputs - can override if necessary
echo "              REPODIR:" "${REPODIR:=stanzapm}"
echo "      CONAN_USER_HOME:" "${CONAN_USER_HOME:=${REPODIR}}"
echo "       CREATE_ARCHIVE:" "${CREATE_ARCHIVE:=false}"
echo "       CREATE_PACKAGE:" "${CREATE_PACKAGE:=false}"
echo "         CREATE_CONAN:" "${CREATE_CONAN:=false}"
echo "      SIGN_EXECUTABLE:" "${SIGN_EXECUTABLE:=false}"
echo "         UPLOAD_CONAN:" "${UPLOAD_CONAN:=false}"
echo "STANZA_BUILD_PLATFORM:" "${STANZA_BUILD_PLATFORM:=$(uname -s)}"  # linux|macos|windows
echo "                  VER:" "${VER:=$(git -C ${REPODIR} describe --tags --abbrev=0)}"

# special case - if STANZA_CONFIG starts with "./", then replace it with the full path
[[ ${STANZA_CONFIG::2} == "./" ]] && STANZA_CONFIG=${PWD}/${STANZA_CONFIG:2}

# Calculated env vars
STANZADIR=$(grep ^install-dir $STANZA_CONFIG/.stanza | cut -f2 -d\")

PLATFORM_DESC="unknown"
case "$STANZA_BUILD_PLATFORM" in
    Linux* | linux* | ubuntu*)
        STANZA_BUILD_PLATFORM=linux
        STANZA_PLATFORMCHAR="l"
        PLATFORM_DESC="$(grep ^ID= /etc/os-release | cut -f2 -d=)-$(grep ^VERSION_CODENAME= /etc/os-release | cut -f2 -d=)"
    ;;
    Darwin | mac* | os-x)
        STANZA_BUILD_PLATFORM=os-x
        STANZA_PLATFORMCHAR=""
        PLATFORM_DESC="macos-unknown"
        case "$(sw_vers -productVersion)" in
            13.*)
                PLATFORM_DESC="macos-ventura"
            ;;
            12.*)
                PLATFORM_DESC="macos-monterey"
            ;;
            11.*)
                PLATFORM_DESC="macos-bigsur"
            ;;
            10.15.*)
                PLATFORM_DESC="macos-catalina"
            ;;
            10.14.*)
                PLATFORM_DESC="macos-mojave"
            ;;
            10.13.*)
                PLATFORM_DESC="macos-highsierra"
            ;;
            10.12.*)
                PLATFORM_DESC="macos-sierra"
            ;;
        esac
    ;;
    MINGW* | win*)
        STANZA_BUILD_PLATFORM=windows
        STANZA_PLATFORMCHAR="w"
        PLATFORM_DESC="windows-unknown"
        case "$(uname -s)" in
            MINGW64*)
                PLATFORM_DESC="windows-mingw64"
            ;;
        esac
    ;;
    *)
        printf "\n\n*** ERROR: unknown build platform \"${STANZA_BUILD_PLATFORM}\"\n\n\n" && exit -2
    ;;
esac


cd "${REPODIR}"
echo "Building stanzapm version ${VER} in ${PWD}"

mkdir -p build
mkdir -p bin

# Compile asmjit.a so that bootstrap compiler can use an
# up-to-date version.
case "$STANZA_BUILD_PLATFORM" in
    os-x)
      scripts/make-asmjit.sh os-x
      cp bin/libasmjit-os-x.a bin/libasmjit.a
    ;;
    linux)
      scripts/make-asmjit.sh linux
      cp bin/libasmjit-linux.a bin/libasmjit.a
    ;;
    windows)
      scripts/make-asmjit.bat
      cp bin/libasmjit-windows.a bin/libasmjit.a
    ;;
    *)
      printf "Bad platform" && exit -2
    ;;
esac

# copy asmjit.a from stanza install into repository bin directory
# so it can be linked into the bootstrap compiler
# cp -a ${STANZADIR}/bin/libasmjit.a bin/libasmjit.a

# call the script to build the bootstrap compiler
ci/bootstrap-stanza.sh
# verify that expected output file exists
ls stanzatemp

scripts/make.sh ./stanzatemp ${STANZA_BUILD_PLATFORM} compile-clean-without-finish

# call ?finish sh or bat
FINISH_EXT="sh"
[[ "${STANZA_PLATFORMCHAR}" == "w" ]] && FINISH_EXT="bat"
scripts/${STANZA_PLATFORMCHAR}finish.${FINISH_EXT}


if [ "$CREATE_PACKAGE" == "true" ] ; then
  #VERU=${VER//./_}  # convert dots to underscores
  STANZA_EXT=""
  [[ "${STANZA_PLATFORMCHAR}" == "w" ]] && STANZA_EXT=".exe"

  FILES="docs \
         bin \
         include \
         ${STANZA_PLATFORMCHAR}pkgs \
         ${STANZA_PLATFORMCHAR}stanza${STANZA_EXT} \
         core \
         compiler \
         debug \
         examples \
         runtime \
         stanza.proj \
         License.txt \
         ChangeLog.txt"

  mkdir -p ziptmp/build
  cp -r ${FILES} ziptmp/

  cd ziptmp

  # rename "lstanza" to "stanza" and "wstanza.exe" to "stanza.exe"
  [[ "${STANZA_PLATFORMCHAR}stanza" != "stanza" ]] \
      && mv ${STANZA_PLATFORMCHAR}stanza${STANZA_EXT} stanza${STANZA_EXT} \
      && mv ${STANZA_PLATFORMCHAR}pkgs pkgs

  if [ "$SIGN_EXECUTABLE" == "true" ] ; then
      ../ci/sign-windows-release.bash
  fi

  #zip -r ../${STANZA_PLATFORMCHAR}stanza_${VERU}.zip *
  zip -r ../stanza-${PLATFORM_DESC}_${VER}.zip *
  cd ..

  if [ "$CREATE_CONAN" == "true" ] ; then
    cd conan
    SBP=${STANZA_BUILD_PLATFORM}
    if [ "${SBP}" == "os-x" ] ; then SBP="macos" ; fi

    make "${SBP}" VER="${VER}" STANZA_ZIP="$PWD/../stanza-${PLATFORM_DESC}_${VER}.zip"

    if [ "$UPLOAD_CONAN" == "true" ] ; then
      make upload VER="${VER}"
    fi
  fi
fi

# if [ "$CREATE_ARCHIVE" == "true" ] ; then
#   zip -r -9 -q stanza-build-${PLATFORM}-${VER}.zip \
#      .conan \
#      .stanza \
#      CMakeUserPresets.json \
#      build
# fi
