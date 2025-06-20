
#export STANZA_BUILD_PLATFORM={linux|macos|windows}
export STANZA_BUILD_PLATFORM=linux
export STANZA_BUILD_VER=$(grep -v '^#' ci/build-stanza-version.txt | head -1)
export STANZA_CONFIG=./
export STANZA_INSTALL_DIR=./stanzai
export CREATE_PACKAGE=true
export REPODIR=.

# execute all lines of a target in one shell
.ONESHELL:

all: build

stanzainstall: ${STANZA_CONFIG}/.stanza

${STANZA_CONFIG}/.stanza:
	ci/install-stanza.sh

build: ${STANZA_CONFIG}/.stanza
	ci/build-stanza.sh

clean:
	${STANZA_INSTALL_DIR}/stanza clean
	rm -rf build/ bin/ bootstrap.macros lpkgs/ ?stanza ?stanza.s stanza stanza.s stanzatemp

stanzauninstall:
	rm -rf ${STANZA_CONFIG}/.stanza ${STANZA_INSTALL_DIR} stanza.??????????/
