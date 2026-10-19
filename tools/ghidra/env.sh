# Sourced by the tools/ghidra scripts. Everything can be overridden from the environment:
#   GHIDRA_INSTALL_DIR  Ghidra 12.1.4 (default ~/Developer/tools/ghidra_12.1.4_PUBLIC)
#   JAVA_HOME           JDK 21 (default: Homebrew openjdk@21, then /usr/libexec/java_home -v 21)
#   SH2_EXE             the Director's Cut exe (default "Silent Hill 2/sh2pc.exe" in the repo)
#   SH2_GHPROJ          folder holding the saved project sh2dc (default ~/ghproj)
#   SH2_REFSRC          reference checkouts from fetch_refs.sh (default ~/Developer/refsrc)
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
K=$ROOT/sh2-3ds-port-kit
GHIDRA_INSTALL_DIR=${GHIDRA_INSTALL_DIR:-$HOME/Developer/tools/ghidra_12.1.4_PUBLIC}
if [ -z "$JAVA_HOME" ]; then
	if [ -d /opt/homebrew/opt/openjdk@21/libexec/openjdk.jdk/Contents/Home ]; then
		JAVA_HOME=/opt/homebrew/opt/openjdk@21/libexec/openjdk.jdk/Contents/Home
	else
		JAVA_HOME=$(/usr/libexec/java_home -v 21 2>/dev/null)
	fi
fi
export GHIDRA_INSTALL_DIR JAVA_HOME
SH2_EXE=${SH2_EXE:-$ROOT/Silent Hill 2/sh2pc.exe}
SH2_GHPROJ=${SH2_GHPROJ:-$HOME/ghproj}
SH2_REFSRC=${SH2_REFSRC:-$HOME/Developer/refsrc}
PS2_SRC="$SH2_REFSRC/Silent-Hill/Debug - dwarf2cpp/Silent Hill 2 (USA) (En,Ja) (Beta)/SLUS_202_28/work/sh2(CVS全取得)/src"
HEADLESS=$GHIDRA_INSTALL_DIR/support/analyzeHeadless
[ -x "$HEADLESS" ] || { echo "Ghidra not found at $GHIDRA_INSTALL_DIR (set GHIDRA_INSTALL_DIR)" >&2; exit 2; }

# Runs a script set against the saved project without changing it; prints only the scripts' output.
gh_read() {
	[ -d "$SH2_GHPROJ/sh2dc.rep" ] || { echo "No Ghidra project at $SH2_GHPROJ/sh2dc (run tools/ghidra/rebuild.sh)" >&2; exit 2; }
	"$HEADLESS" "$SH2_GHPROJ" sh2dc -process sh2pc.exe -noanalysis -readOnly -scriptPath "$K/ghidra_scripts" "$@" 2>&1 \
		| grep -E '\.java> |ERROR|locked' | sed -E 's/^INFO +[A-Za-z]+\.java> //; s/ \(GhidraScript\) *$//'
}
