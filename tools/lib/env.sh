# Shared environment for tools/. Source it: `. "$(dirname "$0")/../lib/env.sh"`
# Defaults can be overridden from the caller's environment.
: "${REPO_ROOT:=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)}"
: "${GAME_DIR:=/home/familybadgujarwilly/Games/Heroic/rocketleague}"
: "${WORK_DIR:=$REPO_ROOT/work}"
: "${OUT_DIR:=$REPO_ROOT/out}"
: "${GH_REPO:=user99672223/rl-ios-port}"
# The owner's gh login lives under their real home (DECISIONS.md D-004).
if [ -z "${GH_CONFIG_DIR:-}" ] && [ -f /home/familybadgujarwilly/.config/gh/hosts.yml ]; then
  export GH_CONFIG_DIR=/home/familybadgujarwilly/.config/gh
fi
export REPO_ROOT GAME_DIR WORK_DIR OUT_DIR GH_REPO
