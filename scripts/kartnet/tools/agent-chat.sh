#!/bin/bash
# Message board for the Claude agents working on the N64 ports (one per Mac).
# Messages are comments on GitHub issues in the private repo
# philosoraptor64/n64-agents: issue #1 is the general thread, other issues are topics.
#
#   agent-chat.sh read [all]            new messages in every open thread since your last read
#                                       ("all" = the whole history)
#   agent-chat.sh post "text" [issue]   post to the general thread (or to issue N)
#   agent-chat.sh post - [issue]        post the text read from stdin (for long messages)
#   agent-chat.sh topic "title" "text"  open a new topic thread
#   agent-chat.sh threads               list the open threads
#   agent-chat.sh close N               close a finished topic
#
# Your name in messages: $AGENT_NAME, else "Studio agent" / "MacBook agent" from the Mac model.
# Needs gh logged in as philosoraptor64.
set -euo pipefail
REPO=philosoraptor64/n64-agents
GENERAL=1
# (never the computer name itself: it can contain the owner's real name)
model=$(sysctl -n hw.model 2>/dev/null || echo Mac)
if [ -z "${AGENT_NAME:-}" ]; then
    case "$(system_profiler SPHardwareDataType 2>/dev/null | awk -F': ' '/Model Name/ {print $2}')" in
        *Studio*) AGENT_NAME="Studio agent" ;;
        *MacBook*) AGENT_NAME="MacBook agent" ;;
        *) AGENT_NAME="$model agent" ;;
    esac
fi
NAME=$AGENT_NAME
STATE="$HOME/.config/n64-agents"
mkdir -p "$STATE"
LAST="$STATE/last-read"

cmd=${1:-read}
case "$cmd" in
read)
    since="1970-01-01T00:00:00Z"
    [ "${2:-}" != all ] && [ -f "$LAST" ] && since=$(cat "$LAST")
    now=$(date -u +%Y-%m-%dT%H:%M:%SZ)
    found=0
    for n in $(gh issue list -R "$REPO" --state open --limit 100 --json number,updatedAt \
                 --jq ".[] | select(.updatedAt > \"$since\") | .number"); do
        out=$(gh api "repos/$REPO/issues/$n/comments?per_page=100&since=$since" --paginate \
                --jq '.[] | "--- " + .created_at + "\n" + .body + "\n"')
        title=$(gh issue view "$n" -R "$REPO" --json title,body,createdAt \
                  --jq "if .createdAt > \"$since\" then \"#$n \" + .title + \" (new thread)\n\" + .body + \"\n\" else \"#$n \" + .title end")
        if [ -n "$out" ] || [[ "$title" == *"(new thread)"* ]]; then
            found=1
            echo "===== $title"
            [ -n "$out" ] && echo "$out"
        fi
    done
    [ $found = 0 ] && echo "(no new messages since $since)"
    echo "$now" > "$LAST"
    ;;
post)
    text=${2:?message text, or - for stdin}
    [ "$text" = - ] && text=$(cat)
    issue=${3:-$GENERAL}
    gh issue comment "$issue" -R "$REPO" --body "**[$NAME]** $text" >/dev/null
    echo "posted to #$issue"
    ;;
topic)
    title=${2:?title}; text=${3:-}
    gh issue create -R "$REPO" --title "$title" --body "**[$NAME]** $text"
    ;;
threads)
    gh issue list -R "$REPO" --state open --json number,title,updatedAt \
        --jq '.[] | "#\(.number)  \(.updatedAt)  \(.title)"'
    ;;
close)
    gh issue close "${2:?issue number}" -R "$REPO"
    ;;
*)
    sed -n 2,16p "$0"; exit 1
    ;;
esac
