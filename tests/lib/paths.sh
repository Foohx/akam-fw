# shellcheck shell=bash
# Single place for the repo-relative paths that tests use. Source it, never execute it:
#   . "$(dirname "${BASH_SOURCE[0]}")/lib/paths.sh"      (from a file directly under tests/)
# When files move (see docs/architecture reorganisation, tools/dev/path-map.txt), edit ONLY this file; tests
# reference the variables below instead of literal "platforms/anyka-ak3918", "src/net", ... paths (SHARED_INC is the -I list of the shared headers).
# Every variable is an absolute path. Callers may pre-set REPO to override the root.

: "${REPO:=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)}"

# Models
MODEL_1080P=lsc-indoor-1080p-ak3918          # model id
MODEL_2K=lsc-rotatable-2k-ak3918av130
PLATFORM_2K=$REPO/platforms/anyka-ak3918av130
FW_2K=$REPO/firmware/$MODEL_2K
FW_1080P=$REPO/firmware/$MODEL_1080P

# Sources: 1080p program and shared code
SRC_DIR=$REPO/src
SRC_1080P=$REPO/platforms/anyka-ak3918      # the platform of the 1080p (anyka-ak3918)
SRC_CORE=$SRC_DIR/core
SRC_NET=$SRC_DIR/net
SRC_API=$SRC_DIR/api
SRC_FEATURES=$SRC_DIR/features
SRC_HAL=$SRC_DIR/hal
SRC_SMOLRTSP=$REPO/third_party/smolrtsp      # fetched by bootstrap, git-ignored
# Include path of the shared headers (they are included by basename): every directory of src/ that holds headers.
SHARED_INC=()
for d in net/rtsp net/mqtt net/http net/onvif net/crypto api core core/control core/version features/audio features/button features/light \
    features/night features/ptz features/track features/scene hal; do SHARED_INC+=(-I"$SRC_DIR/$d"); done
# Include path of the 2K build: its platform (vendor glue), its firmware directory (binding) and the shared headers.
# shellcheck disable=SC2034 # used by the 2K tests that source this file
INC_2K=(-I"$REPO/platforms/anyka-ak3918av130" -I"$FW_2K" -I"$FW_2K/helpers/ptz" "${SHARED_INC[@]}")

# Sources: 2K
APP_2K=$FW_2K/app
OVERLAY_2K=$FW_2K/overlay
APPFS_2K=$FW_2K/appfs
RESCUE_2K=$FW_2K/rescue

# Tests, tools, docs, build output
TESTS_DIR=$REPO/tests
TESTS_NATIVE=$TESTS_DIR/native
TESTS_GOLDEN=$TESTS_DIR/golden
TOOLS_DIR=$REPO/tools
TOOLS_CI=$TOOLS_DIR/ci
TOOLS_ASSISTANT=$TOOLS_DIR/assistant
TOOLS_RELEASE=$TOOLS_DIR/release
DOCS_DIR=$REPO/docs
DIST_DIR=$REPO/dist
DIST_2K=$DIST_DIR/rot2k-app                 # published copy of the 2K binaries (make poc3-dist), what the card/appfs/kit builders read
BUILD_DIR=$REPO/build
BUILD_1080P=$BUILD_DIR/$MODEL_1080P          # everything the 1080p build writes (ak_rtsp, flash_tool, ...)
BUILD_2K=$BUILD_DIR/$MODEL_2K                # everything the 2K build writes (akam2k_rtsp, helpers, BUILDINFO.txt, poc3/ objects)
GOLDEN_1080P=$TESTS_GOLDEN/ak_rtsp_1080p.sha256
GOLDEN_2K=$TESTS_GOLDEN/rot2k_app.golden

export REPO MODEL_1080P MODEL_2K PLATFORM_2K FW_2K FW_1080P SRC_DIR SRC_1080P SRC_CORE SRC_NET SRC_API SRC_FEATURES SRC_HAL SRC_SMOLRTSP \
    APP_2K OVERLAY_2K APPFS_2K RESCUE_2K TESTS_DIR TESTS_NATIVE TESTS_GOLDEN TOOLS_DIR TOOLS_CI TOOLS_ASSISTANT \
    TOOLS_RELEASE DOCS_DIR DIST_DIR DIST_2K BUILD_DIR BUILD_1080P BUILD_2K GOLDEN_1080P GOLDEN_2K
