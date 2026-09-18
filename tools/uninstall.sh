#!/bin/sh
# Remove the Anti that tools/install.sh put under ~/.anti.
#
#   curl -fsSL https://anti-lang.com/uninstall.sh | sh
#   curl -fsSL https://anti-lang.com/uninstall.sh | sh -s -- --intel  # docs-style:ignore
#
# It deletes that one directory and nothing else. The line that the
# installer printed for the shell profile is the user's to remove, and the
# script names the files that hold it.
#
# The option --arm or --intel removes the install of that processor, which
# stands in $HOME/.anti-<cpu> when it is not the one of this machine.
# ANTI_ARCH holds the same choice as arm64 or x86_64.
#
#   ANTI_ARCH       arm64 or x86_64, default the processor of this machine
#   ANTI_HOME       what to remove, default $HOME/.anti
#   ANTI_REMOVE     yes or no to removing the directory
set -eu

remove=${ANTI_REMOVE:-}
arch=${ANTI_ARCH:-}

for argument in "$@"; do
    case $argument in
    --arm | --arm64) arch=arm64 ;;
    --intel | --x86_64 | --x64) arch=x86_64 ;;
    *)
        echo "anti: unknown option $argument" >&2
        echo "usage: uninstall.sh [--arm | --intel]" >&2
        exit 2
        ;;
    esac
done

case "$(uname -m)" in
arm64 | aarch64) native=arm64 ;;
x86_64) native=x86_64 ;;
*) native="" ;;
esac
if [ -z "$arch" ] || [ "$arch" = "$native" ]; then
    home=${ANTI_HOME:-$HOME/.anti}
else
    home=${ANTI_HOME:-$HOME/.anti-$arch}
fi

say() {
    echo "anti: $*"
}

if [ ! -e "$home" ]; then
    say "$home does not exist, so nothing is installed there"
    exit 0
fi

size=$(du -sh "$home" 2>/dev/null | cut -f 1)
say "$home holds $size"
case $remove in
y | Y | yes | Yes) ;;
n | N | no | No) say "nothing removed"; exit 0 ;;
*)
    if { exec 3</dev/tty; } 2>/dev/null; then
        printf 'Remove %s? [Y/n] ' "$home" >&2
        answer=""
        read -r answer <&3 || true
        exec 3<&-
        case $answer in
        n | N | no | No) say "nothing removed"; exit 0 ;;
        esac
    fi
    ;;
esac

rm -rf "$home"
say "removed $home"

# The installer marks its own line with a comment, so only that line
# goes. A line that the user wrote is named rather than edited.
for profile in "$HOME/.zshrc" "$HOME/.bashrc" "$HOME/.bash_profile" \
               "$HOME/.profile"; do
    if [ ! -f "$profile" ]; then
        continue
    fi
    if grep -q "# anti$" "$profile"; then
        grep -v "# anti$" "$profile" > "$profile.anti-new" &&
            mv "$profile.anti-new" "$profile"
        say "removed the line for Anti from $profile"
    elif grep -q "\.anti/bin" "$profile"; then
        say "a line of your own names Anti in $profile"
    fi
done
