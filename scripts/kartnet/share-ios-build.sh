#!/bin/bash
# Builds the iPhone app with a unique build number and puts it in the user's
# iCloud Drive "N64 builds" folder, where the auto-install job (and
# "Install on iPhone.command") installs it onto the phones.
# NEVER put these zips anywhere public: the app contains ROM-derived data.
#
#   share-ios-build.sh            build, then share
#   NOBUILD=1 share-ios-build.sh  share the existing build (it must be stamped)
set -euo pipefail
export PATH=/opt/homebrew/bin:$PATH
REPO=$(cd "$(dirname "$0")/../.." && pwd)
DEST="$HOME/Library/Mobile Documents/com~apple~CloudDocs/Documents/Software Development/N64/N64 builds"
TEAM=${TEAM:-4X24G557F9}
APP="$REPO/build-ios/Release-iphoneos/Spaghettify.app"
KEEP=2

[ -d "$DEST" ] || { echo "No builds folder at $DEST"; exit 1; }
cd "$REPO"
if [ -z "${NOBUILD:-}" ]; then
    STAMP=$(date +%Y%m%d.%H%M)
    LOG=$(mktemp -t spaghettify-build)
    echo "Building (bundle version $STAMP), log $LOG"
    ok=0
    for i in 1 2; do # the first build after a CMake change can link with the old file list
        if xcodebuild -project build-ios/Spaghettify.xcodeproj -scheme Spaghettify -configuration Release \
            -destination 'generic/platform=iOS' DEVELOPMENT_TEAM="$TEAM" CODE_SIGN_STYLE=Automatic \
            -allowProvisioningUpdates CURRENT_PROJECT_VERSION="$STAMP" build > "$LOG" 2>&1; then
            ok=1; break
        fi
    done
    [ $ok = 1 ] || { grep -E "error:" "$LOG" | head; echo "BUILD FAILED"; exit 1; }
fi

version=$(plutil -extract CFBundleVersion raw -o - "$APP/Info.plist")
[[ "$version" =~ ^[0-9]{8}\.[0-9]{4}$ ]] || { echo "Build not stamped (CFBundleVersion '$version')"; exit 1; }
codesign --verify --deep --strict "$APP" || { echo "App not signed"; exit 1; }
commit=$(git rev-parse --short=9 HEAD)
[ -n "$(git status --porcelain -- src CMakeLists.txt n64net libultraship | grep -v include/assets)" ] && commit="$commit+dirty"
id=$(sed -n 's/.*KARTNET_BUILD_ID "\(.*\)".*/\1/p' build-ios/generated/kartnet_build_id.h)
name="Spaghettify-${version/./-}-$commit-$id.zip"

tmp=$(mktemp -d)
ditto -c -k --keepParent "$APP" "$tmp/$name"
mv "$tmp/$name" "$DEST/$name"
rmdir "$tmp"
echo "Shared: $name"

# Keep the newest $KEEP Mario Kart zips.
ls -1t "$DEST"/Spaghettify-*.zip | tail -n +$((KEEP + 1)) | while read -r old; do
    rm -f "$old" && echo "Removed old: $(basename "$old")"
done
